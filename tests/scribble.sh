#! /bin/sh
. ./common.sh

cd mnt

# See fs/ext2/ialloc.c, the comment above ext2_new_inode, about ext2's
# location policy.  We're probably still doing things slightly wrong.
# (E.g. we're currently ignoring the effect that existing files, like
# the root directory and /lost+found, have on allocation.)

dd_bs=4k
n_big=10
dd_big_count=100

dev="`df . | sed -n 2p | awk '{ print $1 }'`"
if [ -r "$dev" ]; then :; else
  echo "$dev not readable; exiting" >&2
  exit 1
fi

blks_per_grp=`LANG=C /sbin/tune2fs -l "$dev" 2>/dev/null | sed -n 's/^Blocks per group: *//p'`
blk_count=`LANG=C /sbin/tune2fs -l "$dev" 2>/dev/null | sed -n 's/^Block count: *//p'`
first_block=`LANG=C /sbin/tune2fs -l "$dev" 2>/dev/null | sed -n 's/^First block: *//p'`
n_grps=`expr $blk_count - $first_block - 1`
n_grps=`expr $n_grps / $blks_per_grp + 1`

n_sm=`expr 2 \* $n_grps`

for i in `seq $n_sm`; do
  r=`expr $i % 255 + 1`

  # Create file sm$i consisting entirely of character code $r.
  # The idea of using a different repeated character for each file
  # is so that defrag errors are more likely to be picked up.
  # The idea of using repeated characters, rather than /dev/urandom,
  # is to make the filesystem compressible, to allow a cmp.

  # [For related amusement, search the linux-kernel mailing list
  # archives for `/dev/monkeys'.]

  mkdir dir$i
  dd if=/dev/zero of=- bs=$dd_bs count=1 2>/dev/null | tr \\0 \\`printf '%o' $r` > dir$i/sm$i
done

# Create some holes in the allocation, so that we can create large
# files that are fragmented.
n_rm=$n_grps
for i in `seq $n_rm`; do
  rm dir$i/sm$i
done

# Create fragmented big files.
for i in `seq $n_big`; do
  # Note that $n_big can be bigger than $n_grps, hence mkdir.
  mkdir -p dir$i
  r=`expr 255 - $i % 255`
  dd if=/dev/zero of=- bs=$dd_bs count=$dd_big_count 2>/dev/null | tr \\0 \\`printf '%o' $r` > dir$i/big$i
done

# Check that both fast & slow symlinks work.
# (In ext2fs, symlinks whose target name is 60 (?) bytes or shorter
# store the target name where the blocks are usually found.  E2defrag
# mustn't treat the target name of fast symlinks as block data.)
# (Handled by defrag.c:optimise_inode.)
ln -s fast fastlink
ln -s this-is-a/-slow-symlink/-needing-its/-own-block/-allocated/-for-it slowlink

sync
