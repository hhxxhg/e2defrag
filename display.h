/*
 * display.h
 *
 * Function and colour attribute declarations for the full screen display.
 *
 * Copyright (C) 1997 Stephen Tweedie (sct@dcs.ed.ac.uk)
 * Copyright (C) 1997 Ulrich E. Habel (espero@b31.hadiko.de)
 * Copyleft (C) 1994 Alexey Vovenko (vovenko@ixwin.ihep.su)
 */

#ifndef INCLUDED__DISPLAY_H
#define INCLUDED__DISPLAY_H

     /* Screen cell types */
#define AT_DIR     0x1
#define AT_REG     0x2
#define AT_BAD     0x4
#define AT_GROUP   0x8
#define AT_SUPER   0x10
#define AT_KERNEL  0x20
#define AT_BITMAP  0x40
#define AT_INODE   0x80
#define AT_DATA    0x100
#define AT_READ    0x200
#define AT_WRITE   0x400

#define AT_FRAG     0x1000
#define AT_SELECTED 0x2000


     /* Replacement recognition */
#define EMPTY           '#'

     /* Colors used for partition-display */
#define COL_SB_FG       COLOR_RED      /* Superblock FG */
#define COL_SB_BG       COLOR_CYAN     /* Superblock BG */
#define COL_GRP_FG      COLOR_RED      /* Group      FG */
#define COL_GRP_BG      COLOR_CYAN     /* Group      BG */
#define COL_BITMAP_FG   COLOR_RED      /* Bitmap     FG */
#define COL_BITMAP_BG   COLOR_CYAN     /* Bitmap     BG */
#define COL_INODE_FG    COLOR_YELLOW   /* Inodes     FG */
#define COL_INODE_BG    COLOR_CYAN     /* Inodes     BG */
#define COL_KERNEL_FG   COLOR_RED      /* Kernel     FG */
#define COL_KERNEL_BG   COLOR_BLUE     /* Kernel     BG */
#define COL_DIR_FG      COLOR_YELLOW   /* Directory  FG */
#define COL_DIR_BG      COLOR_BLUE     /* Directory  BG */
#define COL_FILE_FG     COLOR_YELLOW   /* File       FG */
#define COL_FILE_BG     COLOR_BLUE     /* File       BG */
#define COL_FRAG_FG     COLOR_WHITE    /* Fragment   FG */
#define COL_FRAG_BG     COLOR_BLUE     /* Fragment   BG */
#define COL_BAD_FG      COLOR_RED      /* Bad-block  FG */
#define COL_BAD_BG      COLOR_BLUE     /* Bad-block  BG */
#define COL_DATA_FG     COLOR_YELLOW   /* Data-block FG */
#define COL_DATA_BG     COLOR_BLUE     /* Data-block BG */
#define COL_FREE_FG     COLOR_WHITE    /* Free-block FG */
#define COL_FREE_BG     COLOR_BLUE     /* Free-block BG */
#define COL_RD_FG       COLOR_GREEN    /* reading    FG */
#define COL_RD_BG       COLOR_BLACK    /* reading    BG */
#define COL_WR_FG       COLOR_RED      /* writing    FG */
#define COL_WR_BG       COLOR_BLACK    /* writing    BG */

/* Extra attribute characteristics */
#define ATR_IS_BOLD(x) (	\
	(x) == ATR_INODE ||	\
	(x) == ATR_FILE ||	\
	(x) == ATR_DATA ||	\
	(x) == ATR_DIR ||	\
	(x) == ATR_BAD ||	\
	(x) == ATR_FRAG ||	\
	(x) == ATR_RD ||	\
	(x) == ATR_WR)

#define CELL_ATTR(n) (COLOR_PAIR((n)) | (ATR_IS_BOLD(n) ? A_BOLD : 0))

/* Names for attribute sets */
enum {ATR_SB = 2,	/* Superblock */
      ATR_GRP,		/* Group descriptor */
      ATR_BITMAP,	/* Bitmap */
      ATR_INODE,	/* Inode block */
      ATR_KERNEL,	/* Kernel data */
      ATR_DIR,		/* Directory block */
      ATR_FILE,		/* File block */
      ATR_BAD,		/* Bad block */
      ATR_DATA,		/* Data block */
      ATR_FREE,		/* Free block */
      ATR_FRAG,		/* Fragmented block */
      ATR_RD,		/* Block being read */
      ATR_WR,		/* Block being written */
};


extern int voyer_mode;

void init_screen(ulong blocks);
void done_screen(int wait_key);

void set_attr(ulong block,ushort attr);
void clear_attr(ushort attr);
void display_map(void);
void update_display(void); /* Only map part is updated */

void display_legend(ushort attr);

void add_comment(char *text);
void display_comments(char const *title);
void clear_comments(void);

void stat_line(const char *s, ...);

#ifndef UNUSED
/* Just to suppress `unused parameter' warnings. */
# define UNUSED(_var) ((void) (_var))
#endif

#endif /* INCLUDED__DISPLAY_H */
