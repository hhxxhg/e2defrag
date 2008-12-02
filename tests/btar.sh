#! /bin/sh
. ./common.sh
cd mnt
tar cSf - . | bzip2 > "../$before.tbz"
