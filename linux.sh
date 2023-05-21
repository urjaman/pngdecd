#!/bin/sh
WF="-Wall -Wextra -Wno-implicit-fallthrough"
gcc -Og $WF -std=gnu89 -DLINUX -o png2bmp -x c adler32.c inflate.c main.c crc32.c inffast.c inftrees.c zutil.c
