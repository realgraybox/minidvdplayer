#!/bin/sh
#. ../global_settings.sh
####
NAME=a52dec
VERSION=0.8.0
ARCHIVE=tar.gz
URL=https://distfiles.adelielinux.org/source/a52dec/$NAME-$VERSION.$ARCHIVE
DEPEND=
####

#get_source $NAME-$VERSION.$ARCHIVE
rm -rf $NAME-$VERSION
tar -xzf $NAME-$VERSION.$ARCHIVE --no-same-owner

cd $NAME-$VERSION
#patch -p1 <../patches/xmovie-1.9.13-uclibc.patch

#fight stupid libtool:
find . -name "ltmain.sh" -print | xargs sed -i 's/-all-static)/-all-static|-static)/g' 

LD_GLOBAL=$(which ld)
STATIC="-static"
CC_GLOBAL=$(which gcc)
CFLAGS_GLOBAL="-pipe -Os -Wall -D_BSD_SOURCE -D_GNU_SOURCE -D__KERNEL_STRICT_NAMES  -ffunction-sections -fdata-sections "
LDFLAGS_GLOBAL="$STATIC -Wl,--gc-sections,--sort-common,-s"


LD="$LD_GLOBAL" CC="$CC_GLOBAL" CFLAGS="$CFLAGS_GLOBAL" LDFLAGS="$LDFLAGS_GLOBAL" \
./configure --prefix=/usr ${INCLUDEDIR} --disable-silent-rules

make

exit
