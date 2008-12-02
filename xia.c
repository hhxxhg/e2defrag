/*
 * xia.c - xiafs-specific functions and data for the Linux file system 
 * degragmenter. 
 * $Id: xia.c,v 1.3 1997/08/10 16:32:10 linux Exp $
 *
 * This file is responsible for managing those data structures dependent
 * on the xia-fs.  Specifically, it
 * + allocates space for the various disk maps stored in memory; 
 * + reads and writes superblock information; 
 * + reads the used-data-zones and used-inodes maps into memory
 *   (inode_map and d2n/n2d_map respectively); and, 
 * + once the defragmentation is complete, rewrites the new free-space
 *   map to disk.
 *
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

#include <config.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#include "defrag.h"

char const * program_name = "xdefrag";
char const * fsck = "xfsck";

int inode_table_offset = 0;

/* A few static variables and functions, needed only within xia.c
   for handling minix-specific data structures (like the free zone
   map) */
static char *zone_map = NULL;

#define bm_zone_in_use(x) (bit(zone_map,(x)-FIRSTZONE+1))
#define bm_mark_zone(x) (setbit(zone_map,(x)-FIRSTZONE+1),changed=1)
#define bm_unmark_zone(x) (clrbit(zone_map,(x)-FIRSTZONE+1),changed=1)


/* Read the superblock and inode table and reserve space for the
   remaining disk map tables */
void read_tables (void)
{
	if (debug)
		printf ("DEBUG: read_tables()\n");
	if (0 != nlseek (IN, 0, SEEK_SET))
		die ("seek failed");
	if (BLOCK_SIZE != nread (IN, super_block_buffer, BLOCK_SIZE))
		die ("unable to read super-block");
	if (MAGIC != _XIAFS_SUPER_MAGIC)
		die ("bad magic number in super-block");
	if (ZONESIZE != 1024 || BLOCK_SIZE != 1024)
		die("Only 1k blocks/zones supported");
	if (!IMAPS || IMAPS > _XIAFS_IMAP_SLOTS)
		die("bad s_imap_blocks field in super-block");
	if (!ZMAPS || ZMAPS > _XIAFS_ZMAP_SLOTS)
		die("bad s_zmap_blocks field in super-block");

	inode_map = malloc (IMAPS*BLOCK_SIZE);
	if (!inode_map)
		die ("Unable to allocate inodes bitmap\n");
	if (IMAPS*BLOCK_SIZE != nread(IN,inode_map,IMAPS*BLOCK_SIZE))
		die("Unable to read inode map");

	d2n_map = malloc ((ZONES - FIRSTZONE) * sizeof (*d2n_map));
	n2d_map = malloc ((ZONES - FIRSTZONE) * sizeof (*n2d_map));
	if (!n2d_map || !d2n_map)
		die ("Unable to allocate zone maps\n");

	zone_map = malloc (ZMAPS*BLOCK_SIZE);
	if (!zone_map)
		die ("Unable to allocate zone bitmap\n");
	if (ZMAPS*BLOCK_SIZE != nread(IN,zone_map,ZMAPS*BLOCK_SIZE))
		die("Unable to read zone map");

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

	fixed_map = malloc (((ZONES - FIRSTZONE) / 8) + 1);
	if (!fixed_map)
		die ("Unable to allocate unmoveable zones bitmap\n");
	memset(fixed_map, 0, ((ZONES - FIRSTZONE) / 8) + 1);

	if (NORM_FIRSTZONE != FIRSTZONE && Super.s_firstkernzone==0)
		printf ("Warning: Firstzone != Norm_firstzone\n");
	first_zone = FIRSTZONE;
	zones = ZONES;
	block_size = BLOCK_SIZE;
	bad_block_inode = BAD_INO;
}

/* Write the superblock, free zone/inode maps and inode table to disk */
void write_tables (void)
{
	if (debug)
		printf ("DEBUG: write_tables()\n");
	if (0 != nlseek (IN, 0, SEEK_SET))
		die ("seek failed in write_tables");
	if (BLOCK_SIZE != nwrite (IN, super_block_buffer, BLOCK_SIZE))
		die ("unable to write super-block");
	if (IMAPS*BLOCK_SIZE != nwrite(IN,inode_map,IMAPS*BLOCK_SIZE))
		die("Unable to write inode map");
	if (ZMAPS*BLOCK_SIZE != nwrite(IN,zone_map,ZMAPS*BLOCK_SIZE))
		die("Unable to write zone map");
}

/* Recalculate the free zone map.  The defragmentation procedure will
   migrate all free blocks to the end of the disk partition, and so
   after defragmentation the free space map must be updated to reflect
   this. Free zones are determined by n2d_map, and the macro
   zone_in_use(n) is defined in defrag.h for this purpose. The xiafs
   fs stores the free zone map as a bitmap, so this bitmap must now be
   recreated from the n2d_map. */
void salvage_free_zones (void)
{
	Block i;
	if (debug)
		printf ("DEBUG: salvage_zone_freelist()\n");
	memset (zone_map, -1, ZMAPS*BLOCK_SIZE);
	for (i=FIRSTZONE; i<ZONES; i++)
	{
		if (!zone_in_use(i))
			bm_unmark_zone(i);
	}
}


/* Read the map of used/unused data zones on disk. 
   The map is held jointly in d2n_map and n2d_map, described in
   defrag.h.  These are initialised to the identity map (d2n(i) = n2d(i)
   = i), and then the free zone bitmap is scanned, and all unused zones
   are marked as zero in both d2n_map and n2d_map. */
void init_zone_maps (void)
{
	Block i;

	if (debug)
		printf ("DEBUG: init_zone_maps()\n");
	for (i = FIRSTZONE; i < ZONES; i++)
	{
		if (bm_zone_in_use(i))
		{
			d2n(i) = i;
			n2d(i) = i;
		}
		else
		{
			d2n(i) = 0;
			n2d(i) = 0;
		}
	}
}

/* Read in the map of used/unused inodes.  For the xiafs, we don't
   need to anything since the bitmap is read in by read_tables(). */
void init_inode_bitmap (void)
{
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

/* ---------------------------------------------------------------------*/
void show_reserved_blocks(void) {
   int i,j;
                       /* superblock  */
   set_attr(0,AT_SUPER);
                       /* block and inode bitmaps */                    
   i = 1;
   for (j=0; j < Super.s_imap_zones ; j++)
      set_attr(i + j,AT_BITMAP);
   i += Super.s_imap_zones;  
   for (j=0; j < Super.s_zmap_zones ; j++)
      set_attr(i + j,AT_BITMAP);
                       
                       /* table of inodes */
   i += Super.s_zmap_zones;  
   for (j=0; j < UPPER(Super.s_ninodes*sizeof(struct d_inode),block_size); j++)
      set_attr(i + j,AT_INODE); 
                       /* space, reserved for a kernel */
   for (j=Super.s_firstkernzone; j < Super.s_firstdatazone; j++)
      set_attr(j,AT_KERNEL);

   display_map();
   display_legend(AT_DATA|AT_BITMAP|AT_INODE|AT_SUPER|AT_BAD|AT_KERNEL);
}
void show_super_stats(void) {
   char s[256];
   int i,free;
       
   if (!show && !voyer_mode)
            return;
   for (i=FIRSTZONE,free=0 ; i<ZONES ; i++)
        if (!zone_in_use(i))
    	        free++;
   sprintf (s, "%6ld block%s, %6d free (%ld%%)", 
            ZONES,  (ZONES  != 1) ? "s" : "",free,
            (100*free) / ZONES);
   add_comment(s);           
              
   for (i=1,free=0 ; i<INODES ; i++)
   	if (!inode_in_use(i))
   	        free++;
   sprintf (s,"%6ld inode%s, %6d free (%ld%%)", 
            INODES, (INODES != 1) ? "s" : "",free,
            (100*free) / INODES);
   add_comment(s);         
                      
   sprintf (s,"%ld blocks reserved for kernel", 
            Super.s_firstdatazone - Super.s_firstkernzone);
   add_comment(s);         
   display_comments("");           
}

