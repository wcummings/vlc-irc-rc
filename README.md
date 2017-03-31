vlc-irc-rc
==========

Control your VLC with your IRC.

building
--------

I've included build.sh, which does all of this, very poorly. There's no error
handling. Worst case, it should at least clearly
demonstrate the build procedure.

I removed the Makefile because out-of-tree compilation was too finnicky and
I couldn't come up with build instructions that worked consistently across
distros.

(Note: `#define STOP_HACK' to enable code to seek 0:00 and pause on start. 
This is a hack, if you start vlc without a file in its playlist with this enabled, it will crash)

* Download the latest VLC sources (http://www.videolan.org/vlc/download-sources.html) and install its build dependenices

* Extract vlc and run ./configure

* Copy src/ircrc.c to modules/control/ in the vlc source directory

* Add the following lines to modules/control/Modules.am:

Add

<pre>
libircrc_plugin_la_SOURCES = ircrc.c
control_LTLIBRARIES += \
        libircrc_plugin.la
</pre>

To the end.

* Run ./bootstrap in the vlc directory

* Now run ./configure && make

usage
-----

<pre>
vlc -I ircrc --server your.irc.server --channel "#yourchannel" --nick
some_nickname yourfile.avi
</pre>

commands
--------

* >play
* >pause
