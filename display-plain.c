/*
 * display.c - all screen handling functions for the picture option of the
 * file system defragmenter.
 *
 * Copyright (C) 1997 Stephen Tweedie <sct@dcs.ed.ac.uk>
 */

#include <config.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>

#include "display.h"

int voyer_mode = 0;

#if 0 /* unused */
static void _die(char const *last_words) 
{
	fprintf(stderr,last_words);
	exit(1);
}
#endif

void init_screen(ulong blocks) 
{
  UNUSED(blocks);
}

void done_screen(int wait_key)
{
  UNUSED(wait_key);
}

void display_legend(ushort attr)
{
  UNUSED(attr);
}

void add_comment(char *comment)
{
	puts(comment);
}

void display_comments(char const *title)
{
  UNUSED(title);
}

void clear_comments(void)
{
}

void stat_line(const char *fmt, ...) 
{
	char s[256];
	va_list args;
	va_start(args,fmt);
	vsprintf(s,fmt,args);
	va_end(args);
	
	puts(s);
}

void set_attr(ulong block, ushort attr) 
{
  UNUSED(block);
  UNUSED(attr);
}

void clear_attr(ushort attr) 
{
  UNUSED(attr);
}

void update_display(void) 
{
}

void display_map(void) 
{
}
