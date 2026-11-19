"""Maps a decompiled aux library's variable stack (AuxLibrary::var, +20 of
its object) and Engine fields: per offset, which functions read or write
it, the masks set / cleared / tested, the constants stored."""
import re
import sys
from collections import defaultdict

text = open(sys.argv[1], encoding='utf-8').read()
out = open(sys.argv[2], 'w', encoding='utf-8')
funcs = re.split(r'^// ===== ', text, flags=re.M)[1:]

var_use = defaultdict(lambda: defaultdict(set))   # offset -> kind -> {func}
var_detail = defaultdict(set)                      # offset -> details
eng_use = defaultdict(lambda: defaultdict(set))
eng_detail = defaultdict(set)
params = defaultdict(set)
sounds = defaultdict(set)

ptr_assign = re.compile(r'\b(v\d+) = (?:\([^)]*\))?\(?\*\(_DWORD \*\)\((a\d+|\w+\[\d\]) \+ 20\)\)? \+ (\d+)\)?;')
ptr_assign2 = re.compile(r'\b(v\d+) = (?:\([^)]*\))?\(?\*\(_DWORD \*\)\((a\d+) \+ 20\) \+ (\d+)\)?;')
direct = re.compile(r'\*\((?:_DWORD|float|_BYTE|_WORD|int|unsigned int) \*\)\(\*\(_DWORD \*\)\((a\d+) \+ 20\) \+ (\d+)\)')
eng_ptr = re.compile(r'\b(v\d+) = engine_of\(')
param_name = re.compile(r'param(?:_int|_is_one)?\([^;]*?L"([A-Z0-9_]+)"')
sound_call = re.compile(r'sound_trigger\([^,]+, (\w+)\)')
op_re = r'(\|=|&=|\+=|-=|=(?!=))'

for body in funcs:
    head, _, code = body.partition('\n')
    name = head.split()[0]
    for m in param_name.finditer(code):
        params[m.group(1)].add(name)
    for m in sound_call.finditer(code):
        arg = m.group(1)
        if arg.isdigit() or arg.startswith('0x'):
            sounds[name].add(int(arg.rstrip("u"), 0))
        else:
            for c in re.finditer(r'\b%s = (\d+);' % arg, code):
                sounds[name].add(int(c.group(1)))
    pointers = {}
    for m in ptr_assign2.finditer(code):
        pointers[m.group(1)] = int(m.group(3))
    engines = {m.group(1) for m in eng_ptr.finditer(code)}
    lines = code.split('\n')
    for line in lines:
        s = line.strip()
        # Through a pointer local into the stack.
        for v, off in pointers.items():
            if not re.search(r'\b%s\b' % v, s) or re.search(r'\b%s = ' % v, s) and 'a' in s.split('=')[1][:40] and '+ 20)' in s:
                continue
            for m in re.finditer(r'(?:\*\((?:_DWORD|float|_BYTE|_WORD|char|int) \*\)\(?%s(?: \+ (\d+))?\)?|\*%s\b|%s\[(\d+)\])\s*%s\s*([^;]*)' % (v, v, v, op_re), s):
                extra = int(m.group(1) or 0) if m.group(1) else (int(m.group(2)) * 4 if m.group(2) else 0)
                o = off + extra
                op, rhs = m.group(3), m.group(4).strip()
                var_use[o]['w'].add(name)
                if op in ('|=', '&='):
                    var_detail[o].add('%s %s' % (op, rhs[:24]))
                elif re.fullmatch(r'-?[0-9.]+u?|0x[0-9A-Fa-f]+u?', rhs):
                    var_detail[o].add('= %s' % rhs)
            if re.search(r'(?:\*\(\w+ \*\)\(?%s\b|\*%s\b)(?!\s*(\|=|&=|\+=|-=|=(?!=)))' % (v, v), s):
                var_use[off]['r'].add(name)
                for t in re.finditer(r'\(\*\(_BYTE \*\)\(?%s(?: \+ (\d))?\)? & (0x[0-9A-Fa-f]+|\d+)\)' % v, s):
                    var_detail[off + int(t.group(1) or 0)].add('test byte+%s & %s' % (t.group(1) or 0, t.group(2)))
                for t in re.finditer(r'\*\(_DWORD \*\)%s & (0x[0-9A-Fa-f]+u?)' % v, s):
                    var_detail[off].add('test & %s' % t.group(1))
        for m in direct.finditer(s):
            o = int(m.group(2))
            after = s[m.end():].lstrip()
            mo = re.match(op_re, after)
            var_use[o]['w' if mo else 'r'].add(name)
            if mo and mo.group(1) in ('|=', '&='):
                var_detail[o].add('%s %s' % (mo.group(1), after[len(mo.group(1)):].split(';')[0].strip()[:24]))
        for e in engines:
            for m in re.finditer(r'\*\((_DWORD|float|_BYTE|_WORD|__int16|char) \*\)\(%s \+ (\d+)\)' % e, s):
                o = int(m.group(2))
                after = s[m.end():].lstrip()
                mo = re.match(op_re, after)
                eng_use[o]['w' if mo else 'r'].add(name)
                if mo:
                    rhs = after[len(mo.group(1)):].split(';')[0].strip()
                    if re.fullmatch(r'-?[0-9.]+u?|0x[0-9A-Fa-f]+u?|-?\d+', rhs):
                        eng_detail[o].add('%s %s' % (mo.group(1), rhs))
                    elif mo.group(1) in ('|=', '&='):
                        eng_detail[o].add('%s %s' % (mo.group(1), rhs[:20]))

out.write('# AuxLibrary::var offsets\n')
for o in sorted(var_use):
    u = var_use[o]
    out.write('var+%-4d r:%-34s w:%-34s %s\n' % (o, ','.join(sorted(u['r']))[:60], ','.join(sorted(u['w']))[:60], '; '.join(sorted(var_detail[o]))[:200]))
out.write('\n# Engine offsets\n')
for o in sorted(eng_use):
    u = eng_use[o]
    out.write('eng+%-4d r:%-34s w:%-34s %s\n' % (o, ','.join(sorted(u['r']))[:60], ','.join(sorted(u['w']))[:60], '; '.join(sorted(eng_detail[o]))[:160]))
out.write('\n# Parameters (name: functions)\n')
for p in sorted(params):
    out.write('%s: %s\n' % (p, ','.join(sorted(params[p]))))
out.write('\n# Sound triggers by function\n')
for f in sorted(sounds):
    out.write('%s: %s\n' % (f, ' '.join(str(x) for x in sorted(sounds[f]))))
