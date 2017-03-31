#!/bin/sh
VLC_VERSION=2.0.3
VLC_ARCHIVE=vlc-$VLC_VERSION.tar.xz
VLC_URL=http://download.videolan.org/pub/videolan/vlc/$VLC_VERSION/$VLC_ARCHIVE
CWD=$(pwd)

mkdir -p build
cd build
if [ ! -f "./$VLC_ARCHIVE" ]; then
    wget $VLC_URL -O $VLC_ARCHIVE
fi
rm -r vlc-2.0.3
tar xfJ vlc-2.0.3.tar.xz
cd vlc-2.0.3
./configure
cp $CWD/src/ircrc.c modules/control/
echo -ne 'SOURCES_ircrc = ircrc.c\nlibvlc_LTLIBRARIES += libircrc_plugin.la\n' >> modules/control/Modules.am
./bootstrap
./configure
make

