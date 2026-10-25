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

Timing diagnostics (new timing only):

    MINIDVD_DEBUG=1 minidvd /dev/dvd    # summary on exit (q)
    MINIDVD_DEBUG=2 minidvd /dev/dvd    # also one line per frame
    MINIDVD_XSYNC=1 minidvd /dev/dvd    # wait for the X server after every frame
    MINIDVD_NOSHM=1 minidvd /dev/dvd    # don't use MIT-SHM (shared memory) for drawing
    MINIDVD_NORENDER=1 minidvd /dev/dvd # scale on the CPU instead of with XRender

Frames are handed to X at video size through shared memory (MIT-SHM) and
the X server scales them to the window with XRender (on the GPU with most
drivers), so a resized or fullscreen window costs the same as the normal
size. Without MIT-SHM minidvd uses plain XPutImage; without XRender it
scales on the CPU. Building needs libXext and libXrender.

## Usage

Usage: minidvd /dev/your-dvd-drive or minidvd /dev/your-mounted-dvd-iso-path.

minidvd plays the main title (the longest title in the largest title set),
cell by cell, so logos and extras outside the film are skipped. Its chapters
and cell times are used for the jump keys below. On series discs (at least
three titles of similar length, e.g. episodes) playback starts at the first
of them and continues through the others in disc order. Titles of similar
length that share most of their disc sectors are versions of one film (e.g.
copy protection decoys, which add unreadable cells), not episodes: minidvd
plays the version with the fewest sectors of its own. PgUp/PgDn go to the
previous/next title. The titles are listed at startup. If the disc has no
usable title information, the whole title set is played and jumps are
estimated from the bitrate.

Audio and subtitle streams are taken from the IFO too: `a` and `n` step
through the streams the film actually has (AC3 audio only; DTS/LPCM/MPEG
audio is skipped), and a short on-screen message names the language and
kind, e.g. "Audio 2/3: English, AC3 5.1, director's comments" or
"Subtitles 1/4: Danish".

Keys:
Space: Pause
m: Mute
+ / -: Volume up / down
0: Volume reset
Left / Right: Back / forward 10 s
Down / Up: Back / forward 1 min
Shift + Left / Right: Previous / next chapter
PgUp / PgDn: Previous / next title (episode)
Mouse click: Seek within title (by time)
a: Next audio stream
Shift + a: Previous audio stream
f: Fullscreen
h: Toggle help
n: Next subtitle stream (turns subtitles on)
Shift + n: Previous subtitle stream
s: Toggle subtitle ON/OFF
q / Esc: Quit
