#!/bin/sh
#. ../global_settings.sh
####
NAME=libdvdcss
VERSION=1.4.3
ARCHIVE=tar.bz2
URL=https://mirror.group.one/vlc/libdvdcss/1.4.3/$NAME-$VERSION.$ARCHIVE
DEPEND=
####

#get_source $NAME-$VERSION.$ARCHIVE
rm -rf $NAME-$VERSION
tar -xjf $NAME-$VERSION.$ARCHIVE

cd $NAME-$VERSION

#fight stupid libtool:
find . -name "ltmain.sh" -print | xargs sed -i 's/-all-static)/-all-static|-static)/g' 

LD_GLOBAL=$(which ld)
STATIC="-static"
CC_GLOBAL=$(which gcc)
CFLAGS_GLOBAL="-pipe -Os -Wall -D_BSD_SOURCE -D_GNU_SOURCE -D__KERNEL_STRICT_NAMES  -ffunction-sections -fdata-sections "
LDFLAGS_GLOBAL="$STATIC -Wl,--gc-sections,--sort-common,-s"


LD="$LD_GLOBAL" CC="$CC_GLOBAL" CFLAGS="$CFLAGS_GLOBAL" LDFLAGS="$LDFLAGS_GLOBAL" \
./configure --prefix=/usr ${INCLUDEDIR} --disable-silent-rules --disable-shared \
	--enable-static ac_cv_func_dlopen=no

make

exit
