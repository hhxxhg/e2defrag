#! /bin/sh
. ./common.sh

# FIXME: Currently this is giving lots of bitmap errors :( .
# Could be a bug in Linux kernel.
# Or maybe the umount isn't properly happening, due to loop complications?
e2fsck -fvp "$before" ||:

chmod a-w "$before"
"$e2defrag" -rn "$before"
cp "$before" "$after"
chmod u+w "$after"
"$e2defrag" -n "$after"
e2fsck -fvn "$after"
