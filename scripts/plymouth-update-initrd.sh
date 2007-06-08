#!/bin/bash

if [ "x$PLYMOUTH_DISABLE_INITRD" = "x1" ]; then 
  exit 0
fi

set -e

[ -z "$LIB" ] && LIB="lib"
[ -z "$LIBDIR" ] && LIBDIR="/usr/$LIB"
[ -z "$LIBEXECDIR" ] && LIBEXECDIR="/usr/libexec"
[ -z "$DATADIR" ] && DATADIR="/usr/share"
[ -z "$INITRD" ] && INITRD="/boot/initrd-$(/bin/uname -r).img"

if [ -z "$NEW_INITRD" ]; then
  NEW_INITRD="$(dirname $INITRD)/$(basename $INITRD .img)-plymouth.img"
fi

TMPDIR="$(mktemp -d $PWD/initrd.XXXXXXXXXX)"

(
    cd $TMPDIR
    zcat $INITRD | cpio --quiet -Hnewc -i --make-directories
    sed -i -e 's@^#!\(.*\)@#!/bin/plymouth \1@' init 
    (
        cd $LIBDIR
        install -m755 $(/usr/bin/readlink libply.so) $TMPDIR/lib
        install -m755 $(/usr/bin/readlink libpng12.so) $TMPDIR/lib
	cd /$LIB
    )
    /sbin/ldconfig -n lib

    install -m755 $LIBEXECDIR/plymouth/plymouth bin

    mkdir -p usr/share/plymouth

    install -m644 $DATADIR/pixmaps/fedora-logo.png usr/share/plymouth
    install -m644 $DATADIR/plymouth/star.png usr/share/plymouth

    mkdir -p usr/$LIB/plymouth
    install -m755 $LIBDIR/plymouth/fedora-fade-in.so usr/$LIB/plymouth

    rm -f $NEW_INITRD
    find | cpio --quiet -Hnewc -o | gzip -9 > $NEW_INITRD
    [ $? -eq 0 ] && echo "Wrote $NEW_INITRD"
)

rm -rf "$TMPDIR"
