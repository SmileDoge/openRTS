"""Sets a run with openRTS's cab brake devices against a golden run of
Brakes.dll (openrts-train-check --dll ... --record; --brakes openrts for
the second; docs/brakes-dll-replacement.md, steps 1 and 2). Phase by
phase, between the script's moves (the golden run's switches), it prints
at each phase's end the УР, the independent brake's target, the ЭПТ's
level, BrakeSystemsEngaged, the pipe and the cylinder of both, and the
given displays (gauges, lamps); a difference over the tolerance is marked.

    python golden_compare.py golden.tsv openrts.tsv [--car 0] [--displays 2,3,4,5,21] [--tolerance 0.05]

The golden runs are on RTS's air and openRTS's devices on the realistic
air: the pipe and the cylinders differ by the air itself; the УР, the
targets and the lamps are the devices'."""
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('golden')
parser.add_argument('openrts')
parser.add_argument('--car', type=int, default=0)
parser.add_argument('--displays', default='', help='display ids to set against each other, comma list')
parser.add_argument('--tolerance', type=float, default=0.05)
args = parser.parse_args()
watched = [x for x in args.displays.split(',') if x]


def load(path):
    rows = []
    shown = {}
    with open(path, encoding='utf-8') as f:
        header = f.readline().rstrip('\n').split('\t')
        for line in f:
            cells = line.rstrip('\n').split('\t')
            if len(cells) < len(header) - 1 or int(cells[1]) != args.car:
                continue
            row = dict(zip(header, cells))
            if row.get('displays', '-') != '-':
                for d in row['displays'].split():
                    k, v = d.split('=', 1)
                    shown[k] = v
            row['shown'] = dict(shown)
            rows.append(row)
    return rows


golden = load(args.golden)
ours = load(args.openrts)
if not golden or not ours:
    raise SystemExit('no rows for car %d' % args.car)


def at(rows, t):
    """The last row at or before time t."""
    best = rows[0]
    for row in rows:
        if float(row['t']) > t + 1e-6:
            break
        best = row
    return best


# Brakes.dll with BR_MODE 1 (a multiple unit) gives the ЭПТ as the
# independent brake's target; openRTS's devices give it to the line: the
# higher of the two is the brake asked of the car.
for rows in (golden, ours):
    for row in rows:
        row['asked'] = '%.4f' % max(float(row['e_ind']), float(row['e_ept']))
columns = [('ur', 'УР'), ('asked', 'ind/ЭПТ'), ('pipe', 'ТМ'), ('cyl', 'ТЦ')]
devices = {'ur', 'asked'}
phases = [float(r['t']) for i, r in enumerate(golden) if i > 0 and r['switches'] != '-']
ends = phases[1:] + [float(golden[-1]['t'])]
marked = 0
print('car %d: %d golden rows, %d openRTS rows' % (args.car, len(golden), len(ours)))
for start, end in zip(phases, ends):
    g = at(golden, end - 0.05)
    o = at(ours, end - 0.05)
    print('%7.2f .. %7.2f  switches %s' % (start, end, at(golden, start)['switches']))
    cells = []
    for key, name in columns:
        a, b = float(g[key]), float(o[key])
        mark = '  <--' if key in devices and abs(a - b) > args.tolerance else ''
        marked += 1 if mark else 0
        cells.append('%s %.3f / %.3f%s' % (name, a, b, mark))
    print('    ' + ', '.join(cells))
    if g['e_systems'] != o['e_systems']:
        print('    BrakeSystemsEngaged %s / %s' % (g['e_systems'], o['e_systems']))
    for d in watched:
        a, b = g['shown'].get(d, '-'), o['shown'].get(d, '-')
        if a != b:
            print('    display %s: %s / %s' % (d, a, b))
print('%d device value(s) over %.3f' % (marked, args.tolerance))
