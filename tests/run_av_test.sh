#!/bin/sh
# A/V timing test without a DVD drive or sound card.
#
# Builds minidvd with the test hooks (plain VOB input + simulated sound card
# that reports its position once per 4096-frame period, like many real
# cards), plays a generated 30 s PAL DVD-style VOB under Xvfb, and prints
# frame-interval and A/V-offset statistics. Needs ffmpeg, Xvfb (xvfb-run)
# and python3.
#
#   tests/run_av_test.sh          new timing
#   tests/run_av_test.sh old      previous timing (SYNC=old)
#
# Extra scenarios via MINIDVD_TEST, e.g.
#   MINIDVD_TEST="8:seek:0.5,14:pause:2,20:audio,25:audio" tests/run_av_test.sh
set -e
cd "$(dirname "$0")/.."
SYNC=${1:-new}
OUT=tests/out
mkdir -p $OUT

if [ ! -f $OUT/test.vob ]; then
	ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=720x576:rate=25 \
		-f lavfi -i "sine=frequency=440:sample_rate=48000" -t 30 \
		-target pal-dvd -b:v 5000k -c:a ac3 -b:a 192k -ac 2 $OUT/test.vob
fi

make clean >/dev/null
make SYNC=$SYNC EXTRA_CFLAGS="-DTEST_PLAIN_VOB -DAUDIO_HAVE_SIM" >/dev/null 2>&1
mv minidvd $OUT/minidvd-$SYNC-test
make clean >/dev/null

# xvfb-run merges stderr into stdout
xvfb-run -a -s "-screen 0 1024x768x24" $OUT/minidvd-$SYNC-test $OUT/test.vob > $OUT/$SYNC.log 2>&1 || true
python3 tests/av_analyze.py $OUT/$SYNC.log
