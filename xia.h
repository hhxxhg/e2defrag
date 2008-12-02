/*
 * xia.h - xiafs-specific include file for the Linux file system 
 * degragmenter. 
 * $Id: xia.h,v 1.3 1997/08/10 16:32:10 linux Exp $
 *
 * Copyleft  (C) 1993 Alexey Vovenko (vovenko@ixwin.ihep.su)
 * Copyright (C) 1992 Stephen Tweedie (sct@dcs.ed.ac.uk)
 * Copyright (C) 1992 Remy Card (card@masi.ibp.fr)
 * Copyright (C) 1991 Linus Torvalds (torvalds@kruuna.helsinki.fi)
 * 
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 *
 */

#include <linux/fs.h>
#include <linux/xia_fs.h>
#include <linux/xia_fs_sb.h>

#define ROOT_INO _XIAFS_ROOT_INO
#define BAD_INO  _XIAFS_BAD_INO
#define FIRST_USER_INODE 3
#define DIRECT_ZONES 8

/* Someone please check this (i.e. using signed type for Block).  What is the
 * smallest possible block (zone) size for xia?  In particular, do there exist
 * any xia filesystems with >2 billion blocks?  Negative block numbers will be
 * mishandled by many parts of defrag.
 */ 
typedef int Block;

#define d_inode xiafs_inode

#define UPPER(size,n)		((size + ((n) - 1)) / (n))
#define INODE_SIZE		(sizeof (struct d_inode))
#define INODE_BLOCKS		UPPER(INODES, _XIAFS_INODES_PER_BLOCK)
#define INODE_BUFFER_SIZE	(INODE_BLOCKS * BLOCK_SIZE)

#define BITS_PER_BLOCK		(BLOCK_SIZE<<3)

#define Super		(* (struct xiafs_super_block *) super_block_buffer)
#define INODES ((unsigned long)Super.s_ninodes)
#define ZONES ((unsigned long)Super.s_nzones)
#define IMAPS ((unsigned long)Super.s_imap_zones)
#define ZMAPS ((unsigned long)Super.s_zmap_zones)
#define FIRSTZONE ((unsigned long)Super.s_firstdatazone)
#define ZONESIZE ((unsigned long)Super.s_zone_size)
#define MAXSIZE ((unsigned long)Super.s_max_size)
#define MAGIC (Super.s_magic)
#define NORM_FIRSTZONE (1+IMAPS+ZMAPS+INODE_BLOCKS)
#define INODES_PER_BLOCK (BLOCK_SIZE >> 2)
#define SUPERBLOCK_SIZE 1024
#define MAX_BLOCK_SIZE 1024
