/*
 * defrag.c - the Linux file system degragmenter.
 * $Id: defrag.c,v 1.4 1997/08/17 14:23:57 linux Exp $
 *
 * changes are Copyleft  (C) 1997 Ulrich Habel (espero@b31.hadiko.de)
 *
 * changes are Copyleft  (C) 1997 Anthony Tong (antoine@eci.com)
 *
 * Copyright (C) 1992, 1993, 1997 Stephen Tweedie (sct@dcs.ed.ac.uk)
 *
 * Copyright (C) 1992 Remy Card (card@masi.ibp.fr) 
 *
 * Copyright (C) 1991 Linus Torvalds (torvalds@kruuna.helsinki.fi)
 * 
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 *
 * Based on efsck 0.9 alpha by Remy Card and Linus Torvalds.
 * 
 */

#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

#include "defrag.h"
#include "version.h"

#ifndef NODEBUG
int debug = 0;
#endif

char const *RCSID = "$Id: defrag.c,v 1.4 1997/08/17 14:23:57 linux Exp $";

int die_on_io_error = 1;
int io_errors = 0;

char * device_name = NULL;
int IN;
int verbose = 0; 
int show = 0;
static int show_version = 0; 

int badblocks = 0;
int readonly = 0;
int changed = 0;
int blocks_until_sync = 0;
Block bad_block_inode = 0, user_bad_inode = 0, journal_inode;

#ifndef FS_IS_ext2
Block next_block_to_fill = 0;
#endif
Block first_zone = 0;
unsigned int zones = 0, block_size = 0;

/* Global buffer variables */
char inode_buffer[1024];
int current_inode = 0;
char super_block_buffer[SUPERBLOCK_SIZE];
unsigned char * inode_map = NULL;
Block *inode_average_map = NULL;
signed char *inode_priority_map = NULL;
int *inode_order_map = NULL;
char * fixed_map = NULL;
Block *n2d_map = NULL, *d2n_map = NULL;

/* Local variables */
static float sum_inode_zones;
static int count_inode_blocks;
static int used_inodes = 0;
static FILE *priority_file = 0;

/* Write back the current inode */
void put_inode(void)
{
	if (!current_inode || readonly)
		return;

        if (seek_to_inode(current_inode))
            return;

        if (nwrite (IN, &inode_buffer, EXT2_INODE_SIZE(&Super))!= 
            EXT2_INODE_SIZE(&Super))
	{
		io_error ("Can't write inode");
		return;
	}
	return;
}
/* Load in a given inode */
int get_inode(int i)
{    
	if (current_inode == i)
		return 0;
	current_inode = 0;
	memset (&inode_buffer, 0, EXT2_INODE_SIZE(&Super));
        if (seek_to_inode(i))
                return 1;
	if (nread(IN, &inode_buffer, EXT2_INODE_SIZE(&Super)) 
                 != EXT2_INODE_SIZE(&Super))
	{
		io_error ("Can't read inode");
		return 1;
	}
	current_inode = i;
	return 0;
}
   

/* optimise_zone : find the next destination block for the optimised data,
   and swap the zone with the old contents of that block if necessary.
   Only modify the relocation maps and (if necessary) the zone
   pointer; don't move any data just yet. */
static void optimise_zone (Block *znr)
{
	Block ox, oy, nx, ny;
#if FS_IS_xia        
        /* In the Xia FS the i_blocks parameter (size of inode's 
         * data in 512-bytes blocks) is stored in the high bytes 
         * of the first three i_zone elements.
         */
        Block i_blocks = (*znr) & 0xFF000000;
        *znr &= 0x00FFFFFF;
#endif        
	if (debug)
		printf ("DEBUG: optimise_zone (&%lu)\n", (ulong) *znr);
	changed = 1;

	ox = *znr;
	check_zone_nr(ox);
        set_attr(ox,AT_SELECTED);

	/* Don't attempt to relocate a fixed (probably bad) block! */
	if (zone_is_fixed(*znr)) {
#if FS_IS_xia
                *znr |= i_blocks;
#endif                
		return;
        }        
#if FS_IS_ext2
	ny = choose_block(current_inode);
#else
	while (zone_is_fixed(next_block_to_fill)) {
		next_block_to_fill++;
        }        
	ny = next_block_to_fill++;
#endif  
	check_zone_nr(ny);

	nx = d2n(ox);
	oy = n2d(ny);

	/* Update the zone maps. */
	d2n(ox) = ny;
	if (oy)
		d2n(oy) = nx;
	n2d(nx) = oy;
	n2d(ny) = ox;
	if (!readonly) {
		*znr = ny;
#if FS_IS_xia
                *znr |= i_blocks;
#endif                
        }        
}

#ifndef NODEBUG
static void validate_relocation_maps(void)
{
	Block i;

	for (i=first_zone; i < zones; i++)
	{
		if (n2d(i))
			assert (d2n(n2d(i)) == i);
		if (d2n(i))
			assert (n2d(d2n(i)) == i);
	}
}
#endif

/* walk_[ind_/dind_/tind_]zone - perform a tree walk over inode data zones.
   return true iff the block is relocated.
   Depending on the mode:
   mode == WZ_FIXED_BLOCKS: scan bad block inode and other unusual inodes 
                            to create map of unmovable blocks.
   mode == WZ_SCAN:	  scan inode to determine average occupied block.
   mode == WZ_REMAP_IND:  optimise inode indirection zones
   mode == WZ_REMAP_DATA: optimise inode data zones - by this time the inode
			  indirection zones will have been modified to
			  point to the new zone locations, although
			  the zones will not have moved; hence,
			  lookups through the indirection blocks will
			  have to be passed through the n2d
			  translation.
*/                          
/* Alexey Vovenko: combined WZ_REMAP_IND and WZ_REMAP_DATA into one
 * single WZ_REMAP. In the current version indirection blocks are allocated
 * in the middle of a file, rather than in the head. It is supposed to
 * be a better allocation policy.
 */
/*
   Note - there is NEVER any need to perform that n2d lookup if we are
   in readonly mode, since in that case the zone number changes never
   get written to disk.
*/

static inline void update_inode_average (Block n)
{
  if( (current_inode / Super.s_inodes_per_group) !=
      (n / Super.s_blocks_per_group) ||
      sum_inode_zones == INFINITY )
    {
      /* if blocks have spilled outside the native block group
	 during the last defrag then force the average
	 to -1 to push the inode to the right in the optimisation
	 order, otherwise idnoes will dance back and forth on
	 susequent defrags since their positions will reverse if
	 their non native blocks are allocated in earlier groups */
      sum_inode_zones = INFINITY;
      count_inode_blocks = 1;
    }
  else {
    sum_inode_zones += n;
    count_inode_blocks++;
  }
}

#if FS_IS_ext2
# define HOLE_BLKADDR(_b) ((_b) == 0 || (_b) == EXT2_COMPRESSED_BLKADDR)
#else
# define HOLE_BLKADDR(_b) ((_b) == 0)
#endif


static int walk_zone (Block * znr, enum walk_zone_mode mode)
{       
        Block bn = *znr;

#if FS_IS_xia
           /* In the Xia FS the i_blocks parameter (size of inode's 
            * data in 512-bytes blocks) is stored in the high bytes 
            * of the first three i_zone elements.
            */
        bn &= 0x00FFFFFF;
#endif                

	if (HOLE_BLKADDR(bn))
		return 0;
	if (debug)
		printf ("DEBUG: walk_zone(&%lu, %d)\n", (ulong) bn, mode);
                
	check_zone_nr(bn);
#if FS_IS_ext2
        update_group_population(bn,mode,current_inode);
#endif

	if (mode == WZ_SCAN) {
		update_inode_average(bn);
                set_attr(bn,AT_DATA);
        }
        	
	switch (mode)
	{
	case WZ_FIXED_BLOCKS:
		mark_fixed(bn);
                set_attr(bn,AT_BAD);
                badblocks++;
	case WZ_SCAN:
                break;
        case WZ_REMAP:
		optimise_zone(znr);
		return 1;
	}
	return 0;
}

static int walk_zone_ind (Block * znr, enum walk_zone_mode mode)
{
	static char blk[MAX_BLOCK_SIZE];
	int i, result = 0, blk_chg = 0;

	if (!*znr)
		return 0;
	if (debug)
		printf ("DEBUG: walk_zone_ind (&%lu, %d)\n",
			(ulong) *znr, mode);
	check_zone_nr (*znr);

#if FS_IS_ext2
        update_group_population(*znr,mode,current_inode);
#endif
        set_attr(*znr,AT_DATA);
	if (mode == WZ_SCAN)
		update_inode_average(*znr);
	if( mode == WZ_FIXED_BLOCKS )
	  {
		mark_fixed(*znr);
		set_attr(*znr,AT_BAD);
		badblocks++;
	  }	
	read_current_block(*znr, blk);

        if (mode == WZ_REMAP) {
		optimise_zone(znr);
		result = 1;
  	}
  	
	for (i = 0; i < INODES_PER_BLOCK; i++) 
		blk_chg |= walk_zone (i + (Block *) blk,
				      mode);
                                      
	/* The nodes beneath the single indirection block are data 
	   blocks, so the block will only be changed when mode == 
	   WZ_REMAP_DATA; in this case we need to pass the current 
	   "virtual" zone number through n2d_map to find the real zone 
	   number */
	if (blk_chg && !readonly)
		write_current_block (n2d(*znr), blk);
	if (mode != WZ_REMAP) {
        	assert (!result);
		assert (!blk_chg);
        }        
	return result;
}

static int walk_zone_dind (Block * znr, enum walk_zone_mode mode)
{
	static char blk[MAX_BLOCK_SIZE];
	int i, result = 0, blk_chg = 0;

	if (!*znr)
		return 0;
	if (debug)
		printf ("DEBUG: walk_zone_dind (&%lu, %d)\n",
			(ulong) *znr, mode);
	check_zone_nr (*znr);

#if FS_IS_ext2
        update_group_population(*znr,mode,current_inode);
#endif
        set_attr(*znr,AT_DATA);
	if (mode == WZ_SCAN)
		update_inode_average(*znr);
	if( mode == WZ_FIXED_BLOCKS )
	  {
		mark_fixed(*znr);
		set_attr(*znr,AT_BAD);
		badblocks++;
	  }	
	read_current_block(*znr, blk);
	
	if (mode == WZ_REMAP) {
		optimise_zone(znr);
		result = 1;
  	}
  
	for (i = 0; i < INODES_PER_BLOCK; i++) 
		blk_chg |= walk_zone_ind (i + (Block *) blk,
					  mode);
                                  
	/* By the time (during the WZ_REMAP_IND pass) that we come to 
	   rewrite this block after reallocating children indblocks, 
	   this current zone will have been optimised - so convert 
	   back to real disk blocks using the n2d map.  This also
	   applies to optimising triple indirection blocks below. */
	if (blk_chg && !readonly)
		write_current_block (n2d(*znr), blk);

	if (mode != WZ_REMAP)
		assert ((!blk_chg) && (!result));
	return result;
}

#ifdef HAS_TIND
static int walk_zone_tind (Block * znr, enum walk_zone_mode mode)
{
	static char blk[MAX_BLOCK_SIZE];
	int i, result = 0, blk_chg = 0;

	if (!*znr)
		return 0;
	if (debug)
		printf ("DEBUG: walk_zone_tind (&%lu, %d)\n", (ulong) *znr, mode);
	check_zone_nr (*znr);

#if FS_IS_ext2
        update_group_population(*znr,mode,current_inode);
#endif
        set_attr(*znr,AT_DATA);
	if (mode == WZ_SCAN)
		update_inode_average(*znr);
	if( mode == WZ_FIXED_BLOCKS )
	  {
		mark_fixed(*znr);
		set_attr(*znr,AT_BAD);
		badblocks++;
	  }		
	read_current_block(*znr, blk);

	if (mode == WZ_REMAP)
	{
		optimise_zone(znr);
		result = 1;
	}

	for (i = 0; i < INODES_PER_BLOCK; i++)
		blk_chg |= walk_zone_dind (i + (Block *) blk, mode);
	if (blk_chg && !readonly)
		write_current_block (n2d(*znr), blk);

	if (mode != WZ_REMAP)
		assert ((!blk_chg) && (!result));
	return result;
}
#endif /* HAS_TIND */

static void walk_inode (struct d_inode *inode, enum walk_zone_mode mode)
{
	int i;
	
#ifdef FS_IS_ext2
	if( inode->i_file_acl )
	  mark_fixed( inode->i_file_acl );
#endif
	for (i = 0; i < DIRECT_ZONES ; i++)
	    walk_zone  ((Block *) ( i                 + inode->i_zone), mode);
	walk_zone_ind  ((Block *) ( DIRECT_ZONES      + inode->i_zone), mode);
	walk_zone_dind ((Block *) ((DIRECT_ZONES + 1) + inode->i_zone), mode);
#ifdef HAS_TIND
	walk_zone_tind ((Block *) ((DIRECT_ZONES + 2) + inode->i_zone), mode);
#endif
}

static void read_fixed_zones (void)
{       
    if (debug)
	printf ("DEBUG: read_fixed_zones()\n");
    if (bad_block_inode) {
	if (!inode_in_use (bad_block_inode))
	    die ("The badblock inode is on the free list.");
	if (get_inode(bad_block_inode))
	    die ("Can't read bad block inode.");
	
	walk_inode(&inode_buffer, WZ_FIXED_BLOCKS);
    }
#if FS_IS_ext2
    {
	/* Reserved blocks don't count as bad blocks */
	int tmp = badblocks;
	int i;

	for (i=1; i < FIRST_USER_INODE; i++) {        
	    if ((i == EXT2_ROOT_INO) ||  /* Allow optimization of root */
		(i == bad_block_inode) ||  /* Done with them already */
		(i == journal_inode))
		continue;
	    if (!inode_in_use (i))
		die ("Reserved inode is on the free list.");
	    if (get_inode(i))
		die ("Can't read reserved inode.");
	    
	    walk_inode(&inode_buffer, WZ_FIXED_BLOCKS);
	}
	badblocks = tmp;
    }                    
#endif                
}

static void optimise_inode (unsigned int i, int scan)
{
	struct d_inode * inode = &inode_buffer;

	if (debug)
		printf ("DEBUG: optimise_inode(%u, %d)\n", i, scan);
	if (get_inode(i))
	{
		if (scan)
			die ("Can't read inode.");
		else
		{
			stat_line ("Warning - skipping inode %d.", i);
			return;
		}
	}

	if (!S_ISDIR (inode->i_mode) && !S_ISREG (inode->i_mode) &&     
	    !S_ISLNK (inode->i_mode) && (i != bad_block_inode))
	{
		inode_average_map[i] = 0;
		return;
	}
	
#if FS_IS_ext2
        /* Ext2 fs has so-called fast symlinks, they store the link
         * name in the inode->i_block[] field. (up to ~50 bytes)
         */
        if (S_ISLNK(inode->i_mode) && (inode->i_blocks==0))
        {
                inode_average_map[i] = 0;
		return;
        }
        
        if (inode->i_dtime) 
                if (scan) {
		        char s[256];
                        sprintf(s,"Error: inode %u marked busy, but dtime!=0\n Run e2fsck\n",i);
                        fatal_error(s);
                }        
#endif /* FS_IS_ext2 */
	if (verbose > 1)
	{
		if (scan) {
		        if (voyer_mode)
			   stat_line ("Scanning inode %u...", i);
		}
		else
			stat_line ("Relocating inode %u...", i);
	}
	if (scan)
	{
		sum_inode_zones = 0.0;
		count_inode_blocks = 0;
                
		walk_inode(inode, WZ_SCAN);
		if (count_inode_blocks)
			inode_average_map[i] = sum_inode_zones != INFINITY ?
				(Block) (sum_inode_zones / (float) count_inode_blocks) :
				~0;
		else
			inode_average_map[i] = 0;
	}
	else
	{
                walk_inode(inode, WZ_REMAP);
		put_inode();
	}
	if (verbose > 1 && !voyer_mode)
	{
		if (scan)
    		        stat_line ("Scanning inode %d... %d block%s.", 
			          i,count_inode_blocks,
			          count_inode_blocks==1 ? "" : "s");
	}
}

/* Scan the disk map.  For each inode, calculate the average of all
   zone numbers occupied by that inode. */
static void scan_used_inodes(void)
{
	unsigned i;
        int last_upd = 0;
        
	if (debug)
		printf ("DEBUG: scan_used_inodes()\n");
	if (verbose)
		stat_line ("Scanning inode zones...");
	for (i=FIRST_USER_INODE; i<=INODES; i++) {
	  	if (inode_in_use(i) && (i != bad_block_inode))
			optimise_inode(i, 1);
                                /* Update at least 50 times during scan */
                if (time(NULL)-last_upd!=0 || (i%(INODES/50+1)==0)) {
                        update_display();        
                        last_upd = time(NULL);
                }        
        }
        update_display();
}

/* Optimise the disk map.  This involves passing twice over the
   zone tree for each inode; once to allocate the new block to each
   indirection block, once to allocate data blocks (and at the same
   time modifying the indirection blocks to reflect the new map - but
   we don't actually MOVE any data yet). */
static void optimise_used_inodes(void)
{
	int i;
        int last_upd = 0;
        
	if (debug)
		printf ("DEBUG: optimise_used_inodes()\n");
	if (verbose)
		stat_line ("Creating data relocation maps...");
	for (i=0; i<used_inodes; i++) {
		optimise_inode(inode_order_map[i], 0);
                                /* Update at least 50 times during scan */
				/* +1 is to avoid division by 0 */
                if (time(NULL)-last_upd!=0 || (i%(used_inodes/50+1)==0)) {
                       update_display();        
                       last_upd = time(NULL);
                }       

        }                
        update_display();
}

/* Read the inode priority file to assign priorities to each inode.
   Higher priorities will be allocated nearer the start of the
   filesystem.  This allows the user to group related files together,
   and to place frequently altered files near the end of the file
   where they will be closer to the free space. */
static void read_priority_file(void)
{
	int inode=-1, r;
	static char tmps[128];
	int priority = 1;
	/* The man page doesn't say what the behavior is if inode
	   numbers appear with no preceding priority specification.
	   Using 1 as a priority in this case means that a priority
	   file containing only inode numbers will give priority to
	   the listed inode numbers.

	   Maybe it should be an error for inode numbers to appear
	   before a priority specification (esp. if there are priority
	   specifications later in the file). */

	if (debug)
		printf ("DEBUG: read_priority_file()\n");

	if (!priority_file)
		return;

	if (verbose)
		stat_line ("Reading inode priorities...");
	while (!feof(priority_file))
	{
		fscanf(priority_file, "%127s", tmps);
		/* Priority specification. */
		if (tmps[0] == '=') 
		{
			sscanf(tmps+1, "%d", &priority);
			if (priority < -100) priority = -100;
			if (priority > 100) priority = 100;
			inode = -1;
			continue;
		}

		/* Inode number. */
		if (verbose >= 3 && inode == -1)
			stat_line ("Priority %d:", priority);
		r = sscanf (tmps, "%d", &inode);
		if (r == EOF)
			break;
		if (r != 1)
		{
			stat_line ("Error in inode priorities file");
			break;
		}
		if (inode < 1 || inode > INODES)
		{
			stat_line ("Warning: inode priority file: "
				   "bad inode number %d.",
				   inode);
			continue;
		}
		if (!inode_in_use(inode))
		{
			stat_line ("Warning: inode priority file: "
				   "inode %d not in use.",
			           inode);
			continue;
		}
		if (verbose >= 3)
			stat_line ("   %d", inode);
		inode_priority_map[inode] = priority;
	}
	fclose (priority_file);
}


/* Sort the inodes in ascending order of average occupied block.  
   Optimising inodes in this order should lead to blocks having to be
   moved, on average, shorter distances on the disk.  This reduces the
   typical time between rescuing a block and writing it to its final
   destination, reducing the average size of the rescue pool. */
static int compare_inodes (const void *a, const void *b)
{
	int aa = *((int const *) a), bb = *((int const *) b);
	Block ave_a = inode_average_map[aa];
	Block ave_b = inode_average_map[bb];
	int pa = inode_priority_map[aa];
	int pb = inode_priority_map[bb];

	/* Sort by priority first, then by disk position. */
	if (pa > pb)
		return -1;
	if (pa < pb)
		return 1;
	if (ave_a < ave_b)
		return -1;
	if (ave_a == ave_b)
		return 0;
	return 1;
}

static void sort_inodes (void)
{
	int i;

	if (debug)
		printf ("DEBUG: sort_inodes()\n");
	if (verbose)
		stat_line ("Sorting inodes...");
	
	/* Initialise the inode order. */
	used_inodes = 0;
	inode_order_map[used_inodes++] = ROOT_INO;
	if (bad_block_inode)
		inode_order_map[used_inodes++] = bad_block_inode;
	if( journal_inode )
	  inode_order_map[used_inodes++] = journal_inode;

	for (i = FIRST_USER_INODE; i <= INODES; i++)
	{
		if (inode_in_use(i))
			inode_order_map[used_inodes++] = i;
	}
	/* Artificially give root inode high priority; it will be the
	   first thing on the disk */
	inode_priority_map[ROOT_INO] = 127;
	/* If it exists, we want the bad block inode's indirection
	   blocks next */
	if (bad_block_inode)
		inode_priority_map[bad_block_inode] = 126;
	if( journal_inode )
	  inode_priority_map[journal_inode] = 126;

	/* And sort... */
	qsort (inode_order_map, used_inodes,
	       sizeof(*inode_order_map),
	       compare_inodes);
}

int main (int argc, char ** argv)
{
	int i;
	char c;
	char tmps[128];

	if (argc && *argv)
		program_name = *argv;
	while ((c = getopt (argc, argv, 
			    "nvVrsp:b:i:"
#ifndef NODEBUG
			    "d"
#endif
			    )) != EOF)
		switch (c)
		{
		case 'b':
		{
			user_bad_inode = strtoul(optarg,0,10);
			break;
		}		
		case 'i':
		{
			if(priority_file)
				fclose( priority_file);
			priority_file = (strcmp( optarg, "-")
					 ? fopen( optarg, "r")
					 : stdin);
			/* I haven't checked whether stdin is safe or not.
			   Possible problems: Do we know it's open?  Do we ever
			   read user input (e.g. for warnings) from stdin, or
			   might we want to in future?  (Perhaps /dev/tty would
			   be better for that, though.) */
			if(!priority_file)
			{
				sprintf (tmps, "Can't open %s", optarg);
				perror (tmps);
				exit(1);
			}
			break;
		}
		case 'v': verbose++; break;
                case 'n': voyer_mode=0; break;
		case 'V': show_version=1; break;
		case 'r': readonly=1; show=1; break;
		case 's': show=1; break;
		case 'p': 
		{
			unsigned long ps;
			int ok = sscanf (optarg, "%lu", &ps);
			if (!ok)
				usage();
			if (ps < 20)
				ps = 20;
			pool_size = ps;
			break;
		}
#ifndef NODEBUG
		case 'd': debug=1; break;
#endif
		default: usage();
		}
	if (show_version)
	{
		printf ("%s %s\n", program_name, version);
		printf ("RCS version %s\n", RCSID);
	}

	if (optind != argc - 1)
	{
		if (show_version)
			exit(0);
		usage ();
	}
        
	device_name = argv[optind];
	if (readonly)
		IN = open (device_name, O_RDONLY);
	else {  
                check_mount(device_name);
		IN = open (device_name, O_RDWR);
        }
	if (IN < 0)
		die("unable to open '%s'");
	for (i = 0 ; i < 3; i++)
		sync();
	read_tables ();
        if (debug) 
                voyer_mode = 0;
        if (voyer_mode && (verbose==0)) {
                verbose = 1;
                show = 1;
        }  
        if (voyer_mode)  init_screen(ZONES);
	init_buffer_tables ();
	init_zone_maps ();
	init_inode_bitmap ();
	if (user_bad_inode)
	{
		if (user_bad_inode > INODES)
			die ("Invalid bad block inode");
		if (user_bad_inode == ROOT_INO)
			die ("Root inode cannot be bad block inode");
		if (!inode_in_use(user_bad_inode))
			die ("Requested bad block inode is not in use");
		bad_block_inode = user_bad_inode;
	}
	read_fixed_zones ();

        show_super_stats();
        show_reserved_blocks();

#ifndef FS_IS_ext2
	next_block_to_fill = FIRSTZONE;
#endif
	scan_used_inodes ();
	read_priority_file();
	sort_inodes ();
#if FS_IS_ext2        
        check_group_population();
#endif
	/* From this point on, we are committed to major disk
	   reorganisation, so try to recover from errors. */
	die_on_io_error = 0;
	
	optimise_used_inodes ();

#ifndef NODEBUG
	validate_relocation_maps ();
#endif
        clear_attr(AT_SELECTED);
	remap_disk_blocks ();

        update_display();
 
	if(!readonly) {
		salvage_free_zones ();
		write_tables ();
		sync ();
	}
	if(show || voyer_mode) {
		char s[256];
                clear_comments();
		sprintf (s,"%6u buffer  reads in %3u group%s",
			 count_buffer_reads, count_read_groups,
			 count_read_groups == 1 ? "" : "s");
                add_comment(s);
		sprintf (s,"    of which %7d  read-aheads.",
			 count_buffer_read_aheads);
                add_comment(s);
		sprintf (s,"%6u buffer writes in %3u group%s",
			 count_buffer_writes, count_write_groups,
			 count_write_groups==1 ? "" : "s");
                add_comment(s);
		sprintf (s,"%6d migrations, %8d forces",
			 count_buffer_migrates, count_buffer_forces);
                add_comment(s);
                display_comments(" Relocation statistics ");
	}
        
	if (voyer_mode) {
                done_screen(TRUE);
        }        
	if (io_errors)
		printf ("WARNING - There were %d read-write errors.\n"
			"Run %s to check the filesystem.\n",
			io_errors, fsck);
	return (0);
}
