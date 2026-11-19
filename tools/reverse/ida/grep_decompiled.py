import os, re
import ida_auto, ida_pro, idc, idautils, ida_hexrays

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
outd = os.environ['OUTD']
pattern = re.compile(os.environ['PATTERN'])
last = int(os.environ.get('LAST', '425000'), 16)
out = []
for f in idautils.Functions():
    if f > last:
        continue
    try:
        text = str(ida_hexrays.decompile(f))
    except Exception:
        continue
    lines = text.split('\n')
    for n, line in enumerate(lines):
        if pattern.search(line):
            context = '\n'.join(lines[max(0, n - 6):n + 8])
            out.append('// ----- %s %X line %d\n%s' % (idc.get_func_name(f), f, n, context))
open(outd + '/' + os.environ['OUTNAME'], 'w', encoding='utf-8').write('\n\n'.join(out))
ida_pro.qexit(0)
