# minidvd

A small DVD player for X11. It plays the film on the disc and nothing else:
no menus, no setup, just play the thing. Built as a static binary with
uClibc it is about 350K and runs on almost any Linux with X.

- Plays the main film directly; series discs play episode after episode
- Smooth playback: every frame is shown at its timestamp, locked to the
  sound card's clock, so motion is even and lipsync is held
- Jump by 10 s, 1 min, chapter or title; click in the window to seek
- Audio tracks and subtitles named by language on screen
  ("Audio 2/3: English, AC3 5.1")
- Resizable window and fullscreen, scaled on the GPU (XRender) where available
- tinyalsa sound with OSS fallback, software volume, mute
- Recovers from scratched discs and skips common copy-protection decoys

## Building

Requirements: a C compiler (gcc 3.4 or newer), make, and the X11, Xext and
Xrender development files. Everything else (libdvdcss, libdvdread,
libmpeg2, liba52, tinyalsa) is included in this repository.

    make

This configures and builds the bundled libraries, then links `minidvd`.
With a uClibc compiler the binary is fully static; with glibc it is linked
normally. The libraries are configured once and reconfigured automatically
if `CC`, `HOST`, `LIB_CFLAGS` or `LIB_LDFLAGS` change.

Useful variables (on the make command line):

    make CC=/path/to/i386-linux-uclibc-gcc HOST=i386-linux-uclibc   # cross build
    make STATIC=-static                                              # force a static binary
    make LIB_CFLAGS="..." LIB_LDFLAGS="..."                          # library flags
    make X11_LIBS="-lXrender -lXext -lX11 -lxcb -lXau -lXdmcp"       # custom X11 link line

Targets: `make` / `make all`, `make install` (to `/usr/bin`, honours
`DESTDIR`), `make libs`, `make clean` (minidvd only), `make libs-clean`,
`make distclean` (also removes all library configure output) and
`make test-ifo` (unit test, see `tests/`).

## Usage

    minidvd /dev/sr0                 # DVD drive
    minidvd /path/to/mounted/dvd     # mounted disc or ISO image

At startup minidvd lists the disc's titles, marking where playback starts
(`>`), the other episodes of a series (`*`) and versions of the film that
are skipped (`~`), followed by the audio and subtitle streams.

### Keys and mouse

| Key | Action |
|---|---|
| Space | Pause / continue |
| Left / Right | Back / forward 10 seconds |
| Down / Up | Back / forward 1 minute |
| Shift + Left / Right | Previous / next chapter |
| PgUp / PgDn | Previous / next title (episode) |
| Mouse click (left button) | Seek: the click's horizontal position is the position in the title (left edge = start, right edge = end) |
| a / Shift + a | Next / previous audio track |
| n / Shift + n | Next / previous subtitle language (turns subtitles on) |
| s | Subtitles on / off |
| m | Mute |
| + / - | Volume up / down |
| 0 | Volume back to 100 % |
| f | Fullscreen on / off |
| h | Help screen (pauses) |
| q / Esc | Quit |

Repeated jump keys add up, so holding Right moves on in 10-second steps.
After every jump, track or subtitle change a short message shows the new
position or the selected language.

## How it plays a disc

- **Main title:** the longest title in the largest title set, played cell
  by cell in its program chain order, so logos and extras outside the film
  are skipped. Its chapters and cell times drive the jump keys.
- **Series discs:** two or more separate titles of similar length (e.g.
  episodes) play one after the other, starting with the first.
- **Versions and decoys:** titles of similar length that share most of
  their disc sectors are versions of one film (copy-protection decoys,
  theatrical/extended cut); minidvd plays the one with the fewest sectors
  of its own. Cells with impossible values (copy-protection dummies) are
  skipped.
- **Streams:** the audio and subtitle streams listed for the title, with
  their languages; on 16:9 films the widescreen subtitle variant is used.
- **Damaged discs:** unreadable areas are skipped in growing steps, so a
  scratch costs a few failed reads instead of minutes of drive retries.
- **No usable title information:** the whole largest title set is played
  and jumps are estimated from the bitrate.

### Environment variables

| Variable | Effect |
|---|---|
| `MINIDVD_AV_OFFSET=<ms>` | Fine-tune lipsync for a sound card (positive = show video later) |
| `MINIDVD_DEBUG=1` | Print a timing summary on exit (`2`: also one line per frame) |
| `MINIDVD_NORENDER=1` | Scale on the CPU instead of with XRender |
| `MINIDVD_NOSHM=1` | Send frames through the X socket instead of shared memory |
| `MINIDVD_XSYNC=1` | Wait for the X server after every frame (diagnostics) |

## Limitations

- No DVD menus or navigation commands (minidvd plays the title directly).
- ARccOS-protected discs (Sony, around 2005-2008, e.g. The Da Vinci Code)
  are not supported. minidvd skips the obviously fake cells, but the
  remaining unreadable sectors are normally avoided via the disc's
  navigation commands, so playback stalls on read errors (it gets past
  them, slowly).
- Only AC3 audio is played (DTS, LPCM and MPEG audio tracks are skipped).
- X11 only; there is no framebuffer/console output.

## Tests

`tests/` has a timing test that plays a generated DVD-style video without a
DVD drive, sound card or display (`tests/run_av_test.sh`), and a unit test
for the title, cell, chapter and stream logic (`make test-ifo`). See
`tests/README.md`.

## License

minidvd (`minidvd.c`, `audio/audio.c`, `audio/audio.h`) is
Copyright (C) M. Glargaard (graybox) and released under the zlib license;
see the header of `minidvd.c`.

The bundled libraries keep their own licenses:

| Component | License |
|---|---|
| libdvdcss 1.4.3 | GPL-2.0-or-later |
| libdvdread 5.0.3 | GPL-2.0-or-later |
| libmpeg2 0.5.1 | GPL-2.0-or-later |
| liba52 (a52dec) 0.8.0 | GPL-2.0-or-later |
| tinyalsa (`audio/pcm.c`, `audio/pcm.h`, `audio/asoundlib.h`), Copyright 2011 The Android Open Source Project | BSD-3-Clause (see the file headers) |

A minidvd binary is linked with the GPL libraries above, so the binary as a
whole is covered by the GPL-2.0-or-later: if you distribute it, the
complete source must be available, which this repository provides. Static
builds also include uClibc (LGPL-2.1) and the X11 client libraries
(MIT/X11 license).

## Thanks

- The libdvdcss and libdvdread developers (now maintained by VideoLAN) for
  reading and decrypting DVDs
- Aaron Holtzman, Michel Lespinasse and the other libmpeg2 and liba52
  contributors for the MPEG-2 video and AC3 audio decoders
- The Android Open Source Project for tinyalsa
- The uClibc developers for a C library small enough for a tiny static player
- The X.Org and XFree86 projects for X11, MIT-SHM and XRender
