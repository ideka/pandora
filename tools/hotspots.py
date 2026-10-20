"""Instruction-level view of a host profile dump (PANDORA_PROFILE_DUMP=<file> with --profile).

Prints: samples by guest instruction mnemonic (for the recompiled chunks), the hottest functions,
and for the hottest ones the hottest host instructions, disassembled from the executable and
annotated with sample counts and the guest instruction they implement.

Usage: hotspots.py <dump> <pandora.exe> [--functions N] [--insns N] [--chunks-only]
"""
import collections
import os
import re
import subprocess
import sys

LLVM = r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin'
dump, exe = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
n_functions = int(args[args.index('--functions') + 1]) if '--functions' in args else 8
n_insns = int(args[args.index('--insns') + 1]) if '--insns' in args else 40
chunks_only = '--chunks-only' in args

samples = []  # (rva, count, function, disp, file, line)
total = 0
for text in open(dump, encoding='utf-8', errors='replace'):
    if text.startswith('#'):
        total = int(text.split()[2])
        continue
    m = re.match(r'^([0-9a-f]+) (\d+) (.*)\+([0-9a-f]+) (.*):(\d+)$', text.strip())
    if not m:
        continue
    samples.append((int(m[1], 16), int(m[2]), m[3], int(m[4], 16), m[5], int(m[6])))

# Guest instruction for a chunk source line: the nearest "    // <addr>: <insn>" comment above.
source_cache = {}


def guest_insn(file, line):
    if 'chunk_' not in file or line <= 0:
        return None
    if file not in source_cache:
        try:
            with open(file, encoding='utf-8', errors='replace') as f:
                lines = f.read().split('\n')
        except OSError:
            lines = []
        marks = {}
        last = None
        for i, l in enumerate(lines, 1):
            if l.startswith('    // ') and len(l) > 15 and l[15] == ':' and l[7:15].isalnum():
                last = l[7:].strip()
            elif l.startswith('label_'):
                pass
            marks[i] = last
        source_cache[file] = marks
    return source_cache[file].get(line)


by_fn = collections.Counter()
by_mnemonic = collections.Counter()
chunk_total = 0
for rva, count, fn, disp, file, line in samples:
    by_fn[fn] += count
    insn = guest_insn(file, line)
    if fn.startswith('func_'):
        chunk_total += count
        if insn:
            mnem = insn.split(':', 1)[1].split()[0] if ':' in insn else '?'
            by_mnemonic[mnem] += count
        else:
            by_mnemonic['(no line)' if line == 0 else '(prologue/switch)'] += count

print(f'{total} samples; {100.0 * chunk_total / total:.1f}% in recompiled chunks')
print('\nchunk samples by guest instruction (share of all samples):')
for mnem, c in by_mnemonic.most_common(40):
    print(f'  {100.0 * c / total:5.1f}%  {mnem}')

print('\nhottest functions:')
top = [fn for fn, c in by_fn.most_common() if not chunks_only or fn.startswith('func_')][:n_functions]
for fn in top:
    print(f'  {100.0 * by_fn[fn] / total:5.1f}%  {fn}')

base = 0
for l in subprocess.run([os.path.join(LLVM, 'llvm-readobj.exe'), '--file-headers', exe], capture_output=True,
                        text=True).stdout.split('\n'):
    if 'ImageBase:' in l:
        base = int(l.split(':')[1], 16)


def disassemble(lo, hi):
    out = subprocess.run([os.path.join(LLVM, 'llvm-objdump.exe'), '-d', '--no-show-raw-insn', '-M', 'intel',
                          f'--start-address={base + lo:#x}', f'--stop-address={base + hi:#x}', exe],
                         capture_output=True, text=True).stdout
    insns = {}
    for l in out.split('\n'):
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
        if m:
            insns[int(m[1], 16) - base] = m[2].strip()
    return insns


for fn in top:
    rows = sorted((s for s in samples if s[2] == fn), key=lambda s: -s[1])[:n_insns]
    if not rows:
        continue
    print(f'\n== {fn}: {100.0 * by_fn[fn] / total:.1f}% ({by_fn[fn]} samples); hottest host instructions:')
    lo = min(r[0] for r in rows)
    hi = max(r[0] for r in rows) + 16
    insns = disassemble(lo, hi) if hi - lo < 0x200000 else {}
    for rva, count, _, disp, file, line in sorted(rows):
        g = guest_insn(file, line) or ''
        print(f'  {count:5d}  +{disp:06x}  {insns.get(rva, "?"):50.50s}  {g}')
