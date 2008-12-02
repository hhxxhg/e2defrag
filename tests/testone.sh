#! /bin/bash
set -e
set -x
# Have /sbin in path for mke2fs etc.
PATH="$PATH:/usr/sbin:/sbin"

size="`echo \"$1\"|cut -d, -f1`"
zonesize="`echo \"$1\"|cut -d, -f2`"
features="`echo \"$1\"|cut -d, -f3-`"

if [ x"$features" = x ]; then
  echo "Usage: $0 <fs-size-in-megabytes>M,<blocksize-in-bytes>,<features>" >&2
  echo " where <features> is in a form recognized by mke2fs -O." >&2
  exit 1
fi

unpriv_user=nobody
unpriv_cmd="su -p $unpriv_user"
before=before,$size,$zonesize,$features
after="`echo \"$before\" | sed s/before/after/`"

rm -rf sandbox
mkdir sandbox
chown "$unpriv_user" sandbox
cd sandbox
ln -s ../*.sh .
scriptdir=..
export e2defrag=../../builddir/e2defrag


$unpriv_cmd $scriptdir/mkfs.sh "$before"

mkdir mnt
mount -t ext2 -o loop $before mnt
chown "$unpriv_user" mnt

$unpriv_cmd $scriptdir/scribble.sh "$before"
sync

# Don't want tar to change access times.
mount -o ro,remount mnt

$unpriv_cmd $scriptdir/btar.sh "$before"

umount mnt

$unpriv_cmd $scriptdir/run_defrag.sh "$before"

mount -t ext2 -o ro,loop $after mnt

$unpriv_cmd $scriptdir/atar.sh "$before"

umount mnt

cmp "$before.tbz" "$after.tbz"

bzip2 < "$before" > ../"$before".bz2
bzip2 < "$after" > ../"$after".bz2
cd ..
rm -r sandbox
echo "Success"
