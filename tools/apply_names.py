"""Rename auto-named (fn_XXXXXXXX) functions in a dtk symbols.txt.

Usage: apply_names.py <symbols.txt> <names.txt> [dsy_matches.tsv]

names.txt entries ("<addr> <name>") always win. High-confidence rows from match_dsy.py are applied
only where the function is still auto-named. Names must be plain identifiers and unique.
"""
import re, sys

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')
LINE = re.compile(r'^(\S+)( = \S+:0x)([0-9A-Fa-f]+)(;.*)$')

sym_path, names_path = sys.argv[1], sys.argv[2]
lines = open(sym_path).read().splitlines()

wanted, manual = {}, set()
if len(sys.argv) > 3:
    for row in open(sys.argv[3]):
        addr, _, conf, _, cands = row.rstrip('\n').split('\t', 4)
        if conf == 'high' and IDENT.match(cands):
            wanted[int(addr, 16)] = cands
for row in open(names_path):
    row = row.split('#', 1)[0].split()
    if row:
        wanted[int(row[0], 16)] = row[1]
        manual.add(int(row[0], 16))

taken = {m[1] for m in map(LINE.match, lines) if m}
renamed = skipped = 0
for i, line in enumerate(lines):
    m = LINE.match(line)
    if not m or 'type:function' not in line:
        continue
    addr, cur = int(m[3], 16), m[1]
    new = wanted.get(addr)
    if not new or new == cur:
        continue
    if not cur.startswith('fn_') and addr not in manual:
        continue
    if new in taken:
        skipped += 1
        continue
    taken.discard(cur)
    taken.add(new)
    lines[i] = new + m[2] + m[3] + m[4]
    renamed += 1

open(sym_path, 'w', newline='\n').write('\n'.join(lines) + '\n')
print(f'renamed {renamed}, skipped {skipped} (name already used)')
