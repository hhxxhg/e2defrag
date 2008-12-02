/*
 * ext.c - extfs-specific functions and data for the Linux file system 
 * degragmenter. 
 * $Id: ext.c,v 1.3 1997/08/10 16:32:09 linux Exp $
 *
 * This file is responsible for managing those data structures dependent
 * on the extfs.  Specifically, it
 * + allocates space for the various disk maps stored in memory; 
 * + reads and writes superblock information; 
 * + reads the bad-block map into memory (bad_map); 
 * + reads the used-data-zones and used-inodes maps into memory
 *   (inode_map and d2n/n2d_map respectively); and, 
 * + once the defragmentation is complete, rewrites the new free-space
 *   map to disk.
 *
 * Copyright (C) 1992, 1993 Stephen Tweedie (sct@dcs.ed.ac.uk)
 *
 * Copyright (C) 1992 Remy Card (card@masi.ibp.fr)
 *
 * Copyright (C) 1991 Linus Torvalds (torvalds@kruuna.helsinki.fi)
 * 
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 *
 */

#include <config.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#include "defrag.h"

char const * program_name = "edefrag";
char const * fsck = "efsck";

int inode_table_offset = 0;

/* Read the superblock and inode table and reserve space for the
   remaining disk map tables */
void read_tables (void)
{
	if (debug)
		printf ("DEBUG: read_tables()\n");
	if (BLOCK_SIZE != nlseek (IN, BLOCK_SIZE, SEEK_SET))
		die ("seek failed");
	if (BLOCK_SIZE != nread (IN, super_block_buffer, BLOCK_SIZE))
		die ("unable to read super-block");
	if (MAGIC != EXT_SUPER_MAGIC)
		die ("bad magic number in super-block");
	if (ZONESIZE != 0 || BLOCK_SIZE != 1024)
		die("Only 1k blocks/zones supported");

	inode_table_offset = nlseek(IN, 0, SEEK_CUR);
	if (inode_table_offset < 0)
		die ("Unable to locate inode table");
	if (nlseek (IN, INODE_BUFFER_SIZE, SEEK_CUR) < 0)
		die ("Unable to read inodes");

	inode_average_map = malloc (INODES * sizeof(*inode_average_map));
	if (!inode_average_map)
		die ("Unable to allocate buffer for inode averages");
	memset (inode_average_map, 0, (INODES * sizeof(*inode_average_map)));

	inode_priority_map = malloc (INODES * sizeof(*inode_priority_map));
	if (!inode_priority_map)
		die ("Unable to allocate buffer for inode priorities");
	memset (inode_priority_map, 0, 
		(INODES * sizeof(*inode_priority_map)));

	inode_order_map = malloc (INODES * sizeof(*inode_order_map));
	if (!inode_order_map)
		die ("Unable to allocate buffer for inode order");
	memset (inode_order_map, 0, (INODES * sizeof(*inode_order_map)));

	inode_map = malloc ((INODES / 8) + 1);
	if (!inode_map)
		die ("Unable to allocate inodes bitmap\n");
	memset (inode_map, 0, (INODES / 8) + 1);

	d2n_map = malloc ((ZONES - FIRSTZONE) * sizeof (*d2n_map));
	if (!d2n_map)
		die ("Unable to allocate zones map\n");
	n2d_map = malloc ((ZONES - FIRSTZONE) * sizeof (*n2d_map));
	if (!n2d_map)
		die ("Unable to allocate zones map\n");

	fixed_map = malloc (((ZONES - FIRSTZONE) / 8) + 1);
	if (!fixed_map)
		die ("Unable to allocate bad zone bitmap\n");
	memset(fixed_map, 0, ((ZONES - FIRSTZONE) / 8) + 1);

	if (NORM_FIRSTZONE != FIRSTZONE)
		printf ("Warning: Firstzone != Norm_firstzone\n");
	first_zone = FIRSTZONE;
	zones = ZONES;
	block_size = BLOCK_SIZE;
	bad_block_inode = BAD_INO;
        voyer_mode = 0;
}

/* Write the superblock and inode table to disk */
void write_tables (void)
{
	if (debug)
		printf ("DEBUG: write_tables()\n");
	if (BLOCK_SIZE != nlseek (IN, BLOCK_SIZE, SEEK_SET))
		die ("seek failed in write_tables");
	if (BLOCK_SIZE != nwrite (IN, super_block_buffer, BLOCK_SIZE))
		die ("unable to write super-block");
}

/* Rewrite the free zone map on disk.  The defragmentation procedure
   will migrate all free blocks to the end of the disk partition, and so
   after defragmentation the free space map must be updated to reflect
   this. Free zones are determined by n2d_map, the macro zone_in_use(n)
   is defined in defrag.h for this purpose. The extfs stores the free
   zone map as a linked list of blocks, each block containing a counted
   list of other unused blocks. */
void salvage_free_zones (void)
{
	struct ext_free_block efb;
	Block blk;
	Block block_to_write = 0;
	Block next = 0;
	int count = 0;

	if (debug)
		printf ("DEBUG: salvage_zone_freelist()\n");
	if (verbose)
		printf ("Salvaging free zones list ...\n");
	blk = ZONES - 1;
	while (blk >= FIRSTZONE)
	{
		if (zone_in_use (blk))
		{
			blk--;
			continue;
		}
		if (!block_to_write)
		{
			block_to_write = blk;
			efb.count = 0;
		}
		else
			efb.free[efb.count++] = blk;
		if (efb.count == 254)
		{
			efb.next = next;
			count += efb.count + 1;
			write_current_block (block_to_write, (char *) &efb);
			next = block_to_write;
			block_to_write = 0;
		}
		blk --;
	}
	if (block_to_write)
	{
		efb.next = next;
		write_current_block (block_to_write, (char *) &efb);
		count += efb.count + 1;
		next = block_to_write;
	}
	FIRSTFREEBLOCK = next;
	FREEBLOCKSCOUNT = count;
	if (verbose)
	{
		printf ("Free blocks count : %d\n", count);
		printf ("First free zone : %ld\n", FIRSTFREEBLOCK);
	}
}

/* Read the map of used/unused data zones on disk.  The extfs stores a
   linked list of unused blocks.
   The map is held jointly in d2n_map and n2d_map, described in
   defrag.h.  These are initialised to the identity map (d2n(i) = n2d(i)
   = i), and then the free zone list is scanned, and all unused zones
   are marked as zero in both d2n_map and n2d_map. */
void init_zone_maps (void)
{
	int i;
	Block blknr;
	struct ext_free_block block;

	if (debug)
		printf ("DEBUG: init_zone_maps()\n");
	for (i = FIRSTZONE; i < ZONES; i++)
	{
		d2n(i) = i;
		n2d(i) = i;
	}
	
	/* Scan the free blocks list to unmark_zones */
	if (FIRSTFREEBLOCK)
	{
		blknr = FIRSTFREEBLOCK;
		while (blknr)
		{
			read_old_block (blknr, (char *) &block);
			if (blknr < FIRSTZONE || blknr > ZONES)
				die ("Free blocks list corrupted.");
			if (!zone_in_use (blknr))
				die ("Cycle in the free blocks list.");
			if (debug)
				printf ("%ld ", blknr);
			unmark_zone(blknr);
			for (i = 0; i < block.count; i++)
			{
				if (block.free[i] < FIRSTZONE ||
				    block.free[i] > ZONES)
				    die ("Free blocks list corrupted.");
				if (debug)
				    printf ("%ld ", block.free[i]);
				unmark_zone (block.free[i]);
			}
			blknr = block.next;
		}
	}
	changed = 0;
}

/* Read in the map of used/unused inodes.  Free inodes are held by the
   extfs as a linked list similar to the free zone map.  The macros
   mark_inode() and unmark_inode() are used to manipulate the free inode
   bitmap.  */
void init_inode_bitmap (void)
{
	int i;
	unsigned long ino;
	struct ext_free_inode * inode;

	if (debug)
		printf ("DEBUG: init_inode_bitmap()\n");
	for (i = 1; i <= INODES; i++)
		mark_inode(i);
	/* Scan the free inodes list to unmark inodes */
	if (FIRSTFREEINODE)
	{
		ino = FIRSTFREEINODE;
		inode = (struct ext_free_inode *) &inode_buffer;
		while (ino)
		{
			if (get_inode(ino))
				die ("Can't read free list.");
			if (!ino || ino > INODES)
				die ("Free inodes list corrupted.");
			if (!inode_in_use (ino))
				die ("Cycle in the free inodes list.");
			unmark_inode (ino);
			if (debug)
			    printf ("%ld ", ino);
			for (i = 0; i < inode->count; i++)
			{
				if (!inode->free[i] || inode->free[i] > INODES)
					die ("Free inodes list corrupted.");
				if (debug)
					printf ("%ld ", inode->free[i]);
				unmark_inode (inode->free[i]);
			}
			ino = inode->next;
		}
	}
	changed = 0;
}

int seek_to_inode(int i) {
	if (nlseek (IN,
		   inode_table_offset + (i-1)*(sizeof(struct d_inode)),
		   SEEK_SET)<0)
	{
		io_error ("Can't seek to inode table");
		return 1;
	}
        return 0;
}

void show_super_stats(void) 
{   
	if (show)
	{
		printf ("%ld inode%s\n", INODES, (INODES != 1) ? "s" : "");
		printf ("%ld block%s\n", ZONES, (ZONES != 1) ? "s" : "");
		printf ("Firstdatazone=%ld (%ld)\n", FIRSTZONE, NORM_FIRSTZONE);
		printf ("%ld free block%s\n", FREEBLOCKSCOUNT,
			(FREEBLOCKSCOUNT != 1) ? "s" : "");
		printf ("Firstfreeblock=%ld\n", FIRSTFREEBLOCK);
		printf ("%ld free inode%s\n", FREEINODESCOUNT,
			(FREEINODESCOUNT != 1) ? "s" : "");
		printf ("Firstfreeinode=%ld\n", FIRSTFREEINODE);
		printf ("Zonesize=%d\n", BLOCK_SIZE << ZONESIZE);
		printf ("Maxsize=%ld\n\n", MAXSIZE);
	}
}

void show_reserved_blocks(void) 
{}
