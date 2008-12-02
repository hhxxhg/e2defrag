/*
 * display.c - all screen handling functions for the picture option of the
 * file system defragmenter.
 *
 * Copyright (C) 1997 Stephen Tweedie (sct@dcs.ed.ac.uk)
 * Copyright (C) 1997 Ulrich E. Habel (espero@b31.hadiko.de)
 * Copyleft (C) 1994 Alexey Vovenko (vovenko@ixwin.ihep.su)
 */

#include <config.h>
#include <sys/types.h>
#include <strings.h>
#include <stdlib.h>
#include <ncurses.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>

#include "display.h"

#ifdef FS_IS_ext       /* The use of ext fs is deprecated. */
   int voyer_mode = 0;
#else
   int voyer_mode = 1;
#endif

static WINDOW *map_w=NULL;
static WINDOW *legend=NULL,*stats=NULL, *st_line=NULL;

static int map_w_width;
static int map_w_height;
static ulong screen_cells,granularity;
static ushort *screen_map;

static void _die(char const *last_words) __attribute__((noreturn));

static void _die(char const *last_words)
{
   fprintf(stderr,last_words);
   exit(1);
}

/* Job control handler is broken in ncurses, so we have to set our
 * own.
 */
static RETSIGTYPE
tstp_signal(int dummy)
{
   UNUSED(dummy);

   endwin();
   signal(SIGTSTP,SIG_DFL);
   sigsetmask(0);
   /* Put us to stop */
   kill(getpid(),SIGTSTP);     
   
   /* Something has wakened us */
   signal(SIGTSTP,tstp_signal);
   wrefresh(curscr); 
}

void init_screen(ulong blocks)
{
   initscr();

   if (has_colors ()) {
     start_color ();

     curs_set (0);
     init_pair (1, COLOR_BLUE, COLOR_CYAN);
     attrset (CELL_ATTR (1));
     mvaddstr ( 5, 30, "                    ");
     mvaddstr ( 6, 30, " starting in Colour ");
     mvaddstr ( 7, 30, "                    ");
     refresh ();

     init_pair (ATR_SB,     COL_SB_FG,     COL_SB_BG);
     init_pair (ATR_GRP,    COL_GRP_FG,    COL_GRP_BG);
     init_pair (ATR_BITMAP, COL_BITMAP_FG, COL_BITMAP_BG);
     init_pair (ATR_INODE,  COL_INODE_FG,  COL_INODE_BG);
     init_pair (ATR_KERNEL, COL_KERNEL_FG, COL_KERNEL_BG);
     init_pair (ATR_DIR,    COL_DIR_FG,    COL_DIR_BG);
     init_pair (ATR_FILE,   COL_FILE_FG,   COL_FILE_BG);
     init_pair (ATR_BAD,    COL_BAD_FG,    COL_BAD_BG);
     init_pair (ATR_DATA,   COL_DATA_FG,   COL_DATA_BG);
     init_pair (ATR_FREE,   COL_FREE_FG,   COL_FREE_BG);
     init_pair (ATR_RD,     COL_RD_FG,     COL_RD_BG);
     init_pair (ATR_WR,     COL_WR_FG,     COL_WR_BG);
     init_pair (ATR_FRAG,   COL_FRAG_FG,   COL_FRAG_BG);

     napms (1000);
     erase ();
  }
   else { 
     curs_set (0);
     attrset (A_NORMAL);
     mvaddstr ( 6, 29, "starting in Monochrome");
      
     refresh ();
     napms (1000);
     erase ();
   }

   cbreak();
   noecho();

   keypad(stdscr,TRUE);
   if (LINES < 10 || COLS < 20) 
      _die("Unable to determine screen size, set LINES and COLUMNS in environment\n");
      
   if (COLS < 80)
      _die("Need at least 80-columns display to work\n");
   if (LINES < 15)
      _die("Need at least 15-lines display to work\n");
    
   map_w_width  = COLS;
   map_w_height = LINES - 7;
   map_w        = newwin (map_w_height,map_w_width,0,0);
   legend       = newwin (6,              0, LINES - 7, (COLS / 2) - 2);
   stats        = newwin (6, (COLS / 2) - 2, LINES - 7,              0);
   st_line      = newwin (0,              0, LINES - 1,              0);
   if (!map_w || !legend || !stats || !st_line) 
          _die("allocating screen map");
   map_w_width  -= 2;         /* border */
   map_w_height -= 2;         /* border */

   screen_cells = map_w_width*map_w_height;
   granularity = blocks / screen_cells;
   if (blocks % screen_cells) 
      granularity++;
   if (granularity == 0)
      granularity = 1;
   
   screen_cells = blocks / granularity;

   screen_map = malloc(screen_cells*sizeof(*screen_map));
   
   if (screen_map == NULL) {
      done_screen(FALSE);
      _die("Out of memory\n");
   }
   memset(screen_map, 0, screen_cells);
   signal(SIGINT,SIG_IGN);      /* ignore keyboard interrupt for safety */
   signal(SIGTSTP,tstp_signal);

   init_pair (1, COLOR_YELLOW, COLOR_BLUE);
   attrset (CELL_ATTR (1) | WA_BLINK);
   mvaddstr ( 5, 30, "                   ");
   mvaddstr ( 6, 30, "  Please  wait...  ");
   mvaddstr ( 7, 30, "                   ");
   refresh ();
}

void done_screen(int wait_key)
{
   if (!map_w)
        return;

   if (wait_key) {     
       stat_line("Press any key to quit"); 
      
       /* Deletion of window if getch() is used  ->  getchar() */
       getchar();
   }

   stat_line("");
   
   delwin(map_w);
   delwin(legend);        
   delwin(stats);        
   delwin(st_line);
   map_w = stats = st_line = NULL;

   curs_set (1);
   endwin ();
}

/* -------------------------- Legend window ------------------------------ */
static struct { 
  ushort attr;
  int    abbr;
  char const *name;
} legend_names[] = {
   { AT_SUPER,  'S', " Superblock" },
   { AT_GROUP,  'G', " Group dscr. " },
   { AT_BITMAP, 'M', " Bitmap " },
   { AT_INODE,  'I', " Inode block " },
   { AT_KERNEL, 'K', " Kernel " },
   { AT_DIR,    'D', " Directory" },
   { AT_REG,    'F', " File   " },
   { AT_BAD,    'B', " Bad block" },
   { AT_DATA, EMPTY, " Data block" },
   { 0,         '.', " Free block" },
   { AT_FRAG,   'F', " Fragment" },
   { 0,          0 , NULL          }
};

void display_legend(ushort attr)
{
   int  y = 1,
        x = 2,
        i = 0;

   while (legend_names[i++].abbr != EMPTY)
     ;
   legend_names[--i].abbr = ACS_DIAMOND;

   if (!voyer_mode) 
      return;

   box (legend, ACS_VLINE, ACS_HLINE);
   mvwaddstr (legend, 0, ((COLS + (2 * 2) + 1) / 4) - 4, " Legend ");
   
   for (i = 0; legend_names[i].name != NULL; i++) {
      if ((attr & legend_names[i].attr) || (legend_names[i].attr == 0)) {
	  wattrset (legend, CELL_ATTR ((i + 2)));
          mvwaddch (legend, y++, x, legend_names[i].abbr);
	  wattrset (legend, A_NORMAL);
	  waddstr  (legend,         legend_names[i].name);
	  
          if (y > 4) {
	      y = 1;
		
	      if (x < 10) 
		  x += 15;
	      else if (x < 22) {
		  x += 13;
	      } else      
		  break;
	  }
      }
   }     

   wrefresh (legend);
   wattrset (legend, A_NORMAL);
}

/* --------------------- Statistic window ------------------------------ */
#define MAX_COMMENTS 4
static int comments_count=0;
void add_comment(char *comment)
{
   if (stats==NULL)
       puts(comment);
   else    
       if (comments_count < MAX_COMMENTS) 
          mvwaddstr(stats,++comments_count,2,comment);
}
       
void display_comments (char const *title)
{
   int i;
   if (stats == NULL) return;
   box (stats, ACS_VLINE, ACS_HLINE);
   i = strlen (title) / 2;
   if (i > 0)
      mvwaddstr(stats, 0, ((COLS - (2 * 2) + 1) / 4) - i, title);
   if (comments_count < MAX_COMMENTS)
      mvwprintw(stats,++comments_count,2,"%6d block%s in each screen cell",
                granularity,granularity==1 ? "" : "s");
   wrefresh(stats);
}

void clear_comments (void)
{
   if (stats == NULL) return;
   comments_count = 0;
   wclear(stats);
}

/* -------------------------- Status line -------------------------------- */
void stat_line(const char *fmt, ...)
{
   char s[256];
   va_list args;
   va_start(args,fmt);
   vsprintf(s,fmt,args);
   va_end(args);
   if (st_line != NULL) {
        wmove(st_line,0,0);
        waddstr(st_line,s);
        wclrtoeol(st_line);
        wrefresh(st_line);
   }
   else
        puts(s);
}

/* ------------------------- Disk map window ----------------------------- */
void set_attr(ulong block, ushort attr)
{
   if (!voyer_mode) 
      return;
   block /= granularity;
   if (block > screen_cells) {
      fprintf(stderr,"set_map: block %lu out of range\n",block);
      return;
   }
   screen_map[block] |= attr; 
}

void clear_attr(ushort attr)
{
  if(!voyer_mode)
    return;
  switch(attr) {
  case AT_READ:
    {
                     /* Hack: If we have read the block than its place 
                      * becomes empty. This is not always correct,
                      * because each cell represents more than 1 disk block.
                      */
      unsigned i;
      for (i = 0; i < screen_cells; i++) 
          if (screen_map[i] & AT_READ) 
              screen_map[i] &= ~(AT_READ | AT_DATA);
      return;       
    }

  case AT_WRITE:
    {
      unsigned i;
      for (i = 0; i < screen_cells; i++) 
          if (screen_map[i] & AT_WRITE) {
              screen_map[i] &= ~(AT_WRITE);
              screen_map[i] |= AT_DATA; 
          }    
      return;       
    }

  default:
    {
      unsigned i;
      for (i = 0; i < screen_cells; i++) 
	screen_map[i] &= ~attr;
    }
    update_display();
  }
}

static void show_cell(int i)
{
  int mask = 0;
  int attr;
  int glyph;
  
  if (screen_map[i] & AT_SELECTED && 
     !(screen_map[i] & (AT_SUPER | AT_BITMAP | AT_INODE))) 
         mask |= A_REVERSE;

  /* A screen cell typically contains data for more than one block.
   * If the blocks are of different types we have to choose the
   * most important one to show it on the screen
   */
  if (screen_map[i] & AT_READ) {
      attr = CELL_ATTR (ATR_RD);
      glyph = 'R';
  }

  else if (screen_map[i] & AT_WRITE) {
      attr = CELL_ATTR (ATR_WR);
      glyph = 'W';
  }

  /* Normal status */
  else if (screen_map[i] & AT_GROUP) {
      attr = CELL_ATTR (ATR_GRP);
      glyph = 'G';
  }
  
  else if (screen_map[i] & AT_SUPER) {
      attr = CELL_ATTR (ATR_SB);
      glyph = 'S';
  }
  
  else if (screen_map[i] & AT_BITMAP) {
      attr = CELL_ATTR (ATR_BITMAP);
      glyph = 'M';
  }
  
  else if (screen_map[i] & AT_INODE) {
      attr = CELL_ATTR (ATR_INODE);
      glyph = 'I';
  }
   
  else if (screen_map[i] & AT_BAD) {
      attr = CELL_ATTR (ATR_BAD);
      glyph = 'B';
  }
   
  else if (screen_map[i] & AT_KERNEL) {
      attr = CELL_ATTR (ATR_KERNEL);
      glyph = 'K';
  }
   
  else if (screen_map[i] & AT_DATA) {
      attr = CELL_ATTR (ATR_DATA);
      glyph = ACS_DIAMOND;
  }

  else if (screen_map[i] & AT_DIR) {
      attr = CELL_ATTR (ATR_DIR);
      glyph = 'D';
  }
   
  else if (screen_map[i] & AT_REG) {
      attr = CELL_ATTR (ATR_FILE);
      glyph = 'F';
  }

  else {
      attr = CELL_ATTR (ATR_FREE);
      glyph = '.';
  }

  if ((screen_map[i] & AT_FRAG) &&
     !(screen_map[i] & (AT_SUPER | AT_BITMAP | AT_INODE))) 
         attr = CELL_ATTR (ATR_FRAG);

  wattrset (map_w, attr);
  waddch (map_w, glyph);
}
       
void update_display(void)
{
   unsigned i;
   int row = 0;
   if (!voyer_mode)
        return;
   for (i = 0; i < screen_cells; i++) {
        if ((i % map_w_width) == 0)  {
             wmove(map_w,++row,1);
        }
        show_cell(i);             
   }      
   wrefresh(map_w);
}

void display_map(void)
{
   if (!voyer_mode)
        return;
   box(map_w,ACS_VLINE,ACS_HLINE);
   update_display();
}



