"""lines2rva.py <pdb> <chunk source basename> <line>[-<line>] ...: host RVAs (hex) of the code
generated for those source lines, from the PDB's line tables (llvm-pdbutil's native reader; DIA and
dbghelp refuse this PDB). The .text section is assumed at RVA 0x1000 (lld's default).

Used with PANDORA_HOST_PATCH to patch a recompiled branch without rebuilding."""
import re
import subprocess
import sys

PDBUTIL = r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe'
pdb, src = sys.argv[1], sys.argv[2]
mods = subprocess.run([PDBUTIL, 'dump', '--modules', pdb], capture_output=True, text=True, check=True).stdout
m = re.search(r'Mod (\d+) \| `[^`]*[/\\]' + re.escape(src) + r'\.obj`', mods)
if not m:
    sys.exit('module not found: ' + src)
dump = subprocess.run([PDBUTIL, 'dump', '-l', '--modi=' + str(int(m[1])), pdb], capture_output=True, text=True,
                      check=True).stdout
lines = {}
for ln, off in re.findall(r'(\d+) ([0-9A-F]{8}) !', dump):
    lines.setdefault(int(ln), []).append(0x1000 + int(off, 16))
for spec in sys.argv[3:]:
    a, b = (spec.split('-') + [spec])[:2]
    for n in range(int(a), int(b) + 1):
        for rva in sorted(lines.get(n, [])):
            print('%d %x' % (n, rva))
