"""Match dtk-discovered functions against a Dolphin .dsy hash signature database.

Usage: match_dsy.py <main.dol> <symbols.txt> <db.dsy> [out.tsv]

The hash (Dolphin's HashSignatureDB::ComputeCodeChecksum, GPL-2.0+) keeps only opcode bits, so
matches are only accepted when the stored function size also equals dtk's size. Short functions
collide easily and are flagged low-confidence.
"""
import os, re, struct, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dolutil import Dol

MIN_CONFIDENT = 0x40


def checksum(dol, start, size):
    s = 0
    for a in range(start, start + size, 4):
        w = dol.word(a)
        op = w & 0xFC000000
        aux = op >> 26
        op2 = op3 = 0
        if aux == 4:
            op2 = w & 0x3F
            if op2 in (0, 8, 16, 21, 22):
                op3 = w & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = w & 0x03FF0000
        elif aux in (19, 31, 63):
            op2 = w & 0x7FF
        elif aux == 59:
            op2 = w & 0x3F
            if op2 < 16:
                op3 = w & 0x7C0
        elif 32 <= aux < 56:
            op2 = w & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x0001FFFF)
        s ^= op | op2 | op3
    return s


def load_dsy(path):
    d = open(path, 'rb').read()
    # Dolphin writes the file with native (little-endian) byte order.
    (n,) = struct.unpack('<I', d[:4])
    db = defaultdict(list)
    for i in range(n):
        c, sz = struct.unpack('<II', d[4 + i * 136:12 + i * 136])
        name = d[12 + i * 136:140 + i * 136].split(b'\0')[0].decode('latin1')
        name = name.split('\t')[0].strip()  # entries are "name \tlibrary"
        db[c].append((sz, name))
    return db


def load_functions(path):
    pat = re.compile(r'^(\S+) = (\S+):0x([0-9A-Fa-f]+); // .*type:function size:0x([0-9A-Fa-f]+)')
    for line in open(path):
        m = pat.match(line)
        if m:
            yield m[1], int(m[3], 16), int(m[4], 16)


dol = Dol(sys.argv[1])
db = load_dsy(sys.argv[3])
rows = []
for name, addr, size in load_functions(sys.argv[2]):
    cands = [n for sz, n in db.get(checksum(dol, addr, size), []) if sz == size]
    if cands:
        rows.append((addr, size, name, sorted(set(cands))))

# A DB name that matches several functions here is generic code (accessors, thunks), not an
# identification.
hits = defaultdict(int)
for _, _, _, cands in rows:
    for c in cands:
        hits[c] += 1


def confident(size, cands):
    return size >= MIN_CONFIDENT and len(cands) == 1 and hits[cands[0]] == 1


out = open(sys.argv[4], 'w') if len(sys.argv) > 4 else sys.stdout
for addr, size, cur, cands in rows:
    conf = 'high' if confident(size, cands) else 'low'
    out.write(f'{addr:08X}\t{size:#x}\t{conf}\t{cur}\t{"|".join(cands)}\n')
print(f'db entries: {sum(map(len, db.values()))}, matched functions: {len(rows)}, '
      f'high-confidence: {sum(1 for r in rows if confident(r[1], r[3]))}', file=sys.stderr)
