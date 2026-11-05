"""Prints the 0x4607 train creations of a capture field by field: the ones a
console (connection 3) sends and the ones the server sends it.

  parse4607.py capture.bin [console connection, default 3]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_dump  # noqa: E402

if len(sys.argv) < 2:
    sys.exit(__doc__)
cap = sys.argv[1]
console = int(sys.argv[2]) if len(sys.argv) > 2 else 3
for t, kind, conn, data in mp_dump.frames(cap):
    pid, = struct.unpack_from('<H', data)
    if pid != 0x4607 or (kind != 0 and conn != console):
        continue
    b = data[6:]
    tr, kd, sub, vmax, st = struct.unpack_from('<IHHfB', b)
    p = 17
    cid, clen, count = struct.unpack_from('<HII', b, p)
    q = p + 10
    print('%8.1f #%d local %d kind %d subtype %d f32 %.4g start %d, %d cars' % (t, conn, tr, kd, sub, vmax, st, count))
    for _ in range(count):
        fid, flen = struct.unpack_from('<HI', b, q)
        body = b[q + 6:q + flen]
        ck, n = struct.unpack_from('<HI', body)
        name = body[6:6 + 2 * n].decode('utf-16-le').rsplit(chr(92), 1)[-1]
        r = 6 + 2 * n + 2
        uid, flags, mass = struct.unpack_from('<IIf', body, r)
        dims = struct.unpack_from('<3f', body, r + 12)
        rest = body[r + 24:]
        print('    %s kind %d uid %d flags %#x f32 %.4g dims %s rest %s' % (
            name, ck, uid, flags, mass, ' '.join('%.3g' % d for d in dims), rest.hex(' ')))
        q += flen
    if q + 8 <= len(b):
        print('    tail', struct.unpack_from('<2f', b, q))
