# A/V timing tests

These run minidvd without a DVD drive, sound card or display, and measure
how evenly frames are shown and how far video is from audio.

Test-only compile flags (never used for normal builds):

- `-DTEST_PLAIN_VOB`: `minidvd file.vob` reads a plain MPEG-PS/VOB file
  instead of a DVD.
- `-DAUDIO_HAVE_SIM`: simulated sound card (TinyALSA buffer geometry,
  real-time playback, position reported once per period). For every frame
  minidvd prints `AV <time_us> <frame_pts> <heard_audio_pts> <dropped>` and
  for every subtitle `SPU <pts> <clock>`.
- `MINIDVD_TEST="sec:action,..."` (with `TEST_PLAIN_VOB`): scripted
  `seek:<fraction>`, `pause:<seconds>` and `audio` (toggle audio stream
  0x80/0x81) `size:<W>x<H>` (resize the window, i.e. scaled drawing) and `help`
  (toggle the help screen).

- `MINIDVD_TEST_CELLS="first-last:secs,..."` (with `TEST_PLAIN_VOB`): a
  title map (cells in playback order, one chapter per cell) for the plain
  VOB, to test skip-to-film, cell following and the jump keys. Test actions
  `jump:<secs>` and `chap:+1|-1` press the arrow keys, `sub` and `subtoggle`
  press `n` and `s`.

Files:

- `ifo_map_test.c`: `make test-ifo` builds the title map from IFO tables
  made in memory (main title choice, BCD times, angle/broken cells,
  chapters, start at the film, audio/subtitle stream lists and names) and
  checks it

- `run_av_test.sh [old]`: generate a test VOB, build, play, print stats
- `av_analyze.py log...`: frame interval and A/V offset statistics
- `av_buckets.py log [seconds]`: the same, per second of playback
- `add_test_subs.py in.vob out.vob`: insert two DVD subtitle packets
  (pts 3.54 s and 10.54 s) into the test VOB

Results on the generated 30 s PAL test VOB (simulated card, 4 x 4096-frame
periods):

| | frame interval (target 40 ms) | frames off by > 5 ms | A/V offset |
|---|---|---|---|
| `SYNC=old` | 27..159 ms, sd 19 ms | 626 / 716 | about -490 ms (see note) |
| new | 40 ms, sd 1-2 ms | 4-6 / 720 (scheduler hiccups) | mean +0.5 ms, sd 2 ms |

Note: in this simulation the old code shows video about 0.5 s *before* its
sound. On real hardware the old code was reported to be in sync, so the
simulated card and the test file's muxing don't fully match real discs. The
new code doesn't depend on buffer sizes or muxing: it shows frames by their
timestamps.
