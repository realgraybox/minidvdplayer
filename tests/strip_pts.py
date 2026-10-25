# Keep the video PTS only on every Nth video PES that has one (like discs that
# only timestamp the first picture of a GOP); the rest becomes stuffing.
import sys
src, dst, keep = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = bytearray(open(src, 'rb').read())
i = n = stripped = 0
while True:
    i = d.find(b'\x00\x00\x01', i)
    if i < 0 or i + 14 > len(d): break
    sid = d[i + 3]
    if 0xE0 <= sid <= 0xEF and (d[i + 6] & 0xC0) == 0x80 and (d[i + 7] & 0x80):
        if n % keep:
            hl = d[i + 8]
            flags = d[i + 7] & 0xC0          # 10 = PTS only, 11 = PTS+DTS
            k = 5 if flags == 0x80 else 10
            d[i + 7] &= 0x3F                 # no PTS/DTS
            d[i + 9:i + 9 + k] = b'\xff' * k  # stuffing, header length unchanged
            stripped += 1
        n += 1
    i += 4
open(dst, 'wb').write(d)
print(f"video PES with PTS: {n}, stripped: {stripped}")
