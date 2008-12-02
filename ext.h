/*
 * ext.h - extfs-specific include file for the Linux file system 
 * degragmenter. 
 * $Id: ext.h,v 1.3 1997/08/10 16:32:09 linux Exp $
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

#include <linux/fs.h>
#include <linux/ext_fs.h>

#define HAS_TIND

#define DIRECT_ZONES 9

#define ROOT_INO 1
#define BAD_INO  2
#define FIRST_USER_INODE 3

typedef unsigned long Block;

#define d_inode ext_inode

#define UPPER(size,n)		((size + ((n) - 1)) / (n))
#define INODE_SIZE		(sizeof (struct d_inode))
#define INODE_BLOCKS		UPPER(INODES, EXT_INODES_PER_BLOCK)
#define INODE_BUFFER_SIZE	(INODE_BLOCKS * BLOCK_SIZE)

#define BITS_PER_BLOCK		(BLOCK_SIZE<<3)

#define Super		(* (struct ext_super_block *) super_block_buffer)
#define INODES		(Super.s_ninodes)
#define ZONES		(Super.s_nzones)
#define FIRSTFREEBLOCK	(Super.s_firstfreeblock)
#define FREEBLOCKSCOUNT	(Super.s_freeblockscount)
#define FIRSTFREEINODE	(Super.s_firstfreeinode)
#define FREEINODESCOUNT	(Super.s_freeinodescount)
#define FIRSTZONE	(Super.s_firstdatazone)
#define ZONESIZE	(Super.s_log_zone_size)
#define MAXSIZE		(Super.s_max_size)
#define MAGIC		(Super.s_magic)
#define NORM_FIRSTZONE	(2 + INODE_BLOCKS)
#define INODES_PER_BLOCK (BLOCK_SIZE >> 2)
#define SUPERBLOCK_SIZE 1024
#define MAX_BLOCK_SIZE 1024
