import struct, sys
src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src,'rb').read())
nblk = len(data)//2048
def pts_bytes(p):
    return bytes([0x21 | ((p>>29)&0x0E), (p>>22)&0xFF, ((p>>14)&0xFE)|1, (p>>7)&0xFF, ((p<<1)&0xFE)|1])
def spu():
    x1,x2,y1,y2 = 260,459,480,520
    lines = (y2-y1)//2
    top = b'\x00\x01'*lines          # "rest of line, colour 1"
    bot = b'\x00\x01'*lines
    rle_top, rle_bot = 4, 4+len(top)
    c1 = 4+len(top)+len(bot)
    cmds = bytes([0x03,0x00,0x10, 0x04,0x00,0xF0,
                  0x05, x1>>4, ((x1&15)<<4)|(x2>>8), x2&255, y1>>4, ((y1&15)<<4)|(y2>>8), y2&255,
                  0x06]) + struct.pack('>HH', rle_top, rle_bot) + bytes([0x01, 0xFF])
    c2 = c1 + 4 + len(cmds)
    blk1 = struct.pack('>HH', 0, c2) + cmds
    blk2 = struct.pack('>HH', 100, c2) + bytes([0x02, 0xFF])
    body = top + bot + blk1 + blk2
    total = 4 + len(body)
    return struct.pack('>HH', total, c1) + body
def pack(hdr14, pts):
    s = spu()
    pes_payload = b'\x81\x80\x05' + pts_bytes(pts) + b'\x20' + s
    pes = b'\x00\x00\x01\xBD' + struct.pack('>H', len(pes_payload)) + pes_payload
    out = bytes(hdr14) + pes
    pad = 2048 - len(out) - 6
    out += b'\x00\x00\x01\xBE' + struct.pack('>H', pad) + b'\xFF'*pad
    assert len(out) == 2048
    return out
start, dur = 0.54, 30.0
inserts = []
for t in (3.54, 10.54):
    idx = int((t - 0.5 - start) / dur * nblk)       # muxed ~0.5 s early
    hdr = data[idx*2048: idx*2048+14]
    assert hdr[:4] == b'\x00\x00\x01\xBA'
    inserts.append((idx, pack(hdr, int(t*90000))))
for idx, pk in sorted(inserts, reverse=True):
    data[(idx+1)*2048:(idx+1)*2048] = pk
open(dst,'wb').write(data)
print("inserted at blocks", [i for i,_ in inserts])
