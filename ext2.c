/*
 * ext2.c - extfs-specific functions and data for the Linux file system
 * degragmenter.
 *
 * This file is responsible for managing those data structures dependent
 * on the extfs.  Specifically, it
 * + allocates space for the various disk maps stored in memory;
 * + reads and writes superblock information;
 * + reads the used-data-zones and used-inodes maps into memory
 *   (inode_map and d2n/n2d_map respectively); and,
 * + once the defragmentation is complete, rewrites the new free-space
 *   map to disk.
 *
 * Copyleft  (C) 1993 Alexey Vovenko (vovenko@ixwin.ihep.su)
 * Copyright (C) 1992, 1993, 1997 Stephen Tweedie (sct@dcs.ed.ac.uk)
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
#include <string.h>
#include <values.h>

#include "defrag.h"
#include "display.h"
#include "map.h"

#ifndef CHARBITS
# define CHARBITS 8
#endif

char const * program_name = "e2defrag";
char const * fsck = "e2fsck";

unsigned int groups;
struct ext2_group_desc *bg;
struct groups_population *gp;

/* Joined free blocks bitmap and manipulation routines */
static char *zone_map = NULL;

#define bm_zone_in_use(x) (bit(zone_map,(x)-FIRSTZONE))
#define bm_mark_zone(x) (setbit(zone_map,(x)-FIRSTZONE),changed=1)
#define bm_unmark_zone(x) (clrbit(zone_map,(x)-FIRSTZONE),changed=1)

extern Block journal_inode;

/** @postcondition E1: Super.s_inodes_per_group % 8 == 0.
    @postcondition E2: Super.s_blocks_per_group % 8 == 0.
**/
static void read_groups(void) {
   char s[132];

   assert( block_size != 0);

   bg = malloc(sizeof(struct ext2_group_desc)*groups);
   gp = malloc(sizeof(struct groups_population)*groups);

   if ((bg == NULL) || (gp==NULL))
      die("Out of memory allocating group descriptors");

   memset(gp,0,sizeof(struct groups_population)*groups);

   if(nlseek( IN, block_size * (Super.s_first_data_block + 1), SEEK_SET) < 0)
      die("Can't seek to the group descriptors");
   if(nread( IN, bg, sizeof(struct ext2_group_desc) * groups)
      != (ssize_t) (sizeof(struct ext2_group_desc) * groups))
     die( "Can't read group descriptors");

           /* die here, if any bitmap alignment problems exist */
           /* It's quite improbable, that bitmaps are not byte aligned */
   if ((Super.s_blocks_per_group % 8) != 0) {
      sprintf(s, "Strange blocks_per_group (%lu) value",
              (unsigned long) Super.s_blocks_per_group);
      fatal_error(s);
   }
   if ((Super.s_inodes_per_group % 8) != 0) {
      sprintf(s, "Strange inodes_per_group (%lu) value",
              (unsigned long) Super.s_blocks_per_group);
      fatal_error(s);
   }
}

/* Read the superblock and group description tables and reserve space
 * for the remaining disk map tables (inodes, inodes & block bitmaps) */
void read_tables (void)
{
    loff_t end_of_device;

    if (debug)
	printf ("DEBUG: read_tables()\n");
    if(SUPERBLOCK_OFFSET != nlseek( IN, SUPERBLOCK_OFFSET, SEEK_SET))
	die ("seek failed");
    if(SUPERBLOCK_SIZE != nread( IN, super_block_buffer, SUPERBLOCK_SIZE))
	die ("unable to read super-block");
    /* TODO:ENDIAN: Byte-swap Super here if necessary: see
       e2fsprogs/lib/ext2fs/openfs.c. */
    if (Super.s_magic != EXT2_SUPER_MAGIC)
  	die ("bad magic number in super-block");
    if (Super.s_log_block_size > 2)
      die( "Invalid blocksize >4k");

    /*
     * This modification is lousy, but better than the bug before  :-)
     */
    if(!(Super.s_state & EXT2_VALID_FS)
       && !readonly)
      die( "Please run fsck first, filesystem is not marked valid");

    if((Super.s_feature_ro_compat & ~(EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER
				      | EXT2_FEATURE_RO_COMPAT_LARGE_FILE
				      | EXT4_FEATURE_RO_COMPAT_HUGE_FILE
				      | EXT4_FEATURE_RO_COMPAT_DIR_NLINK
				      | EXT4_FEATURE_RO_COMPAT_GDT_CSUM
				      | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE))
       || (Super.s_feature_incompat & ~(EXT2_FEATURE_INCOMPAT_COMPRESSION
					| EXT4_FEATURE_INCOMPAT_FLEX_BG
					| EXT3_FEATURE_INCOMPAT_EXTENTS
					| EXT2_FEATURE_INCOMPAT_FILETYPE)))
      die("filesystem has unsupported features");
    if(Super.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)
      journal_inode = 8;
    block_size = EXT2_MIN_BLOCK_SIZE << Super.s_log_block_size;

    groups = (Super.s_blocks_count - Super.s_first_data_block +
	      Super.s_blocks_per_group - 1) /
	      Super.s_blocks_per_group;

    if(ZONES == 0xffffffff)
      die( "Found invalid filesystem block count 0xffffffff");
    if(ZONES < 20)
      die( "Found invalid filesystem block count (<20)");
    /* I don't know what the exact minimum is, but it's over 50 with
       mke2fs-1.18. */

    /*
     * Sanity check that llseek() is working OK if this is a large
     * (>2GB) partition.
     */
    end_of_device = ((loff_t) ZONES - 1) << Super.s_log_block_size;
    if(end_of_device >> Super.s_log_block_size != ZONES - 1)
      die( "loff_t not big enough");

    if(nlseek( IN, end_of_device, SEEK_SET) != end_of_device)
 	    die ("Error seeking to end of filesystem");

    read_groups();

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

    first_zone = FIRSTZONE;
    zones = ZONES;
    bad_block_inode = BAD_INO;
}

void show_super_stats(void) {
    char s[256];
    if (voyer_mode)
    {
	sprintf (s, "%6lu block%s, %6lu free (%ld%%)",
		 (unsigned long) ZONES, (ZONES  != 1) ? "s" : "",
		 (unsigned long) FREEBLOCKSCOUNT,
		 (100UL * FREEBLOCKSCOUNT) / ZONES);
	add_comment(s);
	sprintf (s, "%6lu inode%s, %6lu free (%ld%%)",
		 (unsigned long) INODES, (INODES != 1) ? "s" : "",
		 (unsigned long) FREEINODESCOUNT,
		 (100UL * FREEINODESCOUNT) / INODES);
	add_comment(s);
	sprintf (s, "%6u group%s,",
		 groups, (groups != 1) ? "s" : "");
	add_comment(s);
	display_comments("");
    }
    else if (show)
    {
	printf ("%lu inode%s\n", (unsigned long) INODES,
		(INODES != 1) ? "s" : "");
	printf ("%lu block%s\n", (unsigned long) ZONES,
		(ZONES != 1) ? "s" : "");
	printf ("%u bad block%s\n", (unsigned int) badblocks,
		(badblocks != 1) ? "s" : "");
	printf ("Firstdatazone=%lu\n", (unsigned long) FIRSTZONE);
	printf ("%lu free block%s\n", (unsigned long) FREEBLOCKSCOUNT,
		(FREEBLOCKSCOUNT != 1) ? "s" : "");
	printf ("%lu free inode%s\n", (unsigned long) FREEINODESCOUNT,
		(FREEINODESCOUNT != 1) ? "s" : "");
	printf ("Zonesize=%u\n", (unsigned int) (EXT2_MIN_BLOCK_SIZE << Super.s_log_block_size));
    }
}

/* Read in the map of used/unused inodes.
 */
void init_inode_bitmap (void)
{
  unsigned i, size;
  loff_t pos;

  if (debug)
    printf ("DEBUG: init_inode_bitmap()\n");

  assert( (Super.s_inodes_per_group & 7) == 0);
  /* Loose proof: checked for in read_groups (@E1).
     Relevance: Otherwise the below should round up. */

  size = Super.s_inodes_per_group >> 3;
  /* = number of bytes per inode bitmap. */

  inode_map = malloc (size * groups);
  if (!inode_map)
    die ("Unable to allocate inodes bitmap\n");
  memset (inode_map, 0, ((INODES + 1) / 8));

  for (i = 0; i < groups; i++) {
    if( bg[i].bg_flags & EXT2_BG_INODE_UNINIT ) {
      if( debug )
	printf( "Group:%d has uninitialized inode bitmap, skipping\n", i );
      memset( inode_map + size * i, 0, size );
    }
    else {
      pos = (loff_t) bg[i].bg_inode_bitmap * block_size;
      if (debug)
	printf("Group:%d inode_bitmap at:%llu\n",i,(unsigned long long)pos);
      if (pos!=nlseek(IN,pos,SEEK_SET))
	die("seek failed reading inode bitmap");
      if(nread( IN, inode_map + size * i, size) != (ssize_t) size)
	die("error reading inode bitmap");
    }
  }
}

static void mark_fixed_blocks(Block block, unsigned count)
{
   unsigned i;
   if (debug)
        printf( "Mark unmovable: start=%lu count=%u\n",
		(unsigned long) block, count);

   /* effic: mark_fixed is a set_bit operation; should set many bits
      per write operation. */
   for (i=0; i < count; i++) {
        mark_fixed(block+i);
   }
}


#ifndef MIN
# define MIN(_a, _b) ((_a) < (_b) ? (_a) : (_b))
#endif
#define MIN3(_d, _e, _f) \
  ((_d) < (_e) \
   ? MIN((_d), (_f)) \
   : MIN((_e), (_f)))


/* Abstractions for determining whether a given group has a copy of the
   superblock & group descriptors.  This abstraction is intended for if
   you are iterating over all groups, starting at zero.

   Accessible things:

     DCL_NEXT_SB_GRP: necessary declarations.

     NEXT_SB_GRP: next group (starting with zero) that has a superblock copy.

     INC_NEXT_SB_GRP: Statement to execute whenever you get to NEXT_SB_GRP.
*/

/* For filesystems without the sparse_super feature, every group has a copy; for
   filesystems with the sparse_super feature, only groups 0, 1, and groups that
   are a power of either 3, 5 or 7, have a copy. */

#define DCL_NEXT_SB_GRP \
  unsigned next_sb_grp_work[4] = {0, 3, 5, 7}

#define NEXT_SB_GRP next_sb_grp_work[0]

#define INC_NEXT_SB_GRP inc_next_sb_grp( next_sb_grp_work)

#define next_power3 next_sb_grp_work[1]
#define next_power5 next_sb_grp_work[2]
#define next_power7 next_sb_grp_work[3]

static void inc_next_sb_grp(unsigned next_sb_grp_work[])
{
  if(!(Super.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER)
     || (NEXT_SB_GRP == 0))
    NEXT_SB_GRP++;
  else if(NEXT_SB_GRP == 1)
    NEXT_SB_GRP = 3;
  else {
    if(NEXT_SB_GRP == next_power3)
      next_power3 *= 3;
    else if(NEXT_SB_GRP == next_power5)
      next_power5 *= 5;
    else {
      assert( NEXT_SB_GRP == next_power7);
      next_power7 *= 7;
    }
    NEXT_SB_GRP = MIN3( next_power3, next_power5, next_power7);
  }
}
#undef next_power3
#undef next_power5
#undef next_power7


static void mark_group_zones(void)
{
  /* superblock + blocks occupied by group descriptors */
  unsigned const sb_count
    = 1 + UPPER( sizeof(struct ext2_group_desc) * groups, block_size);
  unsigned const blk_bmap_count
    = UPPER( Super.s_blocks_per_group, CHARBITS * block_size);
  unsigned const ino_bmap_count
    = UPPER( Super.s_inodes_per_group, CHARBITS * block_size);
  unsigned const ino_tbl_count
    = UPPER((Super.s_inodes_per_group * EXT2_INODE_SIZE(&Super)),block_size);
  unsigned i;
  DCL_NEXT_SB_GRP;

  Block first_block = Super.s_first_data_block;
  for(i = 0; i < groups; i++) {
    if(i == NEXT_SB_GRP) {
      /* Copy of superblock and group descriptor. */
      mark_fixed_blocks( first_block, sb_count);

      INC_NEXT_SB_GRP;
    }

    /* Block bitmap. */
    mark_fixed_blocks( bg[i].bg_block_bitmap, blk_bmap_count);
    /* Inode bitmap */
    mark_fixed_blocks( bg[i].bg_inode_bitmap, ino_bmap_count);
    /* Inode table */
    mark_fixed_blocks( bg[i].bg_inode_table, ino_tbl_count);

    first_block += Super.s_blocks_per_group;
  }
}

/* Count free blocks in each group using block allocation bitmaps.
 * We do not use bg_free_blocks_count, I'm not sure they are actually correct.
 * Blocks, reserved for superuser sometimes are not counted in
 * bg_free_blocks_count. They are represented however in the
 * s_free_blocks_count. (10-dec-93)
 */
/* (14-dec-93). bg_free_blocks sometimes are not correct, but I'm sure, that
 * there was a situation with wrong counts and the new e2fsck told nothing
 * about it. Let's see if this is true.
 * Yes it does. I've managed to spoil file system: 0 blocks are actually
 * free, bitmap is ok, but bg_free_blocks says "76". Older versions of
 * e2fsck are buggy.  (This is fixed in all recent versions of e2fsck.)
 */

static void count_free_blocks(void)
{
   unsigned n;
   ulong total_free = 0;
   ulong blocks;
   Block first_block = Super.s_first_data_block;

   for (n = 0; n < groups; n++) {
       ulong count = 0;
       unsigned i;

       blocks = Super.s_blocks_per_group;
       if (n == groups - 1) {
	 ulong tmp = (ZONES - FIRSTZONE) % Super.s_blocks_per_group;
	 if(tmp)
	   blocks = tmp;
       }

       for (i = 0; i < blocks; i++)
	 /* effic: This bit-counting can be faster. */
	 if(!bm_zone_in_use( first_block + i))
               count++;
       gp[n].free_blocks = count;
       total_free += count;
       if (debug) printf("Group %d, free blocks %lu\n", n, count);
       if (count != bg[n].bg_free_blocks_count)
          fprintf(stderr, "Group %d: free_blocks:%u counted:%lu\n", n,
                  bg[n].bg_free_blocks_count, count);

       first_block += Super.s_blocks_per_group;
   }
   if (debug) printf("Total free blocks counted: %lu\n", total_free);
   if (FREEBLOCKSCOUNT != total_free) {
       char s[256];
       sprintf (s, "Free blocks count wrong, free:"
		"%lu, reserved:%lu, counted:%lu\nRun fsck.\n",
		(unsigned long) FREEBLOCKSCOUNT,
		(unsigned long) Super.s_r_blocks_count, total_free);
       fatal_error(s);
   }
}

/* Read the map of used/unused data zones on disk.
 * The map is held jointly in d2n_map and n2d_map, described in
 * defrag.h.  These are initialised to the identity map (d2n(i) = n2d(i)
 * = i), and then the free zone list is scanned, and all unused zones
 * are marked as zero in both d2n_map and n2d_map.
 * Mark blocks occupied by group bitmaps and inode tables as unmovable.
 */

void init_zone_maps (void)
{
  Block i;
  unsigned n;
  ulong size;
  loff_t pos;

  assert( (Super.s_blocks_per_group & 7) == 0);
  /* Loose proof: checked by read_groups (@E2).
     Relevance: Otherwise the below should round up. */

  size = Super.s_blocks_per_group >> 3;
  /* The last group on the disk can be shorter,
   * that's why groups * s_blocks_per_group >= s_blocks
   */
  zone_map = malloc (size*groups);
  if (!zone_map)
    die ("Unable to allocate zone bitmap\n");

  for (n = 0; n < groups; n++) {
    if( bg[n].bg_flags & EXT2_BG_BLOCK_UNINIT ) {
      if( debug )
	printf( "Group:%d has uninitialized block bitmap, skipping\n", n );
      memset( zone_map + size * n, 0, size );
    } else {
      pos = (loff_t) bg[n].bg_block_bitmap * block_size;
      if (pos!=nlseek(IN,pos,SEEK_SET))
	die("seek failed reading zone bitmap");
      else if(nread( IN, zone_map + size * n, size) != (ssize_t) size)
	die("error reading zone bitmap");
    }
  }

  if (debug)
    printf ("DEBUG: init_zone_maps()\n");
  for (i = FIRSTZONE; i < ZONES; i++)
    {
      if (bm_zone_in_use(i))
	{
	  map_forward_set (i, i);
	}
    }
  count_free_blocks();
  mark_group_zones();
}

/* Write the superblock inode bitmaps to disk */
void write_tables (void)
{
	if (debug)
		printf ("DEBUG: write_tables()\n");
	if(SUPERBLOCK_OFFSET != nlseek( IN, SUPERBLOCK_OFFSET, SEEK_SET))
		die ("seek failed in write_tables");
	/* TODO:ENDIAN: Swab the superblock here if necessary. */
	if(SUPERBLOCK_SIZE != nwrite( IN, super_block_buffer, SUPERBLOCK_SIZE))
		die ("unable to write super-block");
/* Inodes are not actually moved in the current version - nothing to write */
}

// adapted from e2fsprogs

/** CRC table for the CRC-16. The poly is 0x8005 (x16 + x15 + x2 + 1) */
static __u16 const crc16_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

/**
 * Compute the CRC-16 for the data buffer
 *
 * @param crc     previous CRC value
 * @param buffer  data pointer
 * @param len     number of bytes in the buffer
 * @return        the updated CRC value
 */
__u16 ext2fs_crc16(__u16 crc, const void *buffer, unsigned int len)
{
	const unsigned char *cp = buffer;

	while (len--)
		/*
		 * for an unknown reason, PPC treats __u16 as signed
		 * and keeps doing sign extension on the value.
		 * Instead, use only the low 16 bits of an unsigned
		 * int for holding the CRC value to avoid this.
		 */
		crc = (((crc >> 8) & 0xffU) ^
		       crc16_table[(crc ^ *cp++) & 0xffU]) & 0x0000ffffU;
	return crc;
}

static void update_group_desc_csum(__u32 n)
{
  __u16 crc = 0;

  if (Super.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) {
    crc = ext2fs_crc16(~0, Super.s_uuid,
		       sizeof(Super.s_uuid));
    crc = ext2fs_crc16(crc, &n, sizeof(n));
    crc = ext2fs_crc16(crc, &bg[n], 30);
    bg[n].bg_checksum = crc;
  }
}

/* Rewrite the free zone map on disk.  The defragmentation procedure
   will migrate all free blocks to the end of the disk partition, and so
   after defragmentation the free space map must be updated to reflect
   this. Free zones are determined by n2d_map, the macro zone_in_use(n)
   is defined in defrag.h for this purpose. The ext2fs stores the free
   zone map as a number of bitmaps, one in each group.
 */
void salvage_free_zones (void)
{
  ulong bmp_zones;     /* Number of zones defined by bitmap size,
			  can be larger than the actual zone count. */
  loff_t pos;

  assert( (Super.s_blocks_per_group & 7) == 0);
  /* Loose proof: checked for by read_groups (@E2).
     Relevance: otherwise size calculation and memset should round up. */

  bmp_zones = groups * Super.s_blocks_per_group;

  if(verbose)
    stat_line( "Salvaging free zones list ...\n");

  /* Init counts of free blocks per group. */
  {
    unsigned n;
    for(n = 0; n < groups; n++)
      bg[n].bg_free_blocks_count = 0;
  }

  memset( zone_map, 0, bmp_zones / CHARBITS);

  /* Scan zone_in_use and zone_is_fixed bitmaps, copying to zone_map; and
     simultaneously update bg_free_blocks_count's. */
  {
    Block next_bg_blk = Super.s_first_data_block;
    unsigned n = ~0u;
    Block blk;

    for(blk = next_bg_blk; blk < ZONES; blk++) {
      if(blk == next_bg_blk) {
	next_bg_blk += Super.s_blocks_per_group;
	n++;
      }
      if (zone_in_use(blk) || zone_is_fixed(blk))
	bm_mark_zone(blk);
      else
	bg[n].bg_free_blocks_count++;
    }
  }

  /* Padding to the end of bitmap. */
  {
    Block blk;

    for(blk = ZONES; blk < bmp_zones + FIRSTZONE; blk++)
      bm_mark_zone(blk);
  }

  /* Write the block bitmaps. */
  {
    ulong size = Super.s_blocks_per_group >> 3;
    unsigned n;

    for(n = 0; n < groups; n++) {
      pos = (loff_t) bg[n].bg_block_bitmap * block_size;
      if( Super.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM &&
	  bg[n].bg_free_blocks_count == Super.s_blocks_per_group &&
	  n != groups-1 ) /* fsck doesn't like the last group being uninitialized */
	bg[n].bg_flags |= EXT2_BG_BLOCK_UNINIT;
      else bg[n].bg_flags &= ~EXT2_BG_BLOCK_UNINIT;
      if( !(bg[n].bg_flags & EXT2_BG_BLOCK_UNINIT) ) {
	if(pos != nlseek( IN, pos, SEEK_SET))
	  die("seek failed writing zone bitmap");

	if(nwrite( IN, zone_map + size * n, size) != (ssize_t) size)
	  die("error writing zone bitmap");
      }
      update_group_desc_csum( n );
    }
  }

  /* Write the group descriptors. */
  {
    /* TODO: Shouldn't we be writing a copy of this after every superblock copy
       too? */
    if(nlseek( IN, block_size * (Super.s_first_data_block + 1), SEEK_SET) < 0)
      die( "Can't seek to the group descriptors");
    if(nwrite( IN, bg, sizeof(struct ext2_group_desc) * groups)
       != (ssize_t) (sizeof(struct ext2_group_desc) * groups))
      die( "Can't write group descriptors");
  }
}

int seek_to_inode(int i)
{
  struct ext2_group_desc *p;

  i--;
  assert( i >= 0);
  p = &bg[i / Super.s_inodes_per_group];

  if(nlseek( IN,
	     ((loff_t) p->bg_inode_table * block_size
	      + EXT2_INODE_SIZE(&Super) * (i % Super.s_inodes_per_group)),
	     SEEK_SET)
     >= 0)
    return 0;
  else {
    io_error( "Can't seek to inode table");
    return 1;
  }
}

void update_group_population(ulong znr, enum walk_zone_mode mode, ulong inode)
{
   unsigned i_group = (inode-1) / Super.s_inodes_per_group;
   unsigned z_group = (znr - Super.s_first_data_block) / Super.s_blocks_per_group;

   assert(i_group < groups);
   assert(z_group < groups);

   if (mode == WZ_FIXED_BLOCKS) {
       gp[z_group].native_blocks++;     /* Count fixed blocks as native */
       return;
   }
   if (mode != WZ_SCAN)
       return;
   if (i_group == z_group)
       gp[i_group].native_blocks++;
   else {
       gp[z_group].wants_away++;
       gp[i_group].wants_back++;
   }
}

/* The main problem with groups in is that their population is not
 * even.  The current allocation strategy tries to balance the number
 * of allocated inodes and the total number of blocks in each group.
 * This does not necessarily mean that the number of blocks owned by
 * native inodes are equal in each group. In fact some of the groups
 * are overpopulated and some are not.
 *
 * When we deal with an overpopulated group, we have to put its blocks
 * into some other groups.  So we have to know how many blocks we can
 * put into not-yet filled group without forcing out the native
 * blocks.
 */
void check_group_population(void)
{
   unsigned i;
   Block first_block = Super.s_first_data_block;

   if (verbose > 1) stat_line("Checking groups population...\n");
   for (i=0; i < groups; i++) {

       /* Print data about the present situation in the groups */
       if (verbose > 2 && !voyer_mode) {
         printf( "Group:%u   native:%lu(+%lu), foreign:%lu, free:%lu\n", i,
                 gp[i].native_blocks, gp[i].wants_back,
                 gp[i].wants_away, gp[i].free_blocks);
         printf( "\t total:%lu\n", (unsigned long)
		 gp[i].native_blocks + gp[i].wants_away + gp[i].free_blocks);
       }

                  /* Now let's estimate how many free blocks we are going
                   * to have after the defragmentation
                   */

       gp[i].free_blocks += gp[i].wants_away;
       if (gp[i].free_blocks > gp[i].wants_back)
	 gp[i].free_blocks -= gp[i].wants_back;
       else
	 gp[i].free_blocks = 0;
       gp[i].native_blocks = 0;
       gp[i].wants_away = 0;

       gp[i].next_block_to_fill = first_block;
       first_block += Super.s_blocks_per_group;
       if (i != groups-1)
	 gp[i].last_block = first_block - 1;
       else
	 gp[i].last_block = ZONES - 1;
   }
}

static ulong try_other_groups(ulong inode, unsigned native_group)
{
  unsigned i;
  /* look for free place in groups below the native one */
  for(i = native_group; i--;) {
    if (debug)
      printf("try_other_groups, gr:%u,  free:%lu, next:%lu last:%lu\n",
	     i,gp[i].free_blocks,gp[i].next_block_to_fill,gp[i].last_block);
    if (gp[i].free_blocks==0)
      continue;
    if (gp[i].next_block_to_fill > gp[i].last_block)
      continue;
    while (zone_is_fixed(gp[i].next_block_to_fill))
      if (++gp[i].next_block_to_fill > gp[i].last_block)
	continue;
    gp[i].free_blocks--;
    gp[i].wants_away++;
    return gp[i].next_block_to_fill++;
  }
  /* and if all groups below the native are filled in, then */
  /* look for free space in all other groups */
  if( native_group == 0 )
    native_group--; /* forcing left */
  for (i=native_group+1; i < groups; i++) {
    if (debug)
      printf("try_other_groups, gr:%u,  free:%lu, next:%lu last:%lu\n",
	     i,gp[i].free_blocks,gp[i].next_block_to_fill,gp[i].last_block);
    if( native_group != -1 && gp[i].free_blocks==0 )
      continue;
    if (gp[i].next_block_to_fill > gp[i].last_block)
      continue;
    while (zone_is_fixed(gp[i].next_block_to_fill))
      if (++gp[i].next_block_to_fill > gp[i].last_block)
	continue;
    if( gp[i].free_blocks )
      gp[i].free_blocks--;
    gp[i].wants_away++;
    return gp[i].next_block_to_fill++;
  }

  printf("Can't find a block for inode %lu\n",inode);
  for (i=0; i < groups; i++)
    printf("Group:%u, native:%lu,foreign:%lu\n",i,
	   gp[i].native_blocks,gp[i].wants_away);
  die("Internal problem\n");
}

/* Try to allocate block in its group and if this fails, than in any other
 * group with sufficient free space
 */

ulong choose_block(ulong inode)
{
  unsigned i_group = (inode-1) / Super.s_inodes_per_group;

  if( inode_priority_map[inode] > 0 )
    return try_other_groups( inode, 0 ); /* force to left most block group */
  if (gp[i_group].next_block_to_fill > gp[i_group].last_block)
    return try_other_groups(inode,i_group);

  while (zone_is_fixed(gp[i_group].next_block_to_fill)) {
    if (++gp[i_group].next_block_to_fill > gp[i_group].last_block)
      return try_other_groups(inode,i_group);
  }
  gp[i_group].native_blocks++;
  return gp[i_group].next_block_to_fill++;
}

int gp_stack_count;

struct groups_population *push_group_population()
{
  struct groups_population *ogp;

  gp_stack_count++;
  ogp = malloc(sizeof(struct groups_population)*groups);
  if (!ogp)
    die ("Out of memory");
  memcpy (ogp, gp, sizeof(struct groups_population)*groups);
  return ogp;
}

void pop_group_population (struct groups_population *ogp)
{
  gp_stack_count--;
  memcpy (gp, ogp, sizeof(struct groups_population)*groups);
  free (ogp);
}

/* ---------------------------------------------------------------------*/
void show_reserved_blocks(void)
{
  unsigned i;
  Block first_block = Super.s_first_data_block;
  DCL_NEXT_SB_GRP;

  for(i=0; i < groups; i++) {
    if(i == NEXT_SB_GRP) {
      /* A copy of superblock. */
      set_attr( first_block, AT_SUPER);

      /* A copy of group descriptors. */
      {
	unsigned j;
	unsigned count = UPPER( groups * sizeof(struct ext2_group_desc),
				block_size);
	for(j = 1; j <= count; j++)
	  set_attr( first_block + j, AT_GROUP);
      }

      INC_NEXT_SB_GRP;
    }

    /* Block bitmaps. */
    {
      unsigned count = UPPER( Super.s_blocks_per_group, CHARBITS * block_size);
      unsigned j;
      for(j = 0; j < count; j++)
	set_attr( bg[i].bg_block_bitmap + j, AT_BITMAP);
    }

    /* Inode bitmaps. */
    {
      unsigned count = UPPER( Super.s_inodes_per_group, CHARBITS * block_size);
      unsigned j;
      for(j = 0; j < count; j++)
	set_attr( bg[i].bg_inode_bitmap + j, AT_BITMAP);
    }

    /* Table of inodes. */
    {
      unsigned count = UPPER( Super.s_inodes_per_group,
			      (block_size / sizeof(struct ext2_inode)));
      unsigned j;
      for(j = 0; j < count; j++)
	set_attr( bg[i].bg_inode_table + j, AT_INODE);
    }

    first_block += Super.s_blocks_per_group;
  }
  display_map();
  display_legend( AT_DATA|AT_BITMAP|AT_INODE|AT_SUPER|AT_GROUP|AT_BAD);
}
