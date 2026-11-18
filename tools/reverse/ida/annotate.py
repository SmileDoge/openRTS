import os, re
import ida_auto, ida_pro, idc, idautils, ida_funcs, ida_hexrays, ida_bytes, ida_nalt

ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
outd = os.environ['OUTD']
first = int(os.environ.get('FIRST', '401000'), 16)
last = int(os.environ.get('LAST', '425000'), 16)
renames = {}
for pair in os.environ.get('RENAMES', '').split(','):
    if '=' in pair:
        a, b = pair.split('=')
        renames[a] = b


def wide_at(ea):
    chars = []
    for i in range(64):
        c = ida_bytes.get_wide_word(ea + 2 * i)
        if c == 0:
            break
        if c < 0x20 or c > 0xFFFD:
            return None
        chars.append(chr(c))
    if not chars:
        return None
    return ''.join(chars)


label = re.compile(r'\(\w+ \*?\)?&?\b((?:off|a|word|unk|asc|byte)_?[A-Za-z0-9_]*)\b')
cache = {}


def text_of(name):
    if name in cache:
        return cache[name]
    ea = idc.get_name_ea_simple(name)
    value = None
    if ea != idc.BADADDR:
        value = wide_at(ea)
    cache[name] = value
    return value


out = []
for f in idautils.Functions():
    if f < first or f > last:
        continue
    try:
        text = str(ida_hexrays.decompile(f))
    except Exception as e:
        continue

    def sub(m):
        name = m.group(1)
        value = text_of(name)
        if value is None:
            return m.group(0)
        return 'L"%s"' % value

    text = re.sub(r'\((?:int|const wchar_t \*|wchar_t \*|const size_t|LPCWSTR)\)&?\b((?:off|a|word|unk|asc)_?[A-Za-z0-9_]*)\b', sub, text)
    text = re.sub(r'(?<![\w"])&?\b((?:off|word|unk|asc)_[0-9A-F]{6}|a[A-Z][A-Za-z0-9_]*)\b', sub, text)
    for a, b in renames.items():
        text = re.sub(r'\b%s\b' % a, b, text)
    out.append('// ===== %s %X\n%s' % (idc.get_func_name(f), f, text))
open(outd + '/' + os.environ['OUTNAME'], 'w', encoding='utf-8').write('\n'.join(out))
ida_pro.qexit(0)
