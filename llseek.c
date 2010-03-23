/*
 * llseek.c -- stub calling the llseek system call
 *
 * Copyright (C) 1994, 1995, 1996, 1997 Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

/* Taken from e2fsprogs-19/lib/ext2fs/llseek.c.
 * Modifications for defrag:
 *
 *   o #include <config.h>
 *
 *   o Add whitespace to #-directives, for clarity.
 *
 *   o s/ext2fs_llseek/defrag_llseek/g
 *
 *   o s/ext2_loff_t/defrag_loff_t/g
 *
 *   o #define defrag_loff_t loff_t
 *
 *   o #include "llseek.h", which provides loff_t.
 */

#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

#include <config.h>

#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#if HAVE_ERRNO_H
# include <errno.h>
#endif
#if HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef __MSDOS__
# include <io.h>
#endif

#include "llseek.h"
#define defrag_loff_t loff_t

#ifdef __linux__

# if defined(HAVE_LSEEK64) && defined(HAVE_LSEEK64_PROTOTYPE)

#  define my_llseek lseek64

# elif defined(HAVE_LLSEEK)
#  include <syscall.h>

#  ifndef HAVE_LLSEEK_PROTOTYPE
extern long long llseek (int fd, long long offset, int origin);
#  endif

#  define my_llseek llseek

# else	/* ! HAVE_LLSEEK && !(LSEEK64 stuff) */

#  ifdef __alpha__ /* TODO: Should this test SIZEOF_LONG instead? */

#   define llseek lseek

#  else /* !__alpha__ */

#   include <linux/unistd.h>

#   ifndef __NR__llseek
#    define __NR__llseek            140
	/* 140 is true for most architectures in Linux.  But hopefully
	 * the correct value was already defined by linux/unistd.h.
	 */
#   endif

#   ifndef __i386__
static int _llseek (unsigned int, unsigned long,
		   unsigned long, defrag_loff_t *, unsigned int);

static _syscall5(int,_llseek,unsigned int,fd,unsigned long,offset_high,
		 unsigned long, offset_low,defrag_loff_t *,result,
		 unsigned int, origin)
#   endif

static defrag_loff_t my_llseek (int fd, defrag_loff_t offset, int origin)
{
	defrag_loff_t result;
	int retval;

#   ifndef __i386__
	retval = _llseek(fd, ((unsigned long long) offset) >> 32,
#   else			  
	retval = syscall(__NR__llseek, fd, (unsigned long long) (offset >> 32),
#   endif
			  ((unsigned long long) offset) & 0xffffffff,
			&result, origin);
	return (retval == -1 ? (defrag_loff_t) retval : result);
}

#  endif	/* __alpha__ */

# endif /* HAVE_LLSEEK */

defrag_loff_t defrag_llseek (int fd, defrag_loff_t offset, int origin)
{
	defrag_loff_t result;
	static int do_compat = 0;

	if ((sizeof(off_t) >= sizeof(defrag_loff_t)) ||
	    (offset < ((defrag_loff_t) 1 << ((sizeof(off_t)*8) -1))))
		return lseek(fd, (off_t) offset, origin);

	if (do_compat) {
		errno = EINVAL;
		return -1;
	}
	
	result = my_llseek (fd, offset, origin);
	if (result == -1 && errno == ENOSYS) {
		/*
		 * Just in case this code runs on top of an old kernel
		 * which does not support the llseek system call
		 */
		do_compat++;
		errno = EINVAL;
	}
	return result;
}

#else /* !linux */

# ifndef EINVAL
#  define EINVAL EXT2_ET_INVALID_ARGUMENT
# endif

defrag_loff_t defrag_llseek (int fd, defrag_loff_t offset, int origin)
{
	if ((sizeof(off_t) < sizeof(defrag_loff_t)) &&
	    (offset >= ((defrag_loff_t) 1 << ((sizeof(off_t)*8) -1)))) {
		errno = EINVAL;
		return -1;
	}
	return lseek (fd, (off_t) offset, origin);
}

#endif 	/* linux */
