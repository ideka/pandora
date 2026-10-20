"""Convert a DOL to an ELF (PowerPC, big-endian) with symbols, for disassemblers.

Every DOL section becomes a loadable segment at its address (plus .bss), and every function/object
in the symbols file becomes an ELF symbol, so Binary Ninja/Ghidra/IDA open it with names.

Usage: dol2elf.py <main.dol> <symbols.txt> <out.elf>
"""
import re
import struct
import sys

from dolutil import SECTION_NAMES, Dol

dol = Dol(sys.argv[1])
sym_re = re.compile(r'^(\S+) = (\S+):0x([0-9A-Fa-f]+); // .*type:(\w+)(?: size:0x([0-9A-Fa-f]+))?')
symbols = []
for line in open(sys.argv[2], encoding='utf-8'):
    m = sym_re.match(line)
    if m and m[4] in ('function', 'object'):
        symbols.append((m[1], int(m[3], 16), int(m[5] or '0', 16), m[4] == 'function'))

sections = []  # (name, addr, data or None for nobits, is_text)
for i, va, size, off in dol.sections():
    name = SECTION_NAMES[i] or ('.text%d' % i if i < 7 else '.data%d' % i)
    if any(s[0] == name for s in sections):
        name += str(i)
    sections.append((name, va, dol.data[off:off + size], i < 7))
if dol.bss_size:
    sections.append(('.bss', dol.bss_addr, None, False))

shstr = b'\0'
strtab = b'\0'
body = b''
EHDR, PHDR, SHDR = 52, 32, 40
loadable = [s for s in sections if s[2] is not None]
data_off = EHDR + PHDR * (len(loadable) + 1)
offsets = []
for name, va, data, _ in sections:
    if data is None:
        offsets.append(data_off + len(body))
        continue
    pad = (-len(body)) % 32
    body += b'\0' * pad
    offsets.append(data_off + len(body))
    body += data


def sec_index(addr):
    for k, (_, va, data, _) in enumerate(sections):
        size = len(data) if data is not None else dol.bss_size
        if va <= addr < va + size:
            return k + 1
    return 0xFFF1  # SHN_ABS


symtab = b'\0' * 16
for name, addr, size, is_fn in symbols:
    name_off = len(strtab)
    strtab += name.encode() + b'\0'
    info = (1 << 4) | (2 if is_fn else 1)  # STB_GLOBAL, STT_FUNC/STT_OBJECT
    symtab += struct.pack('>IIIBBH', name_off, addr, size, info, 0, sec_index(addr))

extra = []  # (name, type, data, link, entsize)
extra.append(('.symtab', 2, symtab, len(sections) + 2, 16))
extra.append(('.strtab', 3, strtab, 0, 0))
extra.append(('.shstrtab', 3, None, 0, 0))

shdrs = [b'\0' * SHDR]
names = [s[0] for s in sections] + [e[0] for e in extra]
name_offs = []
for n in names:
    name_offs.append(len(shstr))
    shstr += n.encode() + b'\0'
for k, (name, va, data, is_text) in enumerate(sections):
    flags = 0x2 | (0x4 if is_text else 0x1)  # ALLOC | EXECINSTR or WRITE
    size = len(data) if data is not None else dol.bss_size
    shdrs.append(struct.pack('>IIIIIIIIII', name_offs[k], 1 if data is not None else 8, flags, va,
                             offsets[k], size, 0, 0, 32, 0))
for j, (name, typ, data, link, entsize) in enumerate(extra):
    if data is None:
        data = shstr
    off = data_off + len(body)
    body += data
    info = 1 if typ == 2 else 0  # symtab: index of the first global
    shdrs.append(struct.pack('>IIIIIIIIII', name_offs[len(sections) + j], typ, 0, 0, off, len(data), link,
                             info, 4, entsize))
shoff = data_off + len(body) + ((-len(body)) % 4)
body += b'\0' * ((-len(body)) % 4)

phdrs = b''
for k, (name, va, data, is_text) in enumerate(sections):
    if data is None:
        continue
    phdrs += struct.pack('>IIIIIIII', 1, offsets[k], va, va, len(data), len(data), 5 if is_text else 6, 32)
phdrs += struct.pack('>IIIIIIII', 1, 0, dol.bss_addr, dol.bss_addr, 0, dol.bss_size, 6, 32)

ehdr = b'\x7fELF\x01\x02\x01' + b'\0' * 9 + struct.pack(
    '>HHIIIIIHHHHHH', 2, 20, 1, dol.entry, EHDR, shoff, 0, EHDR, PHDR, len(loadable) + 1, SHDR,
    len(shdrs), len(sections) + 3)
with open(sys.argv[3], 'wb') as f:
    f.write(ehdr + phdrs + body + b''.join(shdrs))
print(f'{sys.argv[3]}: {len(sections)} sections, {len(symbols)} symbols')
