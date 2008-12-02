#! /bin/sh

# testone.sh usage: filesystem size in megabytes,
#                   block size in bytes,
#                   filesystem features (for mke2fs -O)
./testone.sh 1M,1024,none
./testone.sh 1M,1024,sparse_super,filetype
./testone.sh 1M,4096,sparse_super,filetype
./testone.sh 1M,2048,none
