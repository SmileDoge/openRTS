"""In a DLL that looks the BRAKES library up by name: every function that
does, decompiled, with the string shown and the lines that use the
library object or its variable stack."""
import os, re
import ida_auto, ida_pro, idc, idautils, ida_funcs, ida_hexrays, ida_bytes, ida_ida

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
outd = os.environ['OUTD']
pat = 'BRAKES'.encode('utf-16-le') + b'\x00\x00'
eas = []
ea = ida_ida.inf_get_min_ea()
end = ida_ida.inf_get_max_ea()
while True:
    ea = ida_bytes.find_bytes(pat, ea, range_end=end)
    if ea is None or ea == idc.BADADDR:
        break
    eas.append(ea)
    ea += 2
funcs = set()
names = set()
for s in eas:
    names.add(idc.get_name(s))
    for x in idautils.XrefsTo(s):
        f = ida_funcs.get_func(x.frm)
        if f:
            funcs.add(f.start_ea)
out = ['strings at %s; names %s; %d functions' % (','.join('%X' % e for e in eas), ','.join(sorted(n for n in names if n)), len(funcs))]
for f in sorted(funcs):
    try:
        text = str(ida_hexrays.decompile(f))
    except Exception as e:
        out.append('// %X failed %s' % (f, e))
        continue
    for n in names:
        if n:
            text = re.sub(r'(\(int\)|\(const wchar_t \*\))?&?\b%s\b' % re.escape(n), 'L"BRAKES"', text)
    lines = text.split('\n')
    # Locals holding the library or a pointer into its stack.
    libs, ptrs = set(), {}
    for l in lines:
        m = re.search(r'\b(v\d+) = [^;]*L"BRAKES"[^;]*;', l)
        if m and '+ 20)' not in l:
            libs.add(m.group(1))
        m = re.search(r'\b(v\d+) = [^;]*L"BRAKES"[^;]*\+ 20\)\)? \+ (\d+)\)?;', l)
        if m:
            ptrs[m.group(1)] = int(m.group(2))
    for l in lines:
        m = re.search(r'\b(v\d+) = \(?[^;]*\*\(_DWORD \*\)\((v\d+) \+ 20\)\)? \+ (\d+)\)?;', l)
        if m and m.group(2) in libs:
            ptrs[m.group(1)] = int(m.group(3))
    keep = []
    for i, l in enumerate(lines):
        if 'BRAKES' in l or any(re.search(r'\b%s\b' % v, l) for v in list(libs) + list(ptrs)):
            keep.append('%5d %s' % (i, l.strip()))
    out.append('// ===== %s %X libs %s ptrs %s\n%s' % (idc.get_func_name(f), f, sorted(libs), ptrs, '\n'.join(keep)))
open(outd + '/brakes_users.txt', 'w', encoding='utf-8').write('\n'.join(out))
ida_pro.qexit(0)
