"""Find bctr dispatch tables that decomp-toolkit would mis-size, and emit symbols.txt hints.

dtk infers a table's size from the index bound (cmplwi) or, failing that, the rlwinm mask.
Unbounded dispatches (e.g. bytecode handler tables indexed by a raw byte) overrun into
adjacent data. For those we measure the real extent and emit a sized object symbol.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dolutil import Dol, SECTION_NAMES, simm

dol = Dol(sys.argv[1])
code = dict(dol.code())


def op(w):
    return w >> 26


def xo(w):
    return (w >> 1) & 0x3ff


# Small-data base registers, from __init_registers (lis/ori r2, r13 in .init).
SDA = {2: 0x807FAF40, 13: 0x807F79C0}

# Every absolute address the code materialises via lis+addi/ori or lis+load/store displacement.
refs = set()
hi = {}
for a, w in sorted(code.items()):
    o = op(w)
    rD, rA = (w >> 21) & 31, (w >> 16) & 31
    if o == 15 and rA == 0:  # lis
        hi[rD] = (w & 0xffff) << 16
        continue
    if o in (14, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54):  # addi / lwz.. / stw.. / lfs..
        if rA in SDA:
            refs.add((SDA[rA] + simm(w)) & 0xffffffff)
        elif rA in hi:
            refs.add((hi[rA] + simm(w)) & 0xffffffff)
    if o == 24 and rA in hi:  # ori (rA is dest for ori)
        pass

results = []
for a, w in sorted(code.items()):
    if w != 0x4e800420:  # bctr
        continue
    # Walk back: mtctr rX <- lwzx rX, rB, rI ; rB <- addi (lis); rI <- rlwinm/slwi
    window = [(b, code.get(b)) for b in range(a - 4, a - 4 * 40, -4) if code.get(b) is not None]
    ctr_reg = idx_reg = base_reg = None
    base = None
    bounded = False
    mask_max = None
    for b, x in window:
        o = op(x)
        if ctr_reg is None:
            if o == 31 and xo(x) == 467 and ((x >> 11) & 0x3ff) == 0x120:  # mtctr
                ctr_reg = (x >> 21) & 31
            continue
        if base_reg is None:
            if o == 31 and xo(x) == 23 and (x >> 21) & 31 == ctr_reg:  # lwzx
                base_reg, idx_reg = (x >> 16) & 31, (x >> 11) & 31
            continue
        if base is None and o == 14 and (x >> 21) & 31 == base_reg:  # addi base, rS, lo
            lo, src = simm(x), (x >> 16) & 31
            if src in SDA:
                base = (SDA[src] + lo) & 0xffffffff
            for c, y in window if base is None else ():
                if c < b and op(y) == 15 and (y >> 21) & 31 == src:
                    base = (((y & 0xffff) << 16) + lo) & 0xffffffff
                    break
        if o == 21 and (x >> 16) & 31 == idx_reg and mask_max is None:  # rlwinm idx
            sh, mb, me = (x >> 11) & 31, (x >> 6) & 31, (x >> 1) & 31
            m = 0
            for bit in range(mb, me + 1):
                m |= 1 << (31 - bit)
            mask_max = m
        if o in (10, 11) or (o == 31 and xo(x) in (0, 32)):  # cmplwi / cmpwi / cmpw / cmplw
            bounded = True
        if o == 18 or (o == 19 and xo(x) == 16) or x == 0x4e800020:  # earlier branch/blr: block start
            break
    if base is None or bounded or dol.section_of(base) is None or dol.is_text(base):
        continue
    # Measure: stop at first invalid entry or at the next referenced address.
    limit = base + (mask_max + 4 if mask_max is not None else 0x400)
    nxt = min([r for r in refs if base < r < limit] + [limit])
    end = base
    while end < nxt:
        e = dol.word(end)
        if e is None or (e != 0 and not dol.is_text(e)):
            break
        end += 4
    results.append((base, end - base, a, mask_max))

def is_prologue(a):
    w = dol.word(a)
    return w is not None and (w & 0xffff8000) == 0x94218000  # stwu r1, -N(r1)


functions = set()
for base, size, at, mask in results:
    sec = SECTION_NAMES[dol.section_of(base)]
    targets = [dol.word(base + i) for i in range(0, size, 4)]
    targets = [t for t in targets if t]
    # A case body never opens a new stack frame, so a prologue at any target means this is a
    # table of separate functions reached by a tail call, not a switch.
    kind = 'functions' if any(is_prologue(t) for t in targets) else 'switch'
    if kind == 'functions':
        functions.update(targets)
    print(f'dispatch_table_{base:08x} = {sec}:0x{base:08X}; // type:object size:0x{size:X} scope:local'
          f'   ## bctr@{at:08x} mask={mask:#x} -> {size // 4} entries, {kind}')

for t in sorted(functions):
    print(f'fn_{t:08X} = .text:0x{t:08X}; // type:function   ## dispatch table target')
