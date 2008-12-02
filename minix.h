/*
 * minix.h - minix-specific include file for the Linux file system 
 * degragmenter. 
 * $Id: minix.h,v 1.3 1997/08/10 16:32:10 linux Exp $
 *
 * Copyright (C) 1992 Stephen Tweedie (sct@dcs.ed.ac.uk)
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
#include <linux/minix_fs.h>
#include <linux/minix_fs_i.h>
#include <linux/minix_fs_sb.h>

#define ROOT_INO 1
#define FIRST_USER_INODE 2
#define DIRECT_ZONES 7

typedef unsigned short Block;

#define d_inode minix_inode

#define UPPER(size,n)		((size + ((n) - 1)) / (n))
#define INODE_SIZE		(sizeof (struct d_inode))
#define INODE_BLOCKS		UPPER(INODES, MINIX_INODES_PER_BLOCK)
#define INODE_BUFFER_SIZE	(INODE_BLOCKS * BLOCK_SIZE)

#define BITS_PER_BLOCK		(BLOCK_SIZE<<3)

#define Super		(* (struct minix_super_block *) super_block_buffer)
#define INODES ((unsigned long)Super.s_ninodes-1)
#define ZONES ((unsigned long)Super.s_nzones)
#define IMAPS ((unsigned long)Super.s_imap_blocks)
#define ZMAPS ((unsigned long)Super.s_zmap_blocks)
#define FIRSTZONE ((unsigned long)Super.s_firstdatazone)
#define ZONESIZE ((unsigned long)Super.s_log_zone_size)
#define MAXSIZE ((unsigned long)Super.s_max_size)
#define MAGIC (Super.s_magic)
#define NORM_FIRSTZONE (2+IMAPS+ZMAPS+INODE_BLOCKS)
#define INODES_PER_BLOCK (BLOCK_SIZE >> 1)
#define SUPERBLOCK_SIZE 1024
#define MAX_BLOCK_SIZE 1024
