#!/bin/sh
#. ../global_settings.sh
####
NAME=libmpeg2
VERSION=0.5.1
ARCHIVE=tar.gz
URL=http://libmpeg2.sourceforge.net/files/$NAME-$VERSION.$ARCHIVE
DEPEND=
####

#get_source $NAME-$VERSION.$ARCHIVE
rm -rf  $NAME-$VERSION
tar -xzf $NAME-$VERSION.$ARCHIVE

cd $NAME-$VERSION

#fight stupid libtool:
find . -name "ltmain.sh" -print | xargs sed -i 's/-all-static)/-all-static|-static)/g' 

LD_GLOBAL=$(which ld)
STATIC="-static"
CC_GLOBAL=$(which gcc)
CFLAGS_GLOBAL="-pipe -Os -Wall -D_BSD_SOURCE -D_GNU_SOURCE -D__KERNEL_STRICT_NAMES  -ffunction-sections -fdata-sections "
LDFLAGS_GLOBAL="$STATIC -Wl,--gc-sections,--sort-common,-s"



LD="$LD_GLOBAL" CC="$CC_GLOBAL" CFLAGS="$CFLAGS_GLOBAL" LDFLAGS="$LDFLAGS_GLOBAL" \
./configure --prefix=/usr ${INCLUDEDIR} mandir=/usr/share/man


make

exit
