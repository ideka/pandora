"""Export functions from a dtk symbols.txt as a DolRecomp map ("ADDR SIZE NAME" per line).

Usage: export_map.py <symbols.txt> <out.map>
"""
import re, sys

LINE = re.compile(r'^(\S+) = \S+:0x([0-9A-Fa-f]+); // .*type:function size:0x([0-9A-Fa-f]+)')
with open(sys.argv[2], 'w', newline='\n') as out:
    n = 0
    for m in map(LINE.match, open(sys.argv[1])):
        if m:
            out.write(f'{int(m[2], 16):08X} {int(m[3], 16):08X} {m[1]}\n')
            n += 1
print(f'wrote {n} functions')
