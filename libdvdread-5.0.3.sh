#!/bin/sh
#. ../global_settings.sh
####
NAME=libdvdread
VERSION=5.0.3
ARCHIVE=tar.bz2
URL=http://distro.ibiblio.org/fatdog/source/600/l/$NAME-$VERSION.$ARCHIVE
DEPEND=
####

#get_source $NAME-$VERSION.$ARCHIVE
rm -rf $NAME-$VERSION
tar -xjf $NAME-$VERSION.$ARCHIVE

cd $NAME-$VERSION
#patch -p1 <../patches/xmovie-1.9.13-uclibc.patch

#fight stupid libtool:
find . -name "ltmain.sh" -print | xargs sed -i 's/-all-static)/-all-static|-static)/g' 

LD_GLOBAL=$(which ld)
STATIC="-static"
CC_GLOBAL=$(which gcc)
CFLAGS_GLOBAL="-pipe -Os -Wall -D_BSD_SOURCE -D_GNU_SOURCE -D__KERNEL_STRICT_NAMES  -ffunction-sections -fdata-sections "
LDFLAGS_GLOBAL="$STATIC -Wl,--gc-sections,--sort-common,-s "


LD="$LD_GLOBAL" CC="$CC_GLOBAL" CFLAGS="$CFLAGS_GLOBAL" LDFLAGS="$LDFLAGS_GLOBAL" \
./configure --prefix=/usr ${INCLUDEDIR} --disable-silent-rules --disable-shared \
	--enable-static CSS_CFLAGS="-I.././libdvdcss-1.4.3/src" 

find . -name "config.h" -print | xargs sed -i 's/\/\* #undef HAVE_DVDCSS_DVDCSS_H \*\//#define HAVE_DVDCSS_DVDCSS_H 1/g' 

make

exit
