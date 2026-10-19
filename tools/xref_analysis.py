"""Structural pass over every dtk function: source-file attribution, GX FIFO writers, stack
switchers, GQR writers.

Usage: xref_analysis.py <main.dol> <symbols.txt> <dsy_matches.tsv> <out_dir>
"""
import os, re, sys, json
from collections import defaultdict, Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dolutil import Dol, simm

SDA = {2: 0x807FAF40, 13: 0x807F79C0}
WGPIPE = 0xCC008000
STORES = {36: 'stw', 37: 'stwu', 38: 'stb', 44: 'sth', 52: 'stfs', 54: 'stfd'}

dol = Dol(sys.argv[1])
fn_re = re.compile(r'^(\S+) = \S+:0x([0-9A-Fa-f]+); // .*type:function size:0x([0-9A-Fa-f]+)')
funcs = [(int(m[2], 16), int(m[3], 16), m[1]) for m in map(fn_re.match, open(sys.argv[2])) if m]
names = {a: n for a, _, n in funcs}
for line in open(sys.argv[3]):
    a, size, conf, cur, cands = line.rstrip('\n').split('\t')
    if conf == 'high' and cur.startswith('fn_'):
        names[int(a, 16)] = cands.strip()


def cstr(a):
    out = bytearray()
    while len(out) < 128:
        w = dol.word(a & ~3)
        if w is None:
            return None
        b = w.to_bytes(4, 'big')[a & 3]
        if b == 0:
            return out.decode('latin1')
        if not 0x20 <= b < 0x7f:
            return None
        out.append(b)
        a += 1
    return None


src_of = {}
gx_writers, stack_switch, gqr_writers = [], [], []
for addr, size, _ in funcs:
    hi, files, fifo, sw, gqr, aligned = {}, set(), 0, [], 0, False
    for a in range(addr, addr + size, 4):
        w = dol.word(a)
        o, rD, rA = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if o == 15 and rA == 0:
            hi[rD] = (w & 0xffff) << 16
            continue
        if o in (14, 24) or o in STORES or o in (32, 34, 40, 48, 50):
            base = SDA.get(rA) if rA in SDA else hi.get(rA)
            if base is not None:
                t = ((base | (w & 0xffff)) if o == 24 else (base + simm(w))) & 0xffffffff
                if o in STORES and (t & ~0x1f) == WGPIPE:
                    fifo += 1
                elif o in (14, 24) and 0x80000000 <= t < 0x81800000:
                    s = cstr(t)
                    if s and s.endswith(('.cpp', '.c')):
                        files.add(s)
        # r1 reloaded from memory (lwz r1, d(rX), rX != r1) or moved from another GPR.
        if o == 32 and rD == 1 and rA != 1:
            sw.append(a)
        if o == 31 and ((w >> 1) & 0x3ff) == 444 and rA == 1 and rD == ((w >> 11) & 31) and rD != 1:
            # Skip back-chain restores: lwz rX, 0(r1) shortly before mr r1, rX.
            if not any(dol.word(b) == (0x80010000 | (rD << 21)) for b in range(a - 16, a, 4)):
                sw.append(a)
        if o == 31 and ((w >> 1) & 0x3ff) == 183 and rD == 1 and rA == 1:  # stwux r1, r1, rN
            aligned = True
        if o == 31 and ((w >> 1) & 0x3ff) == 467:
            spr = ((w >> 16) & 0x1f) | (((w >> 11) & 0x1f) << 5)
            if 912 <= spr <= 919:
                gqr += 1
    if files:
        src_of[addr] = sorted(files)
    if fifo:
        gx_writers.append((addr, size, fifo))
    # CodeWarrior aligned frames (stwux r1,r1,rN ... mr r1,rM) restore r1 the same way; not a switch.
    if sw and not aligned:
        stack_switch.append((addr, size, sw))
    if gqr:
        gqr_writers.append((addr, gqr))

od = sys.argv[4]
os.makedirs(od, exist_ok=True)
with open(os.path.join(od, 'source_files.tsv'), 'w') as f:
    for a in sorted(src_of):
        f.write(f'{a:08X}\t{names[a]}\t{",".join(src_of[a])}\n')
with open(os.path.join(od, 'gx_fifo_writers.tsv'), 'w') as f:
    for a, s, n in gx_writers:
        f.write(f'{a:08X}\t{s:#x}\t{n}\t{names[a]}\n')
with open(os.path.join(od, 'stack_switchers.tsv'), 'w') as f:
    for a, s, at in stack_switch:
        f.write(f'{a:08X}\t{s:#x}\t{names[a]}\t{" ".join(f"{x:08X}" for x in at)}\n')

files = Counter(x for v in src_of.values() for x in v)
print(f'functions: {len(funcs)}')
print(f'functions referencing a source-file string: {len(src_of)} ({len(files)} distinct files)')
print(f'GX FIFO writers: {len(gx_writers)} functions, {sum(n for *_, n in gx_writers)} stores; '
      f'named GX*: {sum(1 for a, *_ in gx_writers if names[a].startswith("GX"))}')
print(f'stack switchers: {len(stack_switch)}')
for a, s, at in stack_switch:
    print(f'  {a:08X} {s:#6x} {names[a]:40} src={",".join(src_of.get(a, []))}')
print(f'GQR writers: {len(gqr_writers)}')
