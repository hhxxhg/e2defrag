/* frag.c - simple fragmentation checker */

#include <config.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if HAVE_DIRENT_H
# include <dirent.h>
#elif HAVE_SYS_NDIR_H
# include <sys/ndir.h>
#elif HAVE_SYS_DIR_H
# include <sys/dir.h>
#elif HAVE_NDIR_H
# include <ndir.h>
#endif
#include <sys/stat.h>
#include <sys/ioctl.h>


#define FIBMAP 1


typedef struct StackElem {
    struct StackElem *backref, *next;
    char name[NAME_MAX];
    char dir_seen;
} StackElem;

StackElem *top = NULL;

#ifndef HAVE_ABS
static inline int abs (int n)
{
    return (n>=0) ? n : -n;
}
#endif

static void discard( void )
{
    StackElem *se = top;
    if( se == NULL )
	return ;
    top = se->next;
    free(se);
}

static void push( StackElem * se )
{
    se -> next = top;
    top = se;
}

static char *p2s( StackElem *se, char *path )
{
    char *s;
    if( se->backref!=NULL ) {
	path = p2s( se->backref, path );
	if( path[-1]!='/' )
	    *path++ = '/';
    }
    s = se->name;
    while( *s )
	*path++ = *s++;
    return path;
}

static char *path2str( StackElem *se, char *path )
{
    *(p2s( se, path ))=0;
    return path;
}

static void *xmalloc( size_t size )
{
    void *p;
    if( (p=malloc(size))==NULL ) {
	fprintf(stderr,"\nvirtual memory exhausted.\n");
	exit(1);
    }
    return p;
}

int main(int argc,char **argv)
{
    int fd,last,frags,blocks,block, this_frag, frag_left, largest_frag, 
	used_blocks, frag_start;
    long sum_blocks=0, sum_frag_blocks=0, sum_files=0,
	sum_frag_files=0, sum_frags=0,
	n_gaps=0, sum_gaps=0;
    long sum_used_blocks=0;
    struct stat st;
    StackElem *se, *se1;
    char path[PATH_MAX], *p;
    DIR *dir;
    struct dirent *de;
    char silent_flag=0;
    int long_listing = 0;
    
    if (argc < 2) {
        fprintf(stderr,"usage: %s [-l] [-s [-s]] filename ...\n",argv[0]);
        exit(1);
    }
    argc--; argv++;
    while (argc>0) {
	p = *argv;
	if( *p=='-' )
	    while( *++p )
	    switch( *p ){
	      case 's':
		silent_flag++; /* may be 1 or 2 */
		break;
  	      case 'l':
	        long_listing++;
		break;
	      default:
		fprintf(stderr,"\nunknown flag %c\n", *p );
		exit(1);
	    }
	else {
	    se = xmalloc( sizeof(StackElem) );
	    se->backref=NULL; se->dir_seen=0;
	    strcpy( se->name, p );
	    push(se);
	}
	argc--; argv++;
    }
    while ( top != NULL) {
	se = top;
	if( se->dir_seen )
	    discard();
	else {
	    path2str( se, path );
	    if (lstat( path,&st) < 0) {
		perror( path );
		discard();
	    } else {
		if( S_ISREG(st.st_mode)) {
		    if ( (fd = open( path ,O_RDONLY)) < 0 ) {
			perror( path );
			discard();
		    } else {
			last = -1; block = -1; frag_start = -1;
			frags = 0; used_blocks = 0; frag_left = 0;
			this_frag=0; largest_frag=-1;
			for (blocks = 0; blocks < ((st.st_size+1023) >> 10); 
					 blocks++) {
			    last = block;
			    block = blocks;
			    if (ioctl(fd,FIBMAP,&block) < 0) {
				perror(path);
				break;
			    }
			    if (block) {
			        this_frag++;
				used_blocks++;
				frag_left--;
				if (last != block-1 && last != block+1 &&
				    frag_left<0) {
				    if( largest_frag<this_frag )
					largest_frag=this_frag;
				    this_frag=1;
				    frag_left = (last == -1) ? 12 : 256;
				    if (long_listing && frags > 0)
					printf ("    (file %s, fragment #%d "
						"begins at block %d, "
						"ends at block %d)\n",
						path, frags, frag_start, last);
				    if (frags > 0) {
					n_gaps++;
					sum_gaps += abs(block - last);
				    }
				    frags++;
				    frag_start = block;
				}
			    }
			}
			if( largest_frag<this_frag )
			    largest_frag=this_frag;
			if (long_listing)
			    printf ("    (file %s, fragment #%d begins at "
				    "block %d, ends at block %d)\n",
				    path, frags, frag_start, block);
			if( !silent_flag )
			    printf(" %3d%%  %s  (%d block(s), %d used, "
				   "%d fragment(s), largest %d)\n",
				   frags < 2 ? 0 : frags*100/used_blocks,
				   path,blocks,used_blocks,frags,largest_frag);
			sum_blocks+=blocks;
			sum_files++;
			sum_used_blocks+=used_blocks;
			if( frags>1 ) {
			    sum_frag_blocks+=used_blocks-largest_frag;
			    sum_frag_files++;
			    sum_frags += frags - 1;
			}
			discard();
			close(fd);
		    }
		} else if( S_ISDIR( st.st_mode ) ) { /* push dir contents */
		    if( (dir=opendir( path ))==NULL ) {
			perror(path);
			discard();
		    } else {
			if( silent_flag<2 )
			    printf("reading %s\n", path);
			while( (de=readdir(dir))!=NULL ) {
			    if( (strcmp(de->d_name,".")!=0)
			       && (strcmp(de->d_name,"..")!=0) ) {
				se1 = xmalloc( sizeof(StackElem) );
				se1->backref=se; se1->dir_seen=0;
				strcpy( se1->name, de->d_name );
				push(se1);
			    }
			}
			closedir( dir );
			se->dir_seen=1;
		    }
		} else /* if( S_ISREG(st.st_mode)) */
		    discard();
	    }
	} /* if( se->dir_seen ) */
    } /* while ( top != NULL) */
    printf ("\nsummary:\n");
    printf (" %3ld%% file fragmentation "
	    "(%ld of %ld files contain fragments)\n",
	    sum_files < 1 ? 0L : sum_frag_files * 100 / sum_files,
	    sum_frag_files, sum_files);
    printf (" %3ld%% block fragmentation "
	    "(%ld of %ld blocks are in fragments)\n",
	    sum_used_blocks < 1 ? 0L : sum_frag_blocks * 100/ sum_used_blocks, 
	    sum_frag_blocks, sum_used_blocks);
    printf (" %3ld%% overall fragmentation "
	    "(%ld fragments out of %ld blocks)\n",
	    sum_used_blocks < 1 ? 0L : sum_frags / sum_used_blocks,
	    sum_frags, sum_used_blocks);
    printf (" Average inter-fragment gap length = %ld\n", 
	    n_gaps < 1 ? 0L : sum_gaps / n_gaps);
    return 0;
}

