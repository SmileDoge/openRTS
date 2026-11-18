import os
import ida_auto, ida_pro, idc, idautils, ida_funcs, ida_hexrays

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
outd = os.environ['OUTD']
first = int(os.environ.get('FIRST', '401000'), 16)
last = int(os.environ.get('LAST', '425000'), 16)
largest = int(os.environ.get('LARGEST', '600'))
out = []
for f in idautils.Functions():
    if f < first or f > last:
        continue
    fn = ida_funcs.get_func(f)
    size = fn.end_ea - fn.start_ea
    if size > largest:
        continue
    try:
        text = str(ida_hexrays.decompile(f))
    except Exception as e:
        text = 'failed %s' % e
    out.append('// ===== %s %X size %d\n%s' % (idc.get_func_name(f), f, size, text))
open(outd + '/' + os.environ['OUTNAME'], 'w', encoding='utf-8').write('\n'.join(out))
ida_pro.qexit(0)
