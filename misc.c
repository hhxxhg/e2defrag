/*
 * misc.c - miscellaneous functions for the Linux file system defragmenter.
 * $Id: misc.c,v 1.4 1997/08/17 14:02:33 linux Exp $
 *
 * Copyright (C) 1992, 1993 Stephen Tweedie (sct@dcs.ed.ac.uk)
 * 
 * This file may be redistributed under the terms of the GNU General
 * Public License.
 *
 */

#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mntent.h>

/* Added by Timshel Knoll, 23/08/2000 to fix compile error */
#include <errno.h>

#define MISC_C
#include "defrag.h"

void fatal_error (const char *msg)
{
	if (voyer_mode) { 
             stat_line("");             
             done_screen(FALSE); /* Maybe screen is not initalised yet, */
                                 /* done_screen will check it itself    */
        }
        fflush (stdout);
	fprintf (stderr, "%s (%s): %s\n", program_name, device_name, msg);
	exit (1);
}

void usage()
{
	fflush (stdout);
	fprintf (stderr, 
		 "Usage: %s [-V"
#ifndef NODEBUG
		 "d"
#endif
		 "nrsv] [-i inode-list] [-p pool-size] [-F file-prio] [-D dir-prio]"
		 "/dev/name\n", 
		 program_name);
	fprintf (stderr, "  -V : print full version information\n");
#ifndef NODEBUG
	fprintf (stderr, "  -d : debugging mode\n");
#endif
	fprintf (stderr, "  -r : read_only (testing) mode (implies -s)\n");
	fprintf (stderr, "  -s : show summary information\n");
	fprintf (stderr, "  -v : verbose (-vv is even more so)\n");
	fprintf (stderr, "  -n : runs without a picture\n");
	exit (1);
}

void check_mount(char *dev_name)
{
	FILE * f;
	struct mntent * mnt;

	if ((f = setmntent (MOUNTED, "r")) == NULL)
		return;
	while ((mnt = getmntent (f)) != NULL)
		if (strcmp (dev_name, mnt->mnt_fsname) == 0)
			break;
	endmntent (f);
	if (!mnt)
		return;

	fprintf (stderr,"Cannot work on a mounted device: %s\n", dev_name);
	exit (2);
}

loff_t nlseek (int fd, loff_t offset, int whence) {
   loff_t r;
   while ((r=defrag_llseek(fd,offset,whence))==((loff_t) -1) && errno == EINTR)
        ;
   return r;

}
ssize_t nread (int fd, __ptr_t buf, size_t nbytes) {
   ssize_t r;
   while ((r=read(fd,buf,nbytes))==-1 && errno == EINTR)
        ;
   return r;
}

ssize_t nwrite (int fd, __ptr_t buf, size_t nbytes) {
   ssize_t r;
   while ((r=write(fd,buf,nbytes))==-1 && errno == EINTR)
        ;
   return r;
}
