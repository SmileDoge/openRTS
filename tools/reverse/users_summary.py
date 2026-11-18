import os, re, sys
from collections import defaultdict

base = sys.argv[1]
table = defaultdict(lambda: defaultdict(set))   # offset -> dll -> ops
for name in sorted(os.listdir(base)):
    p = os.path.join(base, name, 'brakes_users.txt')
    if not os.path.exists(p):
        continue
    text = open(p, encoding='utf-8').read()
    for block in text.split('// ===== ')[1:]:
        head, _, body = block.partition('\n')
        m = re.search(r'ptrs (\{.*\})', head)
        ptrs = eval(m.group(1)) if m else {}
        for line in body.split('\n'):
            s = line[6:] if len(line) > 6 else line
            # inline: *(T *)(*(_DWORD *)(f(..."BRAKES"...) + 20) + N) op rhs
            for mm in re.finditer(r'\*\(\w+ \*\)\(\*\(_DWORD \*\)\([^;]*?L"BRAKES"[^;]*?\) \+ 20\) \+ (\d+)\)\s*(\|=|&=|=(?!=)|\+=)?\s*([^;]*)', s):
                o = int(mm.group(1))
                if mm.group(2):
                    table[o][name].add('%s %s' % (mm.group(2), mm.group(3).strip()[:30]))
                else:
                    table[o][name].add('read')
            for v, off in ptrs.items():
                if re.search(r'\b%s = ' % v, s) and 'BRAKES' in s:
                    continue
                for mm in re.finditer(r'(?:\*\(\w+ \*\)\(?%s(?: \+ (\d+))?\)?|\*%s\b)\s*(\|=|&=|=(?!=)|\+=)\s*([^;]*)' % (v, v), s):
                    o = off + int(mm.group(1) or 0)
                    table[o][name].add('%s %s' % (mm.group(2), mm.group(3).strip()[:30]))
                if re.search(r'(\*\(\w+ \*\)\(?%s\b|\*%s\b)(?!\s*(\|=|&=|\+=|=(?!=)))' % (v, v), s):
                    for t in re.finditer(r'\(\*\(_BYTE \*\)\(?%s(?: \+ (\d))?\)? & (\w+)\)' % v, s):
                        table[off + int(t.group(1) or 0)][name].add('test byte & %s' % t.group(2))
                    if not re.search(r'_BYTE \*\)\(?%s' % v, s):
                        table[off][name].add('read')
for o in sorted(table):
    print('var+%d' % o)
    for dll, ops in sorted(table[o].items()):
        print('   %-12s %s' % (dll, '; '.join(sorted(ops))[:150]))
