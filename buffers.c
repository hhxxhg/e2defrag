/*
 * buffers.c - buffer management for the Linux file system degragmenter.
 * $Id: buffers.c,v 1.6 1998/01/25 13:51:51 linux Exp $
 *
 * Copyright (C) 1992, 1993, 1997 Stephen Tweedie (sct@dcs.ed.ac.uk)
 * 
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 *
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/fcntl.h>
#include <errno.h>
#include "defrag.h"
#include "map.h"

#define buffer(i) (&pool[i])

/* The buffer pool is a unified buffer space containing both the
   pending pool and the rescue pool.  The pending pool holds buffers
   waiting to be written to disk as part of the sequential write of 
   optimised zones; the rescue pool holds the contents of zones about
   to be overwritten by blocks from the write pool.

   The select set is an arbitrary subset of the whole buffer pool.
   This feature is used in various places to select a group of buffers
   for reading or writing. 

   The buffer pool and hash table will not necessarily be totally in
   synch with the relocation maps d2n_map and n2d_map.  It is possible
   for a buffer to exist for a block which, according to the
   relocation maps, is still on disc.  This is because the relocation
   maps are NOT modified by the creation or deletion of pool buffers,
   but only by the actual reading or writing of those pool buffers.
   (The basis of the defragmenter's optimisation is the deferring as
   long as possible of reads and writes, and the sorting of bulk
   reads/writes to improve performance.)
*/

static char tmps[128];
int pool_size;
Buffer *pool;
static Buffer **select_set;
static int select_set_size;
static int free_buffers, count_output_buffers, count_rescue_buffers;

unsigned count_buffer_writes = 0, count_buffer_reads = 0;
time_t transfer_start_time;
int count_write_groups = 0, count_read_groups = 0;
int count_buffer_migrates = 0, count_buffer_forces = 0;
int count_buffer_read_aheads = 0;
int last_block = -1;
int queue_count;
int queue_block_count;
char queue_direction;
#define QUEUE_MAX 1024
struct iovec queue[QUEUE_MAX];
Block dest_cursor;
int next_free_buffer;

/* We will hash buffered blocks on the least significant bits of the
   block's dest_zone */
#define HASH_SIZE 16384
static Buffer * (hash[HASH_SIZE]) = {0};
#define hash_list(zone) (hash[((unsigned) (zone)) % (HASH_SIZE)])

void io_error(const char *message)
{
	if (die_on_io_error) 
		fatal_error(message);
	stat_line (message);
	io_errors++;
	return;
}

/* First of all, the primitive buffer management functions: allocation
   and freeing of buffer blocks. */

/* Set up the buffer tables and clear the hash table.  This must be
   called after the fs-dependent code has been initialised (typically
   in read_tables() ) so that the block size variables have been
   correctly initialised. */
void init_buffer_tables()
{
	int i;
	char *bp;
	if (debug)
		printf ("DEBUG: init_buffer_tables()\n");
	
	memset( hash, 0, HASH_SIZE * sizeof(*hash));
	if (pool_size == 0)
	{
		/* auto detect pool size: use half of free mem */
		FILE *info;
		int memfree, membuffers, memcache;

		info = fopen ("/proc/meminfo", "r");
		if (!info)
			die ("Unable to open /proc/meminfo.");
		i = fscanf (info, "MemTotal: %*d kB MemFree: %d kB Buffers: %d kB Cached: %d kB",
			    &memfree, &membuffers, &memcache);
		fclose (info);
		if (i != 3)
			die ("Error parsing /proc/meminfo.");
		memfree += membuffers;
		memfree += memcache;
		pool_size = memfree / (block_size / 512);
		if (verbose)
			stat_line ("Auto detected pool size of %d buffers", pool_size);
	}
	pool = (Buffer *) malloc (pool_size * sizeof(Buffer));
	if (!pool)
		die ("Unable to allocate buffer pool.");
	memset (pool, 0, pool_size * sizeof(Buffer));
	bp = malloc ((pool_size * block_size) + 4096);
	bp = (char *)(((unsigned long)bp + 0xFFFUL) & ~0xFFFUL);
	if (!bp)
	  die ("Unable to allocate buffer space.");
	select_set = (Buffer **) malloc (pool_size *
					 sizeof(*select_set));
	if (!select_set)
		die ("Unable to allocate pool select set");
	select_set_size = 0;
	/* Set up the free buffer list */
	for (i=0; i<pool_size-1; i++)
	{
		buffer(i)->datap = bp;
		bp += block_size;
	}
	free_buffers = pool_size;
	count_output_buffers = count_rescue_buffers = 0;
}

/* Lookup a block in the hash table.  Returns a pointer to the entry
   in the hash list (ie. doubly indirected), or zero. */
static Buffer ** hash_lookup (Block zone)
{
	Buffer ** b;
	b = &(hash_list(zone));
	while (*b)
	{
		if ((*b)->dest_zone == zone)
			return b;
		b = &((*b)->next);
	}
	return 0;
}

/* Allocate an unused buffer from the pool. */
static Buffer * allocate_buffer (Block zone, BufferType btype)
{
	Buffer * b;
	if (next_free_buffer >= select_set_size)
		die ("No free buffers");
	assert (free_buffers);
	assert (!(hash_lookup(zone)));

	/* Remove a buffer from the free list */
	b = select_set[next_free_buffer++];
	assert (!b->in_use);
	b->in_use = 1;
	
	/* Set up the buffer fields */
	b->dest_zone = zone;
	b->btype = btype;
	b->full = 0;

	/* Update buffer counts */
	free_buffers--;
	switch (btype)
	{
	case OUTPUT:
		count_output_buffers++;
		break;
	case RESCUE:
		count_rescue_buffers++;
		break;
	}

	/* Link the buffer into the hash table */
	b->next = hash_list(zone);
	hash_list(zone) = b;

	assert ((count_rescue_buffers + count_output_buffers +
		 free_buffers) == pool_size);
	return b;
}

/* Free up a buffer from the buffer pool.  Manage the free buffer list
   and hash table appropriately. */
static void free_buffer (Buffer *b)
{
	Buffer **p;
	if (debug)
		printf ("DEBUG: free_buffer (%lu)\n", 
			(unsigned long) b->dest_zone);
	assert (b->in_use);
	b->in_use = 0;

	/* Unlink this buffer from the hash table */
	p = hash_lookup (b->dest_zone);
	assert (p);
	assert ((*p) == b);
	*p = b->next;

	/* Update buffer counts */
	free_buffers++;
	switch (b->btype)
	{
	case OUTPUT:
	case FORCE:
		count_output_buffers--;
		break;
	case RESCUE:
		count_rescue_buffers--;
		break;
	}
}

/* Set a buffer's type - used to migrate blocks from the RESCUE to the 
   OUTPUT pool. */
static void set_buffer_type (Buffer *b, BufferType btype)
{
	if (debug)
		printf ("DEBUG: set_buffer_type (%lu:%d, %d)\n",
			(unsigned long) b->dest_zone, b->btype, btype);
	if (b->btype == btype)
		return;
	switch (b->btype)
	{
	case OUTPUT:
		count_output_buffers--;
		break;
	case RESCUE:
		count_rescue_buffers--;
		break;
	}
	b->btype = btype;
	switch (btype)
	{
	case OUTPUT:
		count_output_buffers++;
		break;
	case RESCUE:
		count_rescue_buffers++;
		break;
	case FORCE:
		count_output_buffers++;
		count_buffer_forces++;
		break;
	}
	assert ((count_rescue_buffers + count_output_buffers +
		 free_buffers) == pool_size);
}

		
/* Select a group of buffers based on an arbitrary selection predicate */
static void select_buffers (int (*fn) (const Buffer *))
{
	int i;
	if (debug)
		printf ("DEBUG: select_buffers()\n");
	
	select_set_size = 0;
	for (i=0; i<pool_size; i++)
	{
		if (buffer(i)->in_use && fn(buffer(i)))
			select_set[select_set_size++] = buffer(i);
	}
	if (debug)
		printf ("DEBUG: selected %d buffers\n", select_set_size);
}

static void select_free_buffers ()
{
	int i;
	
	select_set_size = 0;
	for (i=0; i<pool_size; i++)
	{
		if (!buffer(i)->in_use)
			select_set[select_set_size++] = buffer(i);
	}
	next_free_buffer = 0;
}

/* Compare two buffers based on their dest_zone fields, for use by
   the stdlib qsort() function */
static int compare_buffer_zones(const void *a, const void *b)
{
	Block azone, bzone;
	azone = (*((Buffer const * const *) a))->dest_zone;
	bzone = (*((Buffer const * const *) b))->dest_zone;
	
	if (azone < bzone)
		return -1;
	if (azone == bzone)
		return 0;
	return 1;
}

/* Compare two buffers again, this time based on their source zone */
static int compare_buffer_zones_for_read (const void *a, const void *b)
{
	Block azone, bzone;
	azone = map_reverse_get((*((Buffer const * const *) a))->dest_zone);
	bzone = map_reverse_get((*((Buffer const * const *) b))->dest_zone);
	
	if (azone < bzone)
		return -1;
	if (azone == bzone)
		return 0;
	return 1;
}

/* Sort the current select_set based on buffer dest_zone.  Sorting
   buffers in this manner before a read or write will significantly
   improve i/o performance, but is not essential for correct running
   of the program. */
static void sort_select_set (void)
{
	if (debug)
		printf ("DEBUG: sort_select_set()\n");
	qsort (select_set, select_set_size, sizeof (*select_set), 
	       compare_buffer_zones);
}

/* Perform a similar sort, but sort on source zones for an order
   suitable for reading the select set. */
static void sort_select_set_for_read (void)
{
	if (debug)
		printf ("DEBUG: sort_select_set_for_read()\n");
	qsort (select_set, select_set_size, sizeof (*select_set), 
	       compare_buffer_zones_for_read);
}

/*
 * check_zone_nr checks to see that *nr is a valid zone nr. It dies if
 * it isn't.
 */
void check_zone_nr (Block nr)
{
	if (debug)
		printf ("DEBUG: check_zone_nr (&%ld)\n", (long) nr);
	if (nr < first_zone)
		printf ("Zone nr %ld < first_zone.\n", (long) nr);
	else if (nr >= zones) {
#if FS_IS_ext2
	   if (nr == EXT2_COMPRESSED_BLKADDR)
		printf ("Unexpectedly found compression zone nr.\n");
	   else
#endif
		printf ("Zone nr %lu > zones.\n", (unsigned long) nr);
	} else
		return;
	die ("Invalid zone number");
}

/*
 * read_current_block reads block nnr into the buffer at addr.
 */
void read_current_block (Block nnr, char * addr)
{
	loff_t offset;

	assert( (block_size > 0) && (block_size <= MAX_BLOCK_SIZE)
		&& ((block_size & (block_size - 1)) == 0));

	if (debug)
		printf ("DEBUG: read_block (&%ld, %p)\n", 
			(long) nnr, addr);
	if (!nnr)
		return;
	check_zone_nr(nnr);

	offset = (loff_t) block_size * nnr;
	
	if (offset != nlseek (IN, offset, SEEK_SET))
	{
		io_error ("seek failed in read_block");
		return;
	}
	if(nread( IN, addr, block_size) != (ssize_t) block_size)
	{
		sprintf (tmps,
			 "Read error: bad block %ld in read_block",
			 (long) nnr);
		io_error (tmps);
		/* Error in read - just set it to all zeros.  There's 
		   nothing better we can do. */
		memset (addr, 0, block_size);
	}
}

void queue_flush()
{
  ssize_t read;

  if (queue_count == 0)
    return;
  if (queue_direction) {
    read = writev (IN, queue, queue_count);
    if (read != queue_block_count * block_size) {
      sprintf (tmps, "writev failed: %s\n", strerror (errno));
      io_error (tmps);
    }
  }
  else {
    read = readv (IN, queue, queue_count);
    if (read != queue_block_count * block_size) {
      sprintf (tmps, "readv failed: %s\n", strerror (errno));
      io_error (tmps);
    }
  }
  queue_count = 0;
  queue_block_count = 0;
  last_block = -1;
}

void queue_read_current_block (Block nnr, char * addr)
{
  loff_t offset;

  assert( (block_size > 0) && (block_size <= MAX_BLOCK_SIZE)
	  && ((block_size & (block_size - 1)) == 0));
  assert (queue_direction == 0);
  if (debug)
    printf ("DEBUG: read_block (&%ld, %p)\n", 
	    (long) nnr, addr);
  if (!nnr)
    return;
  check_zone_nr(nnr);

  offset = (loff_t) block_size * nnr;
  if (last_block+1 != nnr) {
    queue_flush();
    if (offset != nlseek (IN, offset, SEEK_SET))
	{
	  io_error ("seek failed in read_block");
	  return;
	}
  }
  last_block = nnr;
  queue_block_count++;
  if (queue_count &&
      queue[queue_count-1].iov_base + queue[queue_count-1].iov_len == addr)
    {
      /* append to previous entry */
      queue[queue_count-1].iov_len += block_size;
      return;
    }
  queue[queue_count].iov_base = addr;
  queue[queue_count].iov_len = block_size;
  if (++queue_count == QUEUE_MAX)
    queue_flush();
}

void queue_write_current_block (Block nnr, char * addr)
{
  loff_t offset;

  assert( (block_size > 0) && (block_size <= MAX_BLOCK_SIZE)
	  && ((block_size & (block_size - 1)) == 0));
  assert (queue_direction == 1);
  
  if (debug)
    printf ("DEBUG: write_block (&%ld, %p)\n", 
	    (long) nnr, addr);
  if (!nnr)
    return;
  check_zone_nr(nnr);

  offset = (loff_t) block_size * nnr;
  if (last_block+1 != nnr) {
    queue_flush();
    if (offset != nlseek (IN, offset, SEEK_SET))
	{
	  io_error ("seek failed in write_block");
	  return;
	}
  }
  last_block = nnr;
  queue_block_count++;
  if (queue_count &&
      queue[queue_count-1].iov_base + queue[queue_count-1].iov_len == addr)
    {
      /* append to previous entry */
      queue[queue_count-1].iov_len += block_size;
      return;
    }
  queue[queue_count].iov_base = addr;
  queue[queue_count].iov_len = block_size;
  if (++queue_count == QUEUE_MAX)
    queue_flush();
}

/*
 * write_current_block writes block nr to disk.
 */
void write_current_block (Block nnr, char * addr)
{
	loff_t offset;

	if (debug)
		printf ("DEBUG: write_block(%ld, %p)\n", 
			(long) nnr, addr);
	check_zone_nr(nnr);

	offset = (loff_t) block_size * nnr;
	
	if (offset != nlseek (IN, offset, SEEK_SET))
	{
		io_error ("seek failed in write_block");
		return;
	}
	if (readonly)
		return;

        if(nwrite( IN, addr, block_size) != (ssize_t) block_size)
	{
		/* No point telling the user where the error occurred -
		   the kernel's write-behind defeats that. */
		io_error ("Write error: bad block in write_block.");
		return;
	}
	if (blocks_until_sync++ >= SYNC_PERIOD)
	{
		sync();
		blocks_until_sync = 0;
	}
}

/* read/write_old_block function as read_block and write_block, but using
   the zone map to follow blocks which may have been swapped to make room for
   optimised zones. */
void read_old_block (Block onr, char *addr)
{
	check_zone_nr(onr);
	read_current_block (map_forward_get(onr), addr);
}

void write_old_block (Block onr, char *addr)
{
	check_zone_nr(onr);
	write_current_block (map_forward_get(onr), addr);
}


/* Read/write _Buffer_s.  The read/write_old/new_block routines work
   on arbitrary blocks and arbitrary memory locations; the Buffer
   read/write routines, on the other hand, interact fully with the
   zone relocation maps and Buffer data structures from the buffer
   pool. */

void read_buffer_data (Buffer *b)
{
	Block source;
	if (debug)
		printf ("DEBUG: read_buffer_data (%lu)\n",
			(long) b->dest_zone);
	assert (b->in_use);
	if (b->full)
		return;
	source = map_reverse_get (b->dest_zone);
	/* Don't bother reading here if we are in readonly mode; there
	   will be no need to write it back at any time. */
	if (!readonly)
		queue_read_current_block (source, b->datap);
	b->full = 1;
	count_buffer_reads++;
	return;
}

void write_buffer_data_at (Buffer *b, Block dest)
{
	if (debug)
		printf ("DEBUG: write_buffer_data_at (%lu, %lu)\n", 
			(unsigned long) b->dest_zone, 
			(unsigned long) dest);
	assert (b->in_use & b->full);
	if (!readonly)
		queue_write_current_block (dest, b->datap);
	if (b->btype != FORCE) {
		assert (b->btype == OUTPUT);
		count_buffer_writes++;
	} else count_buffer_reads--; /* will count as a read again later */
}

void write_buffer_data (Buffer *b)
{
	write_buffer_data_at (b, b->dest_zone);
}


/***********************************************************************
   The disk relocation routines: the working core of the defragmenter.
   These routines are responsible for implementing the disk block
   relocation defined by the forward and reverse relocation maps
   d2n_map and n2d_map.
 ***********************************************************************/
	
static void read_select_set (void)
{
	int i;
	if (!select_set_size)
		return;
	sort_select_set_for_read();
	if (verbose>1)
		stat_line ("Reading %d blocks...", select_set_size);
	/* effic: Ought to clue the O/S what we're going to be reading
           (asynchronous reads). */
        if (voyer_mode) {
	    /* the locations where the reading
	       takes place are n2d(dest_zone), not dest_zone. */
            for (i=0; i < select_set_size; i++)    
	      set_attr(map_reverse_get(select_set[i]->dest_zone),AT_READ);
            update_display();
        }
	queue_direction = 0;
	for (i=0; i < select_set_size; i++)
	{
		assert (!select_set[i]->full);
		read_buffer_data (select_set[i]);
	}
	if (!readonly)
		queue_flush();
        clear_attr(AT_READ);
	count_read_groups++;
}

static void write_select_set (void)
{
	int i;
	sort_select_set();
	if (!select_set_size)
		return;
	if (verbose>1)
		stat_line ("Writing %d blocks...", select_set_size);
        if (voyer_mode) {
            for (i=0; i < select_set_size; i++)    /* screen */
                set_attr(select_set[i]->dest_zone,AT_WRITE);
            update_display();
        }
	queue_direction = 1;
	for (i=0; i < select_set_size; i++)
	{
		assert (select_set[i]->in_use &&
			select_set[i]->full);
		write_buffer_data(select_set[i]);
	}
	if (!readonly)
		queue_flush();
        if (voyer_mode)
                clear_attr(AT_WRITE);
	count_write_groups++;
}

static void free_select_set(void)
{
	int i;
	for (i=0; i < select_set_size; i++)
		free_buffer (select_set[i]);
	select_set_size = 0;
}

/* A few useful buffer selection predicates... */
static int output_buffer_p (const Buffer *b)
{
	return (b->btype == OUTPUT || b->btype == FORCE);
}

static int empty_buffer_p (const Buffer *b)
{
	return (!b->full);
}

static int rescue_buffer_p (const Buffer *b)
{
	return (b->btype == RESCUE);
}

static int true (const Buffer *b)
{
	UNUSED(b);
	return 1;
}


/* Routines for reading and flushing data */
static void read_all_buffers(void)
{
	/* Select and sort all (non-empty) buffers for reading */
	select_buffers (empty_buffer_p);
	read_select_set();
}	

static void flush_output_pool(void)
{
	read_all_buffers ();
	select_buffers (output_buffer_p);
	if (!select_set_size)
		return;
	if (verbose>1)
		stat_line ("Saving: ");
	write_select_set();
	free_select_set();
}

/* Here we employ various methods for getting rid of some of the
   buffers in the buffer pool.  There are three methods used, in
   order of preference:
   flush output buffers: Buffers waiting to be sequentially written to
		disk are written now.
   If that fails to free more than 25% of buffers:
   flush rescue buffers: Rescue buffers whose destination zones are
   		empty (ie. already moved into the output pool and
		hence to disk) are flushed, by migrating them
		immediately to the output pool.
   If that fails to free more than 20% of the buffers, drastic action
   is necessary:
   force rescue buffers: Rescue buffers are sorted, and a number of
		rescue buffers destined for later disk zones are
		written to free space at the end of the disk (NOT to
		their ultimate destinations, though).  We choose later
		rescue buffers to flush in preference to earlier
		buffers, in anticipation of soon being able to migrate
		early rescued blocks to the output pool. */
static void get_some_buffer_space(void)
{
	int i, count = 0;
	Block dest;
	
	flush_output_pool();
	if ((free_buffers * 4) > pool_size)
		return;

	/* OK, try migrating rescue buffers to the output pool */
	for (i=0; i<pool_size; i++)
	{
		if (buffer(i)->in_use &&
		    buffer(i)->btype == RESCUE &&
		    map_forward_get(buffer(i)->dest_zone) < dest_cursor)
		{
			set_buffer_type(buffer(i), OUTPUT);
			count++;
			count_buffer_migrates++;
		}
	}
	if (verbose>1)
		stat_line ("Migrated %d rescued blocks%s", count,
			count ? ": " : ".");
	flush_output_pool();
	if ((free_buffers * 5) > pool_size)
		return;
	
	/* Serious trouble - more than 80% of the pool is occupied by 
	   unsaveable rescue buffers.  We'll have to force some of 
	   them to disk.  (The algorithm I'm using tries hard to avoid
	   this, since it is the only occasion where a disk block may
	   have to be be moved more than once during relocation.) */
	select_buffers (true);
	sort_select_set ();
	assert (select_set_size + free_buffers == pool_size);
	/* Select the top 25% of rescue buffers for forcing (this is 
	   an entirely arbitrary number; in fact, most of the numbers 
	   in this function could probably be tweaked to tune 
	   performance, but these seem to work OK. */
	dest = zones-1; count = 0;
	if (verbose>1)
		stat_line ("Pool too full - forcing buffers...");
	for (i = select_set_size-1; i >= ((select_set_size*3)/4); i--)
	{
		Buffer **p;
		while (map_forward_get (dest))
			dest--;
		assert (select_set[i]->in_use & select_set[i]->full);
		if (debug)
			printf ("Forcing buffer from %d to %d\n", select_set[i]->dest_zone, dest);
		/* Unlink buffer from the hash table */
		p = hash_lookup (select_set[i]->dest_zone);
		assert (p);
		assert ((*p) == select_set[i]);
		*p = select_set[i]->next;
		/* Link the buffer into the hash table with new dest */
		select_set[i]->next = hash_list(dest);
		hash_list(dest) = select_set[i];
		map_forward_set (dest, select_set[i]->dest_zone);
		select_set[i]->dest_zone = dest;
		assert (select_set[i]->btype == RESCUE);
		set_buffer_type (select_set[i], FORCE);
		count++;
	}
	if (verbose>1)
		stat_line (" %d buffers recovered.", count);
	select_set_size = 0;
	flush_output_pool();
}

void remap_disk_blocks (void)
{
	Block source, dest2;
	Buffer **p;
	struct map_extent *e;
	Block blocks_to_relocate = 0;

	dest_cursor = first_zone;
	assert( (block_size & 127) == 0);
	/* Relevance: the `block_size >> 7' below. */

	if (debug)
		printf ("DEBUG: remap_disk_blocks()\n");
	/* count blocks to relocate */
	e = map_reverse_first ();
	while (e) {
		if (e->old != e->new)
			blocks_to_relocate += (e->count + 1);
		e = map_reverse_next (e);
	}
	if (verbose)
		stat_line ("Relocating %lu MB - this could take a while.",
			   (((unsigned long) blocks_to_relocate >> 10)
			    * (block_size >> 7))
			   >> (20 - 10 - 7));
       	assert (fcntl (IN, F_SETFL, O_DIRECT)==0);
	transfer_start_time = time(NULL);
	/* Walk through each disk block sequentially, rescuing 
	   previous contents and reading the new contents into the 
	   output buffer. */
	e = map_reverse_first ();
	dest_cursor = e->new;
	select_free_buffers ();
	do
	{
		/* move to next extent if the dest cursor has moved
		   past the end of this one, or if this extent does
		   not move ( src == dest or old == 0 ) */
		while (dest_cursor > e->new + e->count ||
		       e->new == e->old ||
		       e->old == 0)
		{
			e = map_reverse_next (e);
			if (!e)
				goto done;
			dest_cursor = e->new;
		}
		/* Don't try to save stuff to disk until we are
		   running out of free buffers. */
		if (free_buffers < 4)
		{
			get_some_buffer_space ();
			select_free_buffers ();
			/* forces may have changed the map, invalidating the
			   current extent.  Look up new one */
			e = map_reverse_get_extent_next (dest_cursor);
			if (!e)
				goto done;
			while (e->new == e->old)
			{
				e = map_reverse_next (e);
				if (!e)
					goto done;
			}
			/* move to new extent */
			if (dest_cursor < e->new)
				dest_cursor = e->new;
			if (verbose)
			{
				unsigned long mb = (((unsigned long) count_buffer_writes >> 10)
						    * (block_size >> 7))
					>> (20 - 10 - 7);
				if (!readonly)
					stat_line( "Relocated : %lu MB (%u%%) %.1f MB/s",
						   mb,
						   (count_buffer_writes * 100) / blocks_to_relocate,
						   (float)mb / (time(NULL) - transfer_start_time));
				else stat_line( "Relocated : %lu MB (%u%%)",
						mb,
						(count_buffer_writes * 100) / blocks_to_relocate);

				/* The funny shifting order is just to avoid overflow. */
			}
		}
		assert (free_buffers >= 4);
		
		/* Use the reverse relocation map to obtain the block 
		   which should go in this space */
		source = dest_cursor - e->new + e->old;
		/* We may have already read the source block into the 
		   rescue pool; look for the block (indexed by dest) 
		   in the hash table */
		p = hash_lookup (dest_cursor);
		if (p)
		{
			/* Yes, we already have the block so make it 
			   an OUTPUT buffer (it must currently be a 
			   RESCUE buffer). */
			assert ((*p)->btype == RESCUE);
			set_buffer_type (*p, OUTPUT);
			count_buffer_read_aheads++;
		}
		else
		{
			/* if the source is to the left of the cursor,
			   and it had an incoming block, we already
			   rescued it and must have migrated, so don't
			   transfer it a second time */
			Block b = map_reverse_get (source);
			if (source < dest_cursor && b != 0 && b != -1)
				continue;
			allocate_buffer (dest_cursor, OUTPUT);
		}
		
		/* Rescue the block about to be overwritten.  All 
		   buffers are referred to by their destination zone 
		   (according to the relocation map), NOT the zone 
		   that block is currently residing in.  So, work out 
		   the final destination of the block currently 
		   residing in the destination zone by looking up the 
		   forward relocation map. */
		dest2 = map_forward_get (dest_cursor);
		/* If dest_cursor is already past this block's dest,
		   then we have already read it so it is safe to overwrite */
		if (dest2 < dest_cursor)
			continue;
		/* Check to see if we have already read this block */
		p = hash_lookup (dest2);
		if (p)
		{
			/* We have, so no need to read it again */
			continue;
		}
		/* Read the block into the rescue pool */
		allocate_buffer (dest2, RESCUE);
	} while (++dest_cursor < zones);
done:
	/* We have got to the end, so flush any remaining buffers. */
	flush_output_pool ();
	assert (!count_output_buffers);
	assert (!count_rescue_buffers);
	assert (free_buffers == pool_size);
       	assert (fcntl (IN, F_SETFL, 0)==0);
}
