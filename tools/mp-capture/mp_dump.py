#!/usr/bin/env python3
"""Reads a capture written by mp_proxy.py --dump.

  mp_dump.py capture.bin                      every frame: time, direction, connection, id, length
  mp_dump.py capture.bin --ids 4603,4639 --hex  those ids, with their bodies in hex
  mp_dump.py capture.bin --counts             frames and bytes per id and direction
  mp_dump.py capture.bin --nested --ids 4603  also the nested packages inside the body

The capture holds real players' data when it is taken on a real server:
keep it out of the repository (*.bin here is ignored by git).
"""
import argparse
import struct
import sys

KINDS = {0: 'c->s', 1: 's->c', 2: 'relay a->b', 3: 'relay b->a'}


def frames(path):
    with open(path, 'rb') as f:
        if f.read(8) != b'MPDUMP1\0':
            sys.exit('%s: not an mp_proxy capture' % path)
        while True:
            head = f.read(17)
            if len(head) < 17:
                return
            t, kind, conn, n = struct.unpack('<dBII', head)
            data = f.read(n)
            if len(data) < n:
                return
            yield t, kind, conn, data


# Ids RTS nests inside other packages (docs/reverse/multiplayer-protocol.md).
NESTED = {0x4604, 0x460A, 0x460C, 0x460E, 0x460F, 0x4621, 0x4625, 0x4626, 0x4628, 0x4629,
          0x462A, 0x462B, 0x462C, 0x4631, 0x4638, 0x463B, 0x463E}


def nested(body, depth=1, out=None):
    """Packages inside a body: u16 id, u32 length (counting these 6 bytes), body.
    Only ids RTS nests count, so a piece of a number is not taken for a header."""
    out = [] if out is None else out
    pos = 0
    while pos + 6 <= len(body):
        pid, length = struct.unpack_from('<HI', body, pos)
        if pid not in NESTED or length < 6 or pos + length > len(body):
            pos += 1
            continue
        out.append((depth, pos, pid, body[pos + 6:pos + length]))
        nested(body[pos + 6:pos + length], depth + 1, out)
        pos += length
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture')
    ap.add_argument('--ids', help='comma-separated hex ids')
    ap.add_argument('--hex', action='store_true', help='print the bodies')
    ap.add_argument('--nested', action='store_true', help='list the nested packages')
    ap.add_argument('--counts', action='store_true')
    ap.add_argument('--limit', type=int, default=0, help='stop after this many frames shown')
    a = ap.parse_args()
    want = {int(x, 16) for x in a.ids.split(',')} if a.ids else None
    counts = {}
    shown = 0
    for t, kind, conn, data in frames(a.capture):
        if len(data) < 2:
            continue
        pid, = struct.unpack_from('<H', data)
        # the login reply carries no length field
        login_reply = kind == 1 and pid == 0x4600 and (len(data) < 6 or struct.unpack_from('<I', data, 2)[0] != len(data))
        body = data[2:] if login_reply else data[6:]
        if a.counts:
            key = (pid, KINDS.get(kind, kind))
            n, b = counts.get(key, (0, 0))
            counts[key] = (n + 1, b + len(data))
            continue
        if want and pid not in want:
            continue
        print('%9.3f %-10s #%-8d %04x len %d%s' % (t, KINDS.get(kind, kind), conn, pid, len(data), ' (login reply)' if login_reply else ''))
        if a.hex:
            for i in range(0, len(body), 32):
                print('      %04x  %s' % (i, body[i:i + 32].hex(' ')))
        if a.nested:
            for depth, pos, npid, nbody in nested(body):
                print('      %s%04x at %d len %d' % ('  ' * depth, npid, pos, len(nbody) + 6))
        shown += 1
        if a.limit and shown >= a.limit:
            break
    if a.counts:
        for (pid, kind), (n, b) in sorted(counts.items()):
            print('%04x %-10s x%-7d %d B' % (pid, kind, n, b))


if __name__ == '__main__':
    main()
