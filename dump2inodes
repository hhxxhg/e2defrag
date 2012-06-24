#!/usr/bin/python

import os, sys
from stat import *
import subprocess

dump = subprocess.Popen(["ureadahead", "--dump"], stdout=subprocess.PIPE)
files = list()
directories = list()

for line in dump.stdout :
    if line[0] == "/" :
        name = line.partition(" ")[0]
        try:
            st = os.stat(name)
            if S_ISDIR(st.st_mode):
                directories.append(st.st_ino)
            else:
                files.append(st.st_ino)
        except OSError:
            pass
print "=2"
for dir in directories :
    print dir
print "=1"
for file in files :
    print file
