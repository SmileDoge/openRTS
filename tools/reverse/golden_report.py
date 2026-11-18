"""Reads a golden run (openrts-train-check --dll ... --record) and tells
what the library did: the car's air and its Engine outputs at the start
and end of each phase between the script's moves, the library's sound
triggers, and given variables of its stack whenever they change.

    python golden_report.py run.tsv [--car 0] [--floats 24,40,44] [--flags 4,8,32] [--displays 6,7]

Floats and flag words are offsets in bytes into its variable stack."""
import argparse
import struct

parser = argparse.ArgumentParser()
parser.add_argument('record')
parser.add_argument('--car', type=int, default=0)
parser.add_argument('--floats', default='24,40,44')
parser.add_argument('--flags', default='4,8,32')
parser.add_argument('--displays', default='', help='display ids to follow (lamps, gauges), comma list')
args = parser.parse_args()
floats = [int(x) for x in args.floats.split(',') if x]
flags = [int(x) for x in args.flags.split(',') if x]
watched = {x for x in args.displays.split(',') if x}

rows = []
with open(args.record, encoding='utf-8') as f:
    header = f.readline().rstrip('\n').split('\t')
    for line in f:
        cells = line.rstrip('\n').split('\t')
        if len(cells) < len(header) or int(cells[1]) != args.car:
            continue
        row = dict(zip(header, cells))
        words = row.get('var', '').split()
        row['words'] = [int(w, 16) for w in words]
        rows.append(row)


def fval(row, off):
    w = row['words']
    i = off // 4
    return struct.unpack('<f', struct.pack('<I', w[i]))[0] if i < len(w) else float('nan')


def uval(row, off):
    w = row['words']
    i = off // 4
    return w[i] if i < len(w) else 0


def air(row):
    return 'TM %s УР %s ТЦ %s всп %s ГР %s | pipe_rate %s ind %s ept %s sys %s' % (
        row['pipe'], row['ur'], row['cyl'], row['ind'], row['mr'], row['e_pipe_rate'], row['e_ind'], row['e_ept'], row['e_systems'])


print('car %d, %d steps, stack %d words' % (args.car, len(rows), len(rows[0]['words']) if rows else 0))
last_switches = None
phase_start = None
last = {}
for i, row in enumerate(rows):
    t = float(row['t'])
    sw = row['switches']
    if sw != '-' and i > 0:
        if phase_start is not None:
            print('  %7.2f end:   %s' % (float(rows[i - 1]['t']), air(rows[i - 1])))
        print('%7.2f switches %s' % (t, sw))
        print('  %7.2f start: %s' % (t, air(row)))
        phase_start = t
    if row['snd_lib'] != '-':
        print('  %7.2f lib sound %s' % (t, row['snd_lib']))
    if watched and row.get('displays', '-') != '-':
        seen = [d for d in row['displays'].split() if d.split('=')[0] in watched]
        if seen:
            print('  %7.2f displays %s' % (t, ' '.join(seen)))
    for off in floats:
        v = round(fval(row, off), 3)
        if last.get(('f', off)) != v:
            if ('f', off) in last:
                print('  %7.2f var+%d %s -> %s' % (t, off, last[('f', off)], v))
            last[('f', off)] = v
    for off in flags:
        v = uval(row, off)
        old = last.get(('u', off))
        if old != v:
            if old is not None:
                print('  %7.2f var+%d 0x%08X -> 0x%08X (set 0x%X, cleared 0x%X)' % (t, off, old, v, v & ~old, old & ~v))
            last[('u', off)] = v
if rows:
    print('  %7.2f end:   %s' % (float(rows[-1]['t']), air(rows[-1])))
