# minidvdplayer
Portable (static linked) dvdplayer, small footprint (304K), tinyalsa (w. OSS fallback), subtitles, audiotracks, resize/fullscreen modes, softvolume control, mousejump and helpscreen.
No menu or other dvd-author support - just play the thing. Slightly rough but judge yourself.

## Building

Everything needed is in the repo (libdvdcss, libdvdread, libmpeg2, liba52). Just run:

    make

This configures and builds the bundled libraries, then links `minidvd` statically.
The libraries are configured once; they are reconfigured automatically if `CC`,
`HOST`, `LIB_CFLAGS` or `LIB_LDFLAGS` change.

Useful variables (pass on the make command line):

    make CC=/path/to/i386-linux-uclibc-gcc HOST=i386-linux-uclibc   # cross build
    make LIB_CFLAGS="..." LIB_LDFLAGS="..."                          # library flags
    make X11_LIBS="-lX11 -lxcb -lXau -lXdmcp"                        # custom X11 link line

Other targets: `make libs`, `make clean` (minidvd only), `make libs-clean`,
`make distclean` (also removes all library configure output), `make install`.

## A/V timing

Each video frame is shown at its timestamp, measured against a clock that is
locked to the sound card's playback position, so motion is even and lipsync
is held. `make clean; make SYNC=old` builds the previous timing instead.

If lipsync on a particular sound card needs fine tuning, set
`MINIDVD_AV_OFFSET` (milliseconds; positive shows video later), e.g.
`MINIDVD_AV_OFFSET=40 minidvd /dev/dvd`.

See `tests/` for the timing test that runs without a DVD drive or sound card.

## Usage

Usage: minidvd /dev/your-dvd-drive or minidvd /dev/your-mounted-dvd-iso-path.
Keys:
Space: Pause
m: Mute
+ / -: Volume up / down
0: Volume reset
Mouse click: Seek within title
a: Next audio stream
Shift + a: Previous audio stream
f: Fullscreen
h: Toggle help
n: Next subtitle stream
Shift + n: Previous subtitle stream
s: Toggle subtitle ON/OFF
q / Esc: Quit
