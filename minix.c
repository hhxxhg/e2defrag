/*
 * minix.c - minix-specific functions and data for the Linux file system 
 * degragmenter. 
 * $Id: minix.c,v 1.3 1997/08/10 16:32:10 linux Exp $
 *
 * This file is responsible for managing those data structures dependent
 * on the minix.  Specifically, it
 * + allocates space for the various disk maps stored in memory; 
 * + reads and writes superblock information; 
 * + reads the used-data-zones and used-inodes maps into memory
 *   (inode_map and d2n/n2d_map respectively); and, 
 * + once the defragmentation is complete, rewrites the new free-space
 *   map to disk.
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

#include <config.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defrag.h"

char const * program_name = "mdefrag";
char const * fsck = "fsck";

int inode_table_offset = 0;

/* A few static variables and functions, needed only within minix.c
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
	if (BLOCK_SIZE != nlseek (IN, BLOCK_SIZE, SEEK_SET))
		die ("seek failed");
	if (BLOCK_SIZE != nread (IN, super_block_buffer, BLOCK_SIZE))
		die ("unable to read super-block");
	if (MAGIC != MINIX_SUPER_MAGIC && 
	    MAGIC != MINIX_SUPER_MAGIC2)
		die ("bad magic number in super-block");
	if (ZONESIZE != 0 || BLOCK_SIZE != 1024)
		die("Only 1k blocks/zones supported");
	if (!IMAPS || IMAPS > MINIX_I_MAP_SLOTS)
		die("bad s_imap_blocks field in super-block");
	if (!ZMAPS || ZMAPS > MINIX_Z_MAP_SLOTS)
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

	if (NORM_FIRSTZONE != FIRSTZONE)
		printf ("Warning: Firstzone != Norm_firstzone\n");
	first_zone = FIRSTZONE;
	zones = ZONES;
	block_size = BLOCK_SIZE;
	bad_block_inode = 0;
}

/* Write the superblock, free zone/inode maps and inode table to disk */
void write_tables (void)
{
	if (debug)
		printf ("DEBUG: write_tables()\n");
	if (BLOCK_SIZE != nlseek (IN, BLOCK_SIZE, SEEK_SET))
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
   zone_in_use(n) is defined in defrag.h for this purpose. The minix
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


/* Read the map of used/unused data zones on disk.  The extfs stores a
   linked list of unused blocks.
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

/* Read in the map of used/unused inodes.  For the minix-fs, we don't
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
void show_reserved_blocks(void)
{
   int i;
                       /* superblock  */
   set_attr(0,AT_SUPER);
                       /* block and inode bitmaps */                    
   i = 1;
   {
     int j;
     for(j = 0; j < Super.s_imap_blocks; j++)
       set_attr( i + j, AT_BITMAP);
   }
   i += Super.s_imap_blocks;  
   {
     int j;
     for(j = 0; j < Super.s_zmap_blocks; j++)
       set_attr( i + j, AT_BITMAP);
   }

                       /* table of inodes */
   i += Super.s_zmap_blocks;
   {
     unsigned count = UPPER( Super.s_ninodes * sizeof(struct d_inode),
			     block_size);
     unsigned j;
     for(j = 0; j < count; j++)
       set_attr( i + j, AT_INODE);
   }
   display_map();
   display_legend(AT_DATA|AT_BITMAP|AT_INODE|AT_SUPER|AT_BAD);
}

void show_super_stats(void) {
        unsigned i, nfree;
	char s[256];

	if (!show && !voyer_mode)
                return;

        for (i=FIRSTZONE, nfree=0 ; i<ZONES ; i++)
                if (!zone_in_use(i))
    	                nfree++;
        sprintf (s, "%6ld block%s, %6d free (%ld%%)", 
                 ZONES,  (ZONES  != 1) ? "s" : "", nfree,
                 (100 * nfree) / ZONES);
        add_comment(s);           
              
        for (i=1, nfree=0 ; i<INODES ; i++)
   	        if (!inode_in_use(i))
   	                nfree++;
        sprintf (s,"%6ld inode%s, %6d free (%ld%%)", 
                 INODES, (INODES != 1) ? "s" : "", nfree,
                 (100 * nfree) / INODES);
        add_comment(s);         
	sprintf (s,"Firstdatazone=%ld (%ld)\n",FIRSTZONE,NORM_FIRSTZONE);
        add_comment(s);
        display_comments("");
}
