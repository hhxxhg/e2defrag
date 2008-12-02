
/* This is the result of my first experiments with the ext2 file system.
 * The code is rather messy and sometimes simply wrong,
 * but for me it's better than nothing.
 * 
 * Alexey Vovenko (vovenko@ixwin.ihep.su)
 */

 /* Known bugs:
  * 1. Picture option uses ncurses and requires a terminfo description
  *    from the ncurses package. Default terminfo files (from SLS at least)
  *    are broken.
  */
  
/* A general structure of the xiafs:
 *  absolute_block              contents                 length in blocks
 *                 +------------------------------------+            
 *     0           | 512 bytes boot sector + superblock |  1
 *                 +------------------------------------+
 *     1           | inode allocation bitmap            | s.s_imap_blocks
 *                 +------------------------------------+
 * 1+s_imap_blocks | zone allocation bitmap             | s.s_zmap_blocks
 *                 +------------------------------------+
 * 1+s_imap+s_zmap | inodes tables                      | s.s_ninodes*64/s_zone_size
 *                 +------------------------------------+
 *                 ....
 *                 +------------------------------------+
 * s_firstkernzone | kernel                             | ?, note that s_kern_zones
 *                 +------------------------------------+ is set to the actual kernel size
 *                 ....
 *                 +------------------------------------+ s_ndatazones +
 * s_firstdatazone | data zones                         | s_firstdatazone
 *                 +------------------------------------+ 
 *
 * Inode numbers are in the range 1 <= i <= s_ninodes. 
 * The least significant bit in the bitmap is for non-existent inode 0.
 * 
 * Data block numbers are in the range 
 * s_firstdatazone <= i < s_firstdatazone+s_ndatazones
 * The least significant bit in the bitmap is for nonexistent block 
 * s_firstdatazone-1.
 * This means that superblock, bitmaps and kernel zones are not represented
 * in the zone bitmap.
 *
 * Broken bitmaps is the MINIX fs heritage :-(
 *
 * Tricky thing: i_blocks are located in i_zone array.
 */   
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <linux/fs.h>
#include <linux/xia_fs.h>
#include "display.h"

#ifndef TRUE
   #define FALSE 0 
   #define TRUE  1
#endif   

typedef unsigned char byte;

static int super_valid = FALSE;
static int block_size = 0;
static ulong *indblock;  /* indirection blocks used in file allocation scheme */
static ulong *dindblock;
static byte *zone_map, *inode_map;

struct xiafs_super_block s;

#if defined(__i386__) && defined(__GNUC__)
static inline int bit_is_set(unsigned char *bitmap, unsigned int nr) 
{ 
	int __res; 
	__asm__ __volatile__("btl %1,%2; adcl $0,%0" 
		:"=g" (__res) 
		:"r" (nr),"m" (*(bitmap)),"0" (0)); 
	return __res; 
}
#else
/* FIXME: Someone who knows xiafs should check whether this is correct
   wrt endianness. */
static inline int bit_is_set(unsigned char *bitmap, unsigned int nr) 
{
        return (bitmap[nr >> 3] >> (nr & 7)) & 1;
}
#endif

#define block_is_busy(bn) bit_is_set(zone_map,bn+1)
#define inode_is_busy(bn) bit_is_set(inode_map,bn)

#define UPPER(size,n)		((size + ((n) - 1)) / (n))

#define my_printf(a...) do { if (!quiet) printf(##a); } while(0)

static int IN;         /* Input device descriptor */

static void die(char const *msg) __attribute__((noreturn));

static void die(char const *s)
{
   fflush(NULL);                                    /* flush all streams */
   fprintf(stderr,s);
   exit(1);
}

static void * malloc_chk(ulong size)
{
  void *p = malloc(size);
/*  fprintf(stderr,"Alloc %u\n",size); */
  if (p == NULL) 
     die("Out of memory\n");
  return p;  
}
     
static void load_super(void) {
   lseek(IN,0,SEEK_SET);                            /* read super block */
   if (read(IN,&s,sizeof(s))!=sizeof(s)) 
      exit(1);
   super_valid = (s.s_magic == _XIAFS_SUPER_MAGIC);
   if (!super_valid) 
             return;
   block_size = s.s_zone_size; 
   
   lseek(IN,block_size,SEEK_SET);

   inode_map = malloc_chk(block_size * s.s_imap_zones);
   if (read(IN,inode_map,block_size * s.s_imap_zones)!=
       block_size * s.s_imap_zones) 
           die("Can't read inode bitmap\n");
            
   zone_map = malloc_chk(block_size * s.s_zmap_zones); 
   if (read(IN,zone_map,block_size * s.s_zmap_zones)!=
       block_size * s.s_zmap_zones) 
           die("Can't read zone bitmap\n");
           
   indblock  = malloc_chk(block_size);        
   dindblock = malloc_chk(block_size);        
}

/* Inode number is in [1..super_block.s_inodes_count] range */
static void load_inode(struct xiafs_inode *n,ulong inode_no)
{
   
   if (inode_no < 1 || inode_no > s.s_ninodes) 
      die("Illegal inode requested!\n");
   inode_no--;
   
   lseek(IN,(1 + s.s_imap_zones + s.s_zmap_zones)*block_size + 
             sizeof(struct xiafs_inode)*inode_no, SEEK_SET);
   read(IN,n,sizeof(struct xiafs_inode));
}

static void load_block(void * buf, ulong bn)
{
   lseek(IN, bn*block_size,SEEK_SET);
   if (read(IN,buf,block_size)!=block_size) 
      die("Unable to read block\n");
}
  
#define N_BLOCKS_IN_PAGE 256
static struct block_list{
    struct block_list *next;
    ulong blocks[N_BLOCKS_IN_PAGE];
} *bl = NULL;

static struct block_list *add_p, *read_p;
static int add_i, read_i;

static void init_block_list(void)
{
   if (bl == NULL) {
      bl = malloc_chk(sizeof(struct block_list));
      add_p->next = 0;   
      add_i = 0;
      add_p->blocks[add_i] = 0;  /* Mark the end of the list */
  }  
  add_p = read_p = bl;
  add_i = read_i = 0;
}

static void add_to_block_list(ulong bn)
{
   if (add_i < N_BLOCKS_IN_PAGE) 
      add_p->blocks[add_i++] = bn;
   else {
      if (add_p->next == NULL) {
         add_p->next = malloc_chk(sizeof(struct block_list));
         add_p = add_p->next;
         add_p->next = NULL;   
      }
      else
         add_p = add_p->next;
      
      add_i = 0;
      add_p->blocks[add_i++] = bn;
   }
}   

static ulong get_from_block_list(void)
{
   if ((add_p == read_p) && (read_i >= add_i)) 
      return 0;                             /* No more blocks */
   
   if (read_i < N_BLOCKS_IN_PAGE) 
      return read_p->blocks[read_i++];
   else {
      read_p = read_p->next;
      if (read_p == NULL) 
         return 0;
      read_i = 0;
      return read_p->blocks[read_i++];
   }
}
                 /* statistic on file fragmentation */
static ulong old_bn,fragments,sparse_blocks,distance;
                 /* statistic on group usage */
static ulong blocks_in_file; /* including indirection blocks */

static ulong check_block_location(ulong bn)
{
   bn &= 0x00FFFFFF;      /* Actual block address is 24 bits only */          
   if (bn!=0) {
      if (bn >= s.s_firstdatazone+s.s_ndatazones || bn < s.s_firstdatazone) {
         fprintf(stderr,"check_block_location:%lu\n",bn);
         exit(1);
      }
      if (!block_is_busy(bn-s.s_firstdatazone)) {
         fprintf(stderr,"block %lu is not marked busy in bitmap\n",bn);
         exit(1);
      }
      blocks_in_file++;
         
      add_to_block_list(bn);
      if (old_bn+1!=bn) {
         fragments++;
         if (old_bn!=0) 
            if (old_bn > bn)
               distance += old_bn - bn - 1;
            else
               distance += bn - old_bn - 1;   
      }      
      old_bn = bn;
   }   
   else 
      sparse_blocks++;
   return bn;       
}

static void check_allocation(struct xiafs_inode *n, int quiet)
{
   int checked_size = 0;
   int i_idx = 0;
   int ii_idx = 0;
   int id_idx = 0;
   ulong bn=0;       /* block numbers */
   
   fragments = 0;    /* inconsistency counter */
   sparse_blocks = 0;
   distance  = 0;
   old_bn = 0;
   blocks_in_file = 0;
      
   init_block_list();
   if (n->i_size==0) /* FIXME */
      return;
      
   my_printf("Allocation:") ;
   while (checked_size < n->i_size) {
      if (i_idx < 8) {      /* first 8k allocation */
          bn = check_block_location(n->i_zone[i_idx]);
          my_printf("%5lu ",bn);
          i_idx++;
      }
      else 
      if (i_idx == 8) {       /* ind_zone 256k allocation */
         if (ii_idx == 0) {
            bn = check_block_location(n->i_ind_zone);
            my_printf("(indblock %lu)\n",bn);
            load_block(indblock,bn);
         }   
         bn = check_block_location(indblock[ii_idx]);
         my_printf("%5lu ",bn);
         ii_idx++;
         if (ii_idx % 8 == 0) my_printf("\n");
         if (ii_idx*sizeof(ulong) >= block_size) {
            i_idx++;
            ii_idx = 0;
         }
      }
      else 
      if (i_idx == 9) {        /* dind_zone 256*256k = 64M */                 
         if (id_idx == 0 && ii_idx == 0) {
            bn = check_block_location(n->i_dind_zone);
            my_printf("(dindblock %lu)\n",bn);
            load_block(dindblock,bn);
         }   
         if (ii_idx == 0) {
            bn = check_block_location(dindblock[id_idx]);
            my_printf("(indblock %lu)\n",bn);
            load_block(indblock,bn);
         }
         bn = check_block_location(indblock[ii_idx]);
         my_printf("%5lu ",bn);
         ii_idx++;
         if (ii_idx % 8 == 0) my_printf("\n");
         if (ii_idx*sizeof(ulong) >= block_size) {
            id_idx++;
            ii_idx = 0;
         }
         if (id_idx*sizeof(ulong) >= block_size) {
            die("File > big\n");
         }
      }
      checked_size += block_size;
   }
   my_printf("\n");
   
   if (fragments > 1) 
      my_printf("Fragments:%lu, average distance %lu\n",
                 fragments,distance/(fragments-1));
   else               
      my_printf("Not fragmented\n");
   if (sparse_blocks) 
      my_printf("File has %lu unallocated blocks\n",sparse_blocks);
   return;
}

static void dump_super(void)
{
  printf("SUPERBLOCK:\n");
  printf("Zone size   :%lu\n",s.s_zone_size); 
  printf("Zone shift  :%lu\n",s.s_zone_shift); 
  printf("Inodes count:%lu\n",s.s_ninodes);	  
  printf("Zones count:%lu\n",s.s_nzones);
  printf("Data zones:%lu\n",s.s_ndatazones);       
  printf("Imap zones:%lu\n",s.s_imap_zones); 
  printf("Zmap zones:%lu\n",s.s_zmap_zones);
  printf("First data zone:%lu\n",s.s_firstdatazone); 
  printf("Max file size  :%lu\n",s.s_max_size);    
  printf("First Kernel zone:%lu\n",s.s_firstkernzone);
  printf("Kernel zones     :%lu\n",s.s_kernzones); 
  printf("magic:0x%lX", s.s_magic); 		       /* Magic signature */
  if (s.s_magic == _XIAFS_SUPER_MAGIC) printf(" (OK)\n");
  else printf(" (???) ");
}

static void dump_inode(ulong inode_no)
{
   struct xiafs_inode n;
   ulong blocks;
   
   printf("\nINODE %lu\n", inode_no);
   load_inode(&n,inode_no);

   printf("File mode %o ",(uint)n.i_mode);
   if (S_ISREG(n.i_mode)) printf("(regular file)\n");
   else if (S_ISDIR(n.i_mode)) printf("(directory)\n");
   else if (S_ISCHR(n.i_mode)) printf("(character dev)\n");
   else if (S_ISBLK(n.i_mode)) printf("(block dev)\n");
   else if (S_ISLNK(n.i_mode)) printf("(link)\n");
   else if (S_ISFIFO(n.i_mode)) printf("(fifo)\n");
   else if (S_ISSOCK(n.i_mode)) printf("(socket)\n");
   printf("Links count: %d\n", (uint) n.i_nlinks);
   printf("Owner Uid %d ",(uint)n.i_uid);
   printf("Group Id %d\n",(uint) n.i_gid);
   printf("File size %u\n",n.i_size);
   printf("Access time      : %s",ctime(&n.i_atime));
   printf("Creation time    : %s",ctime(&n.i_ctime));
   printf("Modification time: %s",ctime(&n.i_mtime));
   blocks=((n.i_zone[0] >> 24) & 0xff) |
          ((n.i_zone[1] >> 16) & 0xff00) |
	  ((n.i_zone[2] >>  8) & 0xff0000);
   printf("512-blocks:%lu\n", blocks);
   if (!inode_is_busy(inode_no)) 
         printf("(Marked free in bitmap)\n");
   
   check_allocation(&n,FALSE); 
}

static void dump_block_bitmap(void)
{
   int i;
   
   for (i=0; i < s.s_zmap_zones*s.s_zone_size; ) {
         if (i%s.s_zone_size==0)
                printf("Block %ld\n",i/s.s_zone_size);
                
         printf("%2.2X ",zone_map[i]);
         i++;       
         if (i%16 == 0)
                printf("\n");
   }             
}

static void dump_inode_bitmap(void)
{
   int i;
   
   for (i=0; i < s.s_imap_zones*s.s_zone_size; ) {
         if (i%s.s_zone_size==0)
                printf("Block %ld\n",i/s.s_zone_size);

         printf("%2.2X ",inode_map[i]);
         i++;       
         if (i%16 == 0)
                printf("\n");
   }             
}

static void dump_block(ulong bn)
{
   int i;
   byte *buf;
   
   printf("Block %lu:\n",bn);
   buf = malloc_chk(block_size);
   if (lseek(IN,bn*block_size,SEEK_SET)<0)
       die("Can't seek there\n");
   if (block_size!=read(IN,buf,block_size))
       die("Can't read the block\n");
       
   for (i=0; i < block_size; ) {
         printf("%2.2X ",buf[i]);
         i++;       
         if (i%16 == 0)
                printf("\n");
   }             
   free(buf);
}

static void report_fragm(void)
{
   ulong inode_no;
   struct xiafs_inode n;
   ulong total_files=0,
         total_frag_files=0,
         total_fragments=0,
         total_dirs=0,
         total_frag_dirs=0,
         avr_dist=0,
         avr_fr_size=0,
         avr_dist_file=0,
         avr_fr_size_file=0;
         
   for (inode_no=1; inode_no <= s.s_ninodes; inode_no++) {
      load_inode(&n,inode_no);      
      if (!inode_is_busy(inode_no))
              continue;                   
         
      check_allocation(&n,TRUE);
      if (fragments > 1) {
         printf("Inode: %lu, frag: %lu, file size %u, ",
                inode_no, fragments, n.i_size);
         total_frag_files++;       
         total_fragments += fragments;
      }
      if (n.i_size > 0) {
         if (fragments > 1) {
            avr_dist_file  = distance /(fragments-1);
            avr_fr_size_file = UPPER(n.i_size,block_size)/(fragments);
            avr_dist += avr_dist_file;
            avr_fr_size += avr_fr_size_file;
         }   
         if (S_ISREG(n.i_mode)) {
            total_files++;
            if (fragments > 1) {
               printf("(Average frag size: %lu, dist: %lu blocks)\n",
                      avr_fr_size_file,
                      avr_dist_file);
            }
         }   
         if (S_ISDIR(n.i_mode)) {
            total_dirs++;
            if (fragments > 1) {
               total_frag_dirs++;
               printf("Fragmented directory!\n");
            }   
         }       
      }
      if (sparse_blocks) printf("Inode %lu has %lu unallocated blocks\n",
                                inode_no,sparse_blocks);   
   }       
              /* We do not count zero length files here */
              
   printf("\n%lu out of %lu files are fragmented (%ld%%)\n",
          total_frag_files,total_files,
          (total_files==0) ? 0 : (total_frag_files*100)/total_files);
   printf("%lu out of %lu directories are fragmented (%ld%%)\n",
          total_frag_dirs,total_dirs,
          (total_dirs==0) ? 0 : (total_frag_dirs*100)/total_dirs);
   printf("Total number of fragments: %lu, ",total_fragments);
   if (total_frag_files==0) total_frag_files = 1;  
   printf("Average is %lu fragments per fragmented file\n",
          total_fragments / total_frag_files);
   printf("Average fragment size in fragmented file:%lu blocks\n",
          avr_fr_size / total_frag_files);
   printf("Average distance between fragments: %ld blocks\n",
          avr_dist / total_frag_files );              
}

static void show_map(void)                 /* make block map */
{
   ushort attr;    
   int j,k,inode;
   struct xiafs_inode n;
   ulong bn;
   ulong total_files=0,total_dirs=0,
         frag_files=0,frag_dirs=0;
   char str[256];      
         
                          /* superblock + bootsector */
   set_attr(0,AT_SUPER);
      
                          /* bitmaps */
   for (j=0; j < s.s_imap_zones ; j++)
          set_attr(j+1,AT_BITMAP);
   j = 1 + s.s_imap_zones;
   for (k=0; k < s.s_zmap_zones ; k++)
          set_attr(j+k,AT_BITMAP);
      
                          /* table of inodes */
   k = 1 + s.s_imap_zones + s.s_zmap_zones;
   for(j=0; 
       j < UPPER(s.s_ninodes, (block_size / sizeof(struct xiafs_inode)));
       j++)
          set_attr(k+j,AT_INODE);
   for (j=s.s_firstkernzone; j < s.s_firstdatazone; j++)
          set_attr(j,AT_KERNEL);
   
   for (inode=1; inode <= s.s_ninodes; inode++) {
      load_inode(&n,inode);
      if (!inode_is_busy(inode))
         continue;
      if (n.i_mtime == 0) { /* Should not happen */
         printf("Inode %d, mtime is 0\n",inode);
         continue;        
      }
         
      check_allocation(&n,TRUE);
      
      attr = 0;   
      if (S_ISDIR(n.i_mode)) {
         attr |= AT_DIR;
         total_dirs++;
         if (fragments > 1) 
             frag_dirs++;
      }   
      if (S_ISREG(n.i_mode)) {
         attr |= AT_REG;   
         total_files++;
         if (fragments > 1) 
             frag_files++;
      }
      if (fragments > 1) 
         attr |= AT_FRAG;
     
      if (inode==_XIAFS_BAD_INO) 
         attr |= AT_BAD;
      while (0 != (bn=get_from_block_list())) 
         set_attr(bn,attr);
   }

   display_map();
   if (total_files==0) total_files=1; /* avoid division by 0 */
   if (total_dirs==0) total_dirs=1;
   
   sprintf(str,"%ld%% files fragmented",(frag_files*100)/total_files);
   add_comment(str);
   sprintf(str,"%ld%% directories fragmented",(frag_dirs*100)/total_dirs);
   add_comment(str);
   display_comments(" Fragmentation ");
}

enum commands {      
   CM_NONE,
   CM_SUPER,
   CM_INODE,
   CM_FRAG,
   CM_FILE,
   CM_DISPLAY,
   CM_B_BITMAP,
   CM_I_BITMAP,
   CM_B_DUMP
};

static char *InputDevice=NULL;   /* Ext2-formatted block device we are analysing */
static char *FileName;           /* Name of a file to print fragmentation data   */
static ulong StartInode,InodeCount;                   /* Range of inodes to dump */
static ulong StartBlock; /* Block to dump */

static void usage(void) __attribute__((noreturn));

static void usage(void)
{
   printf("Usage:e2dump [<option>] <device>\n");
   printf("\t-s\t\tdumps superblock\n");
   printf("\t-m\t\tdumps block allocation bitmaps in hex\n");
   printf("\t-M\t\tdumps inode allocation bitmaps in hex\n");
   printf("\t-b <number>\tdumps block's contents\n"); 
   printf("\t-i <number>\tdumps inode data and allocation chain\n");
   printf("\t-n <file>\tdumps file's inode data and allocation chain\n");
   printf("\t-f\t\tprints some statistic for all fragmented files on device\n");   
   printf("\t-d (default)\tdisplays a picture of device allocation\n");
   
   exit(1);
}

static enum commands parse_args(int argc, char *argv[])
{
   int c;
   c = getopt(argc,argv,"si:fn:dmMb:");
   InputDevice = argv[optind];
   if (c==EOF) return CM_DISPLAY;
   
   switch (c) {
      case 's': return CM_SUPER; 
      case 'i': if (0 == sscanf(optarg,"%lu", &StartInode))
                   usage();
                InodeCount = 1;
                return CM_INODE;
      case 'n': FileName = optarg;
                return CM_FILE;         
      case 'f': return CM_FRAG;
      case 'd': return CM_DISPLAY;
      case 'm': return CM_B_BITMAP;       
      case 'M': return CM_I_BITMAP;       
      case 'b': if (0 == sscanf(optarg,"%lu", &StartBlock))
                   usage();
                return CM_B_DUMP;       
      default:  usage();
                return CM_NONE; /* never reached in fact */
   }                      
}

int main(int argc, char *argv[])
{
   ulong i;
   enum commands cmd;
   struct stat st;
                    
   cmd = parse_args(argc,argv);
   IN = open(InputDevice,O_RDONLY);
   if (IN < 0) {
      fprintf(stderr,"Unable to open device %s\n",InputDevice);
      usage();
      exit(1);
   }
   load_super();
   if (!super_valid && cmd!=CM_SUPER) {
      fprintf(stderr,"Bad magic in superblock\n");
      exit(1);
   }
   switch (cmd) {
      case CM_SUPER:  dump_super();  break;
      case CM_INODE:  for (i = StartInode; i < StartInode + InodeCount; i++)
                         dump_inode(i);
                      break;   
      case CM_FRAG:   report_fragm(); break;
      case CM_FILE:   if (0!=stat(FileName,&st)) {
                         char s[256];
                         sprintf(s,"Can't stat %s",FileName);
                         perror(s);
                         exit(1);
                      }
                      dump_inode(st.st_ino);
                      break;
      case CM_B_BITMAP:
                      dump_block_bitmap();
                      break;
      case CM_I_BITMAP:
                      dump_inode_bitmap();
                      break;
      case CM_B_DUMP: dump_block(StartBlock);                
                      break;
      case CM_DISPLAY:init_screen(s.s_nzones);
                      show_map();
                      display_legend(AT_REG|AT_DIR|AT_BITMAP|AT_KERNEL|
                                     AT_INODE|AT_SUPER|AT_BAD);
                      done_screen(TRUE);
                      /* Fall through */
      case CM_NONE:   break;
   }
   return 0;
}
