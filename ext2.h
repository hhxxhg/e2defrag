/*
 * ext2.h - extfs-specific include file for the Linux file system 
 * degragmenter. 
 *
 * Copyleft  (C) 1993 Alexey Vovenko (vovenko@ixwin.ihep.su)
 * Copyright (C) 1992, 1993 Stephen Tweedie (sct@dcs.ed.ac.uk)
 * Copyright (C) 1992 Remy Card (card@masi.ibp.fr)
 * Copyright (C) 1991 Linus Torvalds (torvalds@kruuna.helsinki.fi)
 * 
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 *
 */

/*#include <linux/fs.h>*/
#include <ext2fs/ext2_fs.h>
/*#include <ext2fs/ext2_fs_i.h>
 *#include <ext2fs/ext2_fs_sb.h>*/

#define HAS_TIND

#ifndef SUPERBLOCK_OFFSET
# define SUPERBLOCK_OFFSET 1024
# define SUPERBLOCK_SIZE 1024
#endif

#define DIRECT_ZONES     EXT2_NDIR_BLOCKS

#define ROOT_INO         EXT2_ROOT_INO
#define BAD_INO          EXT2_BAD_INO
#define FIRST_USER_INODE EXT2_FIRST_INO(&Super)

/* This needs to be exactly 32 bits, given that walk_zone_ind etc. in
   defrag.c have `i + (Block *) blk'.

   If you don't like `__u32', then could use `unsigned' (which is
   simpler for printf).  Many parts of defrag already assume that
   unsigned/int are at least 32 bits, and the configure script checks
   that sizeof(unsigned)==4. */
typedef __u32 Block;

#define d_inode ext2_inode
#define i_zone i_block

#define UPPER(size,n)		((size + ((n) - 1)) / (n))
#define INODE_SIZE		(sizeof (struct d_inode))
#define INODE_BLOCKS		UPPER(INODES, EXT2_INODES_PER_BLOCK)

#define Super		(* (struct ext2_super_block *) super_block_buffer)
#define INODES		(Super.s_inodes_count)
#define ZONES		(Super.s_blocks_count)
#define FREEBLOCKSCOUNT	(Super.s_free_blocks_count)
#define FREEINODESCOUNT	(Super.s_free_inodes_count)
#define FIRSTZONE	(Super.s_first_data_block) 
#define ZONESIZE	(Super.s_log_block_size)
#define MAGIC		(Super.s_magic)
#define INODES_PER_BLOCK (block_size >> 2)
#define MAX_BLOCK_SIZE EXT2_MAX_BLOCK_SIZE
#define EXT2_COMPRESSED_BLKADDR 0xffffffff

    /* This structure is used to calculate, how many blocks are claimed 
     * by the inodes in the each group. 
     * native_blocks is set by update_group_population and counts blocks
     *               that should be in the group and already are there.
     *               This is the most useless variable. It's collected for
     *               report only.
     *
     * wants_away    is set by update_group_population and counts blocks   
     *               that shouldn't be in the group, but initially are.
     *
     * wants_back    is also set by check_group_population and holds 
     *               count of blocks, that aren't initially in their group
     *
     * free_blocks   is set by_check_group_population() from the block 
     *               bitmaps to the amount of the blocks that would be
     *               left free in the group, after all its blocks are
     *               brought there and all foreign blocks are removed.
     *               This is the key parameter which tells us, how
     *               many blocks from other groups are allowed into the
     *               current group without pushing out the blocks from 
     *               this group.
     * 
     */
      
struct groups_population {
   ulong free_blocks;
   ulong native_blocks; /* Not used actually anywhere, but verbose report */
   ulong wants_away;
   ulong wants_back;
   ulong next_block_to_fill;
   ulong last_block;
};

ulong choose_block (ulong inode);
void check_group_population (void);
void update_group_population (ulong znr, enum walk_zone_mode mode, ulong inode);
struct groups_population *push_group_population();
void pop_group_population (struct groups_population *ogp);

extern int gp_stack_count;
