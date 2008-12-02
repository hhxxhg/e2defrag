#! /bin/sh
. ./common.sh

size="`echo \"$before\" | cut -d, -f2`"
zonesize="`echo \"$before\" | cut -d, -f3`"
features="`echo \"$before\" | cut -d, -f4-`"

# Loop-back filesystem doesn't like holes.
#dd if=/dev/null count=0 of=$before bs=$size seek=1

sizeM="`echo \"$size\" | sed -n '/^[1-9][0-9]*M$/s/M//p'`"

if [ x"$sizeM" = x ]; then
  echo "Size must be specified with M suffix." >&2
  exit 1
fi

dd if=/dev/zero bs=1M count="$sizeM" of="$before"
sync
du -k "$before"
mke2fs -b "$zonesize" -m 1 -O "$features" "$before"
sync
e2fsck -fv "$before"
sync
du -k "$before"
