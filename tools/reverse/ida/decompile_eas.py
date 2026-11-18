import os
import ida_auto, ida_pro, idc, ida_hexrays, ida_funcs

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
outd = os.environ['OUTD']
out = []
for text in os.environ['EAS'].split(','):
    ea = int(text, 16)
    f = ida_funcs.get_func(ea)
    start = f.start_ea if f else ea
    try:
        out.append('// ===== %s %X\n%s' % (idc.get_func_name(start), start, ida_hexrays.decompile(start)))
    except Exception as e:
        out.append('// ===== %X failed %s' % (start, e))
open(outd + '/' + os.environ['OUTNAME'], 'w', encoding='utf-8').write('\n'.join(out))
ida_pro.qexit(0)
