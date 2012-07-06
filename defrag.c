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
#include <ext2fs/ext2fs.h>

#include "defrag.h"
#include "version.h"
#include "map.h"

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
Block bad_block_inode = 0, journal_inode;

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
signed char *inode_priority_map = NULL;
__u32 *inode_order_map = NULL;
char * fixed_map = NULL;

/* Local variables */
static int used_inodes = 0;
static FILE *priority_file = 0;
static char default_file_prio, default_dir_prio;

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
  if (!gp_stack_count)
    {
      /* Update the zone maps. */
      map_forward_set (ox, ny);
    }
  *znr = ny;
#if FS_IS_xia
  *znr |= i_blocks;
#endif                
}

#ifndef NODEBUG
static void validate_relocation_maps(void)
{
	Block i, x;

	for (i=first_zone; i < zones; i++)
	{
		x = map_reverse_get (i);
		if (x && x != -1)
			assert (map_forward_get (x) == i);
		x = map_forward_get (i);
		if (x)
			assert (map_reverse_get(x) == i);
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
		write_current_block (map_reverse_get(*znr), blk);
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
		write_current_block (map_reverse_get(*znr), blk);

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
		write_current_block (map_reverse_get(*znr), blk);

	if (mode != WZ_REMAP)
		assert ((!blk_chg) && (!result));
	return result;
}
#endif /* HAS_TIND */

/**************************************************************************
 * Extents are handled in 3 phases.  The first phase loads the extent tree
 * into memory.  This walks the tree, one block at a time, copying the
 * extents to a flat, linear extent array for easier processing.  In the
 * process, we count the number of blocks in the tree.  The second phase
 * walks the array of extents, allocating new blocks and building a new
 * array of extents.  It first allocates enough blocks to hold the current
 * extent tree, assuming that the new extent tree will be the same size.
 * If the new extents won't fit in the space allocated, phase 2 is
 * restarted, allocting more blocks this time for the extent tree.  If
 * the size of the tree is smaller than allocated, and we have not already
 * had to restart to grow the tree ( to prevent an infinite loop of grow,
 * shrink, grow, etc ), then restart phase 2 with fewer blocks allocated
 * to the extent tree.  Finally, phase 3 is the inverse of phase 1:
 * take the new flat extent array and write it out to disk as a tree.
 * In order to allow restarting phase2, the group population must be
 * saved and updates to the relocation map must be disabled.  This means
 * we do only simulations until we settle on the tree size, then do
 * the real remap pass.
 *
 * Walking the tree requires a cursor to keep track of a few things:
 *   1) A buffer holding the current tree node
 *   2) The current index within that node
 * And you need one of these for every level in the tree.  The lowest
 * level ( zero ) contains extents, the others index the next level.
 * The highest level of the tree is always stored right in the inode.
 * We allow for trees to be as high as 4 levels.
 *************************************************************************/

signed int walk_extent_tree (struct ext3_extent_header *tree_root,
			     enum walk_zone_mode mode)
{
  struct ext3_extent_header *tree_buffer[4];
  __u16 tree_index[4];
  __u16 tree_level;
  int old_extents_count = 0, new_extents_count;
  struct ext3_extent *old_extents = 0, *new_extents, *last_new_extent;
  __u16 tree_blocks_count = 0;
  Block *tree_blocks = 0;
  int i;
  char already_grown = 0;
  char simulate = 1;
  signed int block_delta = 0;
  __u16 needed_tree_blocks;
  const int entries_per_block = (EXT2_BLOCK_SIZE(&Super) - sizeof (struct ext3_extent_header)) /
    sizeof (struct ext3_extent);

  /* Phase 1: walk the tree */
  tree_level = tree_root->eh_depth;
  tree_index[tree_level] = 0;
  tree_buffer[tree_level] = tree_root;
  /* allocate buffers for the lower levels */
  while (tree_level--)
    tree_buffer[tree_level] = malloc (EXT2_BLOCK_SIZE(&Super));
  tree_level = tree_root->eh_depth;
    
  if (tree_root->eh_magic != EXT3_EXT_MAGIC)
    die ("extent tree root bad magic");

  /* we're done when we have come back to the root and
     processed the last entry in it */
  while (1)
    {
      if (tree_level == 0) {
	/* we have reached extents */
	old_extents_count += tree_buffer[tree_level]->eh_entries;
	old_extents = realloc (old_extents, old_extents_count * sizeof(struct ext3_extent));
	memcpy (old_extents + old_extents_count - tree_buffer[tree_level]->eh_entries,
		tree_buffer[tree_level]+1,
		sizeof(struct ext3_extent) * tree_buffer[tree_level]->eh_entries);
	tree_index[0] = tree_buffer[0]->eh_entries - 1;
	while (++tree_index[tree_level] >= tree_buffer[tree_level]->eh_entries)
	  /* finished with this node, go up a level and over one */
	  if (tree_level == tree_root->eh_depth)
	    break; /* we're done */
	  else tree_level++;
      }
      if (tree_level == tree_root->eh_depth &&
	  tree_index[tree_level] == tree_root->eh_entries)
	break; /* we're done */
      /* load the new block */
      struct ext3_extent_idx *idx = (struct ext3_extent_idx *)(tree_buffer[tree_level]+1);
      idx += tree_index[tree_level];
      if (idx->ei_leaf_hi)
	die ("ei_leaf_hi != 0");
      read_current_block (idx->ei_leaf,
			  (char *)tree_buffer[--tree_level]);
      if (mode == WZ_REMAP)
	map_forward_set (idx->ei_leaf, 0); /* free block */
      tree_index[tree_level] = 0;
      if (tree_buffer[tree_level]->eh_magic != EXT3_EXT_MAGIC)
	die ("extent tree bad magic");
      if (tree_level != tree_buffer[tree_level]->eh_depth)
	die ("extent block has wrong depth");
      tree_blocks_count++;
      tree_blocks = realloc (tree_blocks, tree_blocks_count * sizeof(Block));
      tree_blocks[tree_blocks_count-1] = idx->ei_leaf;
      Block b = idx->ei_leaf;
      if (mode != WZ_REMAP)
	walk_zone (&b, mode);
    }
  /* free buffers */
  while (tree_level--)
    free (tree_buffer[tree_level]);
  if (mode != WZ_REMAP) {
    /* just need walk_zone to scan data blocks */
    for (i = 0; i < old_extents_count; i++) {
      Block start, end;
      /* block = logical: relative to file, start = physical: relative to disk */
      __u16 ee_len;
      ee_len = old_extents[i].ee_len;
      if (ee_len > 0x8000) {
	/* decode uninitialized flag and length */
	ee_len &= 0x7FFF;
      }
      start = old_extents[i].ee_start;
      end = start + ee_len;
      for ( ; start < end; start++ )
	walk_zone (&start, mode);
    }
    goto cleanup_1;
  }
  /* Phase 2: Remap the blocks */
  save_group_population(); /* so we can restore and retry if needed */
  goto phase2;

 phase2_restart:
  block_delta += (needed_tree_blocks - tree_blocks_count);
  restore_group_population();
  if (simulate)
    save_group_population(); /* so we can restore and retry if needed */
  if (needed_tree_blocks < tree_blocks_count) {
    /* free some blocks */
    while (needed_tree_blocks < tree_blocks_count) {
      map_forward_set (tree_blocks[--tree_blocks_count], 0);
      update_group_population (tree_blocks[tree_blocks_count],
			       WZ_FREE, current_inode);
    }
    tree_blocks = realloc (tree_blocks, tree_blocks_count * sizeof(Block));
  }
  if (needed_tree_blocks > tree_blocks_count) {
    /* allocate some blocks */
    Block b = zones - 1;
    tree_blocks = realloc (tree_blocks, sizeof(Block) * needed_tree_blocks);
    while (needed_tree_blocks > tree_blocks_count) {
      /* find a block that is currently free */
      while (map_forward_get (b))
	b--;
      tree_blocks[tree_blocks_count++] = b;
      update_group_population (b, WZ_ALLOC, current_inode);
    }
  }
  tree_root->eh_depth = tree_level;
  free (new_extents);
 phase2:
  new_extents_count = 0;
  new_extents = 0;
  last_new_extent = 0;
    
  /* first allocate the blocks to hold the extent tree */
  for (i = 0; i < tree_blocks_count; i++) {
    Block b = tree_blocks[i];
    walk_zone (&b, mode);
  }
  /* now do the data blocks */
  for (i = 0; i < old_extents_count; i++) {
    Block start, end, block;
    /* block = logical: relative to file, start = physical: relative to disk */
    __u16 ee_len;
    __u16 uninitialized;
    ee_len = old_extents[i].ee_len;
    if (ee_len > 0x8000) {
      /* decode uninitialized flag and length */
      uninitialized = 0x8000;
      ee_len &= 0x7FFF;
    } else uninitialized = 0;
    block = old_extents[i].ee_block;
    start = old_extents[i].ee_start;
    end = start + ee_len;
    for ( ; start < end; start++,block++ ) {
      Block nstart = start;
      if (uninitialized) {
	/* don't bother relocating uninitialized blocks */
	nstart = choose_block (current_inode);
	if (!simulate) {
	  map_forward_set (start, 0);
	  map_forward_set (0, nstart);
	}
      } else optimise_zone (&nstart);

      if (last_new_extent) {
	__u16 last_uninitialized;
	__u16 last_ee_len;
	if (last_new_extent->ee_len > 0x8000) {
	  last_uninitialized = 0x8000;
	  last_ee_len = last_new_extent->ee_len & 0x7FFF;
	} else {
	  last_ee_len = last_new_extent->ee_len;
	  last_uninitialized = 0;
	}
	/* we can only append to the last new extent if:
	   1) its uninitialized flag matches the current old extent
	   2) its logical block follows that of the current block
	   3) its physical block follows that of the last new extent
	   4) The length of the last new extent is not already at max */
	if (uninitialized != last_uninitialized ||
	    block != last_new_extent->ee_block + last_ee_len ||
	    nstart != last_new_extent->ee_start + last_ee_len ||
	    last_new_extent->ee_len == 0x8000 ||
	    last_new_extent->ee_len == 0xFFFF)
	  last_new_extent = 0;
      }
      if (last_new_extent)
	last_new_extent->ee_len++; /* append */
      else {
	/* allocat a new extent */
	new_extents_count++;
	new_extents = realloc (new_extents,
			       new_extents_count * sizeof(struct ext3_extent));
	last_new_extent = new_extents + new_extents_count - 1;
	last_new_extent->ee_len = 1 | uninitialized;
	last_new_extent->ee_start = nstart;
	last_new_extent->ee_start_hi = 0;
	last_new_extent->ee_block = block;
      }
    }
  }
  /* calculate how many blocks are needed to hold the extent tree */
  if (new_extents_count <= 4) {
    needed_tree_blocks = 0; /* can fit in inode */
    tree_level = 0;
  }
  else if (new_extents_count < entries_per_block * 4)
    {
      /* can fit in a level 1 tree */
      needed_tree_blocks = ((new_extents_count - 1) / entries_per_block) + 1;
      tree_level = 1;
    }
  else if (new_extents_count < entries_per_block * entries_per_block * 4)
    {
      /* can fit in a level 2 tree */
      needed_tree_blocks = ((new_extents_count - 1) / entries_per_block) + 1;
      /* add count of level 1 nodes needed to hold that many level 0 leafs */
      needed_tree_blocks += ((needed_tree_blocks - 1) / entries_per_block) + 1;
      tree_level = 2;
    }
  else if (new_extents_count < entries_per_block * entries_per_block * entries_per_block * 4)
    {
      /* needs a level 3 tree */
      int needed_level_0_blocks = ((new_extents_count - 1) / entries_per_block) + 1;
      /* add count of level 1 nodes needed to hold that many level 0 leaves */
      int needed_level_1_blocks = ((needed_level_0_blocks - 1) / entries_per_block) + 1;
      /* add count of level 2 nodes needed to hold that many level 1 nodes */
      int needed_level_2_blocks = ((needed_level_1_blocks - 1) / entries_per_block) + 1;
      needed_tree_blocks = needed_level_0_blocks + needed_level_1_blocks + needed_level_2_blocks;
      tree_level = 3;
    } else die ("Too many extents");
  /* restart phase 2 if we can't fit the extents in the blocks allocated,
     or if we can reduce the allocated blocks */
  if (needed_tree_blocks > tree_blocks_count) {
    already_grown = 1;
    goto phase2_restart;
  }
  if (!already_grown && needed_tree_blocks < tree_blocks_count) {
    goto phase2_restart;
  }
  if (simulate) {
    simulate = 0;
    goto phase2_restart;
  }
  if (readonly)
    goto cleanup_2;
  /* Phase 3: write out extents */
  tree_level = tree_root->eh_depth;
  tree_index[tree_level] = 0;
  tree_buffer[tree_level] = tree_root;
  memset (tree_root+1, 0, sizeof(struct ext3_extent) * tree_root->eh_max);
  /* allocate buffers for the lower levels */
  while (tree_level--)
    tree_buffer[tree_level] = malloc (EXT2_BLOCK_SIZE(&Super));
  tree_level = tree_root->eh_depth;
  tree_root->eh_depth = tree_level;
  i = 0;
  tree_root->eh_entries = 0;
  int tree_i = 0;
  while (1)
    {
      if (tree_level == 0) {
	/* we have reached extents */
	if (new_extents_count - i > tree_buffer[tree_level]->eh_max)
	  tree_buffer[tree_level]->eh_entries = tree_buffer[tree_level]->eh_max;
	else tree_buffer[tree_level]->eh_entries = new_extents_count - i;
	memcpy (tree_buffer[tree_level]+1,
		new_extents + i,
		sizeof(struct ext3_extent) * tree_buffer[tree_level]->eh_entries);
	i += tree_buffer[0]->eh_entries;
	tree_index[0] = tree_buffer[0]->eh_max;
	while (tree_index[tree_level] >= tree_buffer[tree_level]->eh_max ||
	       i == new_extents_count) {
	  struct ext3_extent *last_extent = (struct ext3_extent *)(tree_buffer[tree_level])+1;
	  /* finished with this node, go up a level and over one */
	  if (tree_level == tree_root->eh_depth)
	    break; /* already at the root */
	  tree_level++;
	  /* update the keys at this index */
	  struct ext3_extent_idx *idx = (struct ext3_extent_idx *)(tree_buffer[tree_level]+1);
	  idx += tree_index[tree_level];
	  idx->ei_block = last_extent->ee_block;
	  /* write out node to its old location */
	  write_current_block (map_reverse_get (idx->ei_leaf), (char *)tree_buffer[tree_level-1]);
	  /* move over one */
	  tree_index[tree_level]++;
	}
      }
      if (tree_level == tree_root->eh_depth &&
	  i == new_extents_count)
	break; /* we're done */
      /* create the new block */
      struct ext3_extent_idx *idx = (struct ext3_extent_idx *)(tree_buffer[tree_level]+1);
      idx += tree_index[tree_level];
      idx->ei_leaf_hi = 0;
      assert (tree_i < tree_blocks_count);
      idx->ei_leaf = map_forward_get (tree_blocks[tree_i++]);
      idx->ei_unused = 0;
      tree_buffer[tree_level]->eh_entries++;
      tree_level--;
      tree_index[tree_level] = 0;
      /* zero the block */
      memset (tree_buffer[tree_level], 0, EXT2_BLOCK_SIZE(&Super));
      tree_buffer[tree_level]->eh_magic = EXT3_EXT_MAGIC;
      tree_buffer[tree_level]->eh_depth = tree_level;
      tree_buffer[tree_level]->eh_max = entries_per_block;
    }
  /* free buffers */
  while (tree_level--)
    free (tree_buffer[tree_level]);
 cleanup_2:
  free (tree_blocks);
  free (new_extents);
 cleanup_1:
  free (old_extents);
  return block_delta;
}
	
static void walk_inode (struct d_inode *inode, enum walk_zone_mode mode)
{
	int i;
	__s64 blocks;
	
#ifdef FS_IS_ext2
	if (inode->i_file_acl)
	  mark_fixed (inode->i_file_acl);
	if (inode->i_flags & EXT4_EXTENTS_FL)
	  {
	    struct ext3_extent_header *eh;

	    eh = (struct ext3_extent_header *)&inode->i_block[0];
	    if (eh->eh_magic != EXT3_EXT_MAGIC)
	      die ("Bad eh_magic");
	    if (eh->eh_max != 4)
	      die ("Bad eh_max");
	    if ( !voyer_mode &&
		 verbose > 1)
	      stat_line ("Inode %u has depth %u", current_inode, eh->eh_depth);
	    blocks = ((long long)inode->osd2.linux2.l_i_blocks_hi) << 16;
	    blocks |= inode->i_blocks;
	    if (inode->i_flags & EXT4_HUGE_FILE_FL)
	      blocks += walk_extent_tree (eh, mode);
	    blocks += (walk_extent_tree (eh, mode) * (EXT2_BLOCK_SIZE (&Super) / 512));
	    inode->osd2.linux2.l_i_blocks_hi = blocks>>32;
	    inode->i_blocks = blocks;
	    return;
	  }
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
	
	walk_inode((struct ext2_inode *)&inode_buffer, WZ_FIXED_BLOCKS);
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
	    
	    walk_inode((struct ext2_inode *)&inode_buffer, WZ_FIXED_BLOCKS);
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
		return;
	}
	
#if FS_IS_ext2
        /* Ext2 fs has so-called fast symlinks, they store the link
         * name in the inode->i_block[] field. (up to ~50 bytes)
         */
        if (S_ISLNK(inode->i_mode) && (inode->i_blocks==0))
        {
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
		if (S_ISDIR(inode->i_mode))
			inode_priority_map[i] = default_dir_prio;
		else inode_priority_map[i] = default_file_prio;
		walk_inode(inode, WZ_SCAN);
	}
	else
	{
                walk_inode(inode, WZ_REMAP);
		put_inode();
	}
	if (verbose > 1 && !voyer_mode)
	{
		if (scan)
    		        stat_line ("Scanning inode %d...", i);
	}
}

/* Scan the disk map.  For each inode, calculate the average of all
   zone numbers occupied by that inode. */
static void scan_used_inodes(void)
{
	unsigned i;
        
	if (debug)
		printf ("DEBUG: scan_used_inodes()\n");
	if (verbose)
		stat_line ("Scanning inode zones...");
	for (i=FIRST_USER_INODE; i<=INODES; i++) {
	  	if (inode_in_use(i) && (i != bad_block_inode))
			optimise_inode(i, 1);
                                /* Update at least 50 times during scan */
                if (i%(INODES/50+1)==0) {
                        update_display();
                }        
        }
        update_display();
	if (debug)
		dump_extents();
}

/* Optimise the disk map.  This involves passing twice over the
   zone tree for each inode; once to allocate the new block to each
   indirection block, once to allocate data blocks (and at the same
   time modifying the indirection blocks to reflect the new map - but
   we don't actually MOVE any data yet). */
static void optimise_used_inodes(void)
{
	int i;
        
	if (debug)
		printf ("DEBUG: optimise_used_inodes()\n");
	if (verbose)
		stat_line ("Creating data relocation maps...");
	for (i=0; i<used_inodes; i++) {
		optimise_inode(inode_order_map[i], 0);
                                /* Update at least 50 times during scan */
				/* +1 is to avoid division by 0 */
                if (i%(used_inodes/50+1)==0) {
                       update_display();        
                }       

        }                
        update_display();
	if (debug)
		dump_extents();
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
	int pa = inode_priority_map[aa];
	int pb = inode_priority_map[bb];

	/* Sort by priority first, then by disk position. */
	if (pa > pb)
		return -1;
	if (pa < pb)
		return 1;
	if (aa > bb)
		return 1;
	if (aa < bb)
		return -1;
	return 0;
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
		if (inode_in_use(i)) {
			inode_order_map[used_inodes++] = i;
			assert (used_inodes < (INODES-FREEINODESCOUNT));
		}
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
			    "nvVrsp:i:F:D:"
#ifndef NODEBUG
			    "d"
#endif
			    )) != EOF)
		switch (c)
		{
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
		case 'F': default_file_prio = atoi (optarg); break;
		case 'D': default_dir_prio = atoi (optarg); break;
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
		die("unable to open device");
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
	map_init();
	init_buffer_tables ();
	init_zone_maps ();
	init_inode_bitmap ();
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
	if (debug)
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
		unsigned long mb = (((unsigned long) count_buffer_writes >> 10)
				    * (block_size >> 7))
		  >> (20 - 10 - 7);
		sprintf (s, "avg transfer speed: %.1f MB/s",
			 (float)mb / (time(NULL) - transfer_start_time));
		if (!readonly)
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
