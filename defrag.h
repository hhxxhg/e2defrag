/*
 * defrag.h - include file for the Linux file system degragmenter.
 * $Id: defrag.h,v 1.4 1997/08/17 14:02:31 linux Exp $
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

#ifdef NODEBUG
 #define debug 0
 #define assert(a)
#else
 extern int debug;
 #include <assert.h>
#endif

#include "types.h"

/* Imported from Adam Heath's Debian sources by Timshel Knoll,
 * 23/08/2000, fix for Debian bug#17136 */
#include <sys/param.h>


#if defined(FS_IS_ext) + defined(FS_IS_ext2) + defined(FS_IS_minix) + defined(FS_IS_xia) != 1
# error "Exactly one of FS_IS_ext, FS_IS_ext2, FS_IS_minix, FS_IS_xia must be defined."
#endif

#if FS_IS_ext
# include "ext.h"
#elif FS_IS_minix
# include "minix.h"
#elif FS_IS_ext2
# include "ext2.h"
#elif FS_IS_xia
# include "xia.h"
#endif

#include "display.h"

#define SYNC_PERIOD 500

extern char const * program_name, * version, * CVSID, * fsck;
extern char * device_name;
extern int IN; /* File descriptor of filesystem. */
extern int verbose, show, badblocks, readonly;
extern int blocks_until_sync;

extern int die_on_io_error;
extern int io_errors;

extern int changed; /* flags if the filesystem has been changed */

/* We omit a lot of the declarations when compiling misc.c, since
   misc.o is shared between extfs and minixfs versions, so we can't
   give correct types for a lot of fs-dependent stuff. */

#ifndef MISC_C

extern Block bad_block_inode;
# ifndef FS_IS_ext2
extern Block next_block_to_fill;
# endif
extern Block first_zone;
extern unsigned int zones, block_size;

extern struct d_inode inode_buffer;
extern int current_inode;
extern char super_block_buffer[];
extern int inode_table_offset;

/* n2d_map and d2n_map contain the zone movement maps.  Whenever a zone is
   moved, these maps are updated to allow the current occupant of a zone
   to be mapped to its original location, and the original location of any
   zone data to be mapped to its current location.

   When a zone was originally empty, the d2n_map will contain zero at
   that location to reflect the fact that there was no original occupant
   of that zone is not to be found.  The n2d_map will also be zero for
   blocks which do not currently hold any useful data and which may be
   overwritten freely.

   o2n_map contains the final translation from  old to new disc
   blocks, and is used to update inode block tables and indirection
   blocks.

   inode_map and fixed_map maintain flags recording which inodes are in
   use, and which blocks are unmovable, respectively.
*/

extern unsigned char * inode_map;
extern Block *inode_average_map;
extern signed char *inode_priority_map;
extern int *inode_order_map;

extern Block *n2d_map, *d2n_map, *o2n_map;

/* Manipulate the inode map */
#if FS_IS_ext2
  #define inode_in_use(x)	(bit (inode_map, (x-1)))
  #define mark_inode(x)	        (setbit (inode_map, (x-1)), changed = 1)
  #define unmark_inode(x)	(clrbit (inode_map, (x-1)), changed = 1)
#else   /* !FS_IS_ext2 */
  /* MINIX and XIAFS do use broken bitmaps, the least significant bit
   * is not used there.
   */
  #define inode_in_use(x)	(bit (inode_map, (x)))
  #define mark_inode(x)	        (setbit (inode_map, (x)), changed = 1)
  #define unmark_inode(x)	(clrbit (inode_map, (x)), changed = 1)
#endif /* !FS_IS_ext2 */

/* Manipulate the block translation maps */
#define n2d(x)		(n2d_map[(x)-first_zone])
#define d2n(x)		(d2n_map[(x)-first_zone])
#define o2n(x)		(o2n_map[(x)-first_zone])

#define unmark_zone(x)	(d2n(x) = 0, n2d(x) = 0, changed = 1)
#define zone_in_use(x)	(n2d(x) != 0)

/* Manipulate the fixed block map */
extern char * fixed_map;
#define zone_is_fixed(x) (bit (fixed_map, (x) - first_zone))
#define mark_fixed(x)	 (setbit (fixed_map, (x) - first_zone))

/* Miscellaneous function declarations */

/* The inline bitop functions... */

typedef struct Buffer Buffer;
struct Buffer
{
	BufferType btype : 1;
	unsigned int in_use : 1;
	unsigned int full : 1; /* contains data */
	Buffer *next;
	Block dest_zone;
	char data[0];
};


extern int pool_size;
extern Buffer *pool;

extern unsigned count_buffer_writes, count_buffer_reads;
extern int count_write_groups, count_read_groups;
extern int count_buffer_migrates, count_buffer_forces;
extern int count_buffer_read_aheads;

void read_buffer_data (Buffer *b);
void write_buffer_data_at (Buffer *b, Block dest);
void write_buffer_data (Buffer *b);

void init_buffer_tables (void);
void remap_disk_blocks (void);

int get_inode(int);
void put_inode(void);

void check_zone_nr (Block nr);
/* Read/write block currently at given address */
void read_current_block (Block nnr, char * addr);
void write_current_block (Block nnr, char * addr);
/* Read/write block originally at given address */
void read_old_block (Block onr, char *addr);
void write_old_block (Block onr, char *addr);

/* Prototypes for fs-specific portions */
void read_tables (void);
void write_tables (void);
void salvage_free_zones (void);
void init_zone_maps (void);
void init_inode_bitmap (void);
void show_reserved_blocks(void);
void show_super_stats(void);
int seek_to_inode(int); /* Returns zero iff succeeds. */

#endif /* !MISC_C */

#ifdef i386bogus

/* Use inline assembler to generate efficient bit operators */
#ifndef __GNUC__
#error "needs gcc for the bitop-__asm__'s"
#endif

#define bitop(name,op) \
extern inline int name(char * addr,unsigned int nr) \
{ \
	int __res; \
	__asm__ __volatile__("bt" op "l %1,%2; adcl $0,%0" \
		:"=g" (__res) \
		:"r" (nr),"m" (*(addr)),"0" (0)); \
	return __res; \
}

bitop(bit,"")
bitop(setbit,"s")
bitop(clrbit,"r")

#else /* ! i386bogus */

#define bitopt(name,cmd) \
extern int name (char * addr, unsigned int nr) \
{ \
	return (int) cmd; \
}

#define bit(field,bit) isset(field,bit)
#endif /* i386bogus */

/* Now for the prototypes for misc.c */
void check_mount(char *dev_name);
loff_t  nlseek (int fd, loff_t offset, int whence);
ssize_t nread  (int fd, __ptr_t buf, size_t nbytes);
ssize_t nwrite (int fd, __ptr_t buf, size_t nbytes);

/* Write msg to stderr (with program name and device name) and exit.
   msg should not end with a newline. */
void fatal_error (char const *msg) __attribute__((noreturn));
void usage (void) __attribute__((noreturn));
/* `__attribute__((noreturn))' lets gcc know that this doesn't return.
   Should perhaps be ifdef __GNUC__. */

#define die(str)	fatal_error(str)
#define nomore()	fatal_error("cannot continue.")
extern void io_error(const char *message);

#ifndef TRUE
# define TRUE 1
# define FALSE 0
#endif   

#include "llseek.h"

#ifndef UNUSED
# define UNUSED(_var) ((void) (_var))
	/* Just to suppress `unused parameter' warnings. */
#endif
