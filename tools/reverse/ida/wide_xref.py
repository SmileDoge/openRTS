import os
import ida_auto, ida_pro, idc, idautils, ida_funcs, ida_hexrays, ida_bytes, ida_ida, ida_search
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
outd = os.environ['OUTD']
out = []
funcs = set()
for word in os.environ['WORDS'].split(','):
    pat = (word + '\0').encode('utf-16-le')
    ea = ida_ida.inf_get_min_ea()
    end = ida_ida.inf_get_max_ea()
    while True:
        ea = ida_bytes.find_bytes(pat, ea, range_end=end)
        if ea == idc.BADADDR or ea is None: break
        out.append('WIDE %s at %X' % (word, ea))
        for x in idautils.XrefsTo(ea):
            f = ida_funcs.get_func(x.frm)
            out.append('  xref %X in %s' % (x.frm, idc.get_func_name(f.start_ea) if f else '?'))
            if f: funcs.add(f.start_ea)
        ea += 2
for f in sorted(funcs):
    try:
        out.append('// ===== %s %X\n%s' % (idc.get_func_name(f), f, ida_hexrays.decompile(f)))
    except Exception as e:
        out.append('// ===== %X failed %s' % (f, e))
open(outd + '/wide.c', 'w', encoding='utf-8').write('\n'.join(out))
ida_pro.qexit(0)
