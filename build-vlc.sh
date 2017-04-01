#!/bin/bash
VLC_VERSION=2.2.4
VLC_ARCHIVE=vlc-$VLC_VERSION.tar.xz
VLC_URL=http://download.videolan.org/pub/videolan/vlc/$VLC_VERSION/$VLC_ARCHIVE
CWD=$(pwd)

mkdir -p build
cd build
if [ ! -f "./$VLC_ARCHIVE" ]; then
    wget $VLC_URL -O $VLC_ARCHIVE
fi

if [ ! -d vlc-$VLC_VERSION ]; then
    tar xfJ $VLC_ARCHIVE
fi

cd vlc-$VLC_VERSION

if [ "$1" == "--update" ]; then
    cp $CWD/src/ircrc.c modules/control/
    make
    exit 0;
fi

./configure --disable-qt
cp $CWD/src/ircrc.c modules/control/

if ! grep --quiet libircrc_plugin_la_SOURCES modules/control/Modules.am; then
    echo -ne 'libircrc_plugin_la_SOURCES = ircrc.c\ncontrol_LTLIBRARIES += libircrc_plugin.la\n' >> modules/control/Modules.am
fi

./bootstrap
./configure
make
