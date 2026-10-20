"""Prune the entry switches of DolRecomp's generated chunks to the addresses that can be entered.

Each chunk function starts with `switch (ctx->pc)` and a case for every instruction, so every
instruction is a merge point and the compiler can keep nothing in registers across instructions.
Only these addresses can be entered from the dispatcher:
  - function starts (symbols.txt) and code addresses stored in data (function pointers, vtables,
    jump tables);
  - every constant the generated code assigns to ctx->pc (branch targets, loop-budget exits,
    fall-through into the next chunk) and the targets of the chunks' return_dispatch switches;
  - the instruction after a call (bl, bctrl, blrl, ...) or sc (return addresses), and after a
    fallback-executed instruction;
  - the continuation addresses that runtime hooks jump to (runtime/hle.cpp: c->pc = ...).
FP-unavailable exceptions, which would return to any FP instruction, are handled in place by
runtime/fpu_switch.cpp. Interrupts are taken between dispatches, at one of the addresses above.
The runtime halts with a message if a chunk is ever entered at an address it does not list.

Used by sync_dir.py --prune-entries.
"""
import re
import struct

ENTRY_SWITCH = re.compile(r'^((?:__attribute__\(\(sysv_abi\)\) )?void func_[0-9A-F]{8}\(CPUState\* (?:__restrict )?ctx\) \{\r?\n'
                          r'(?:    s64 dc = ctx->downcount;\r?\n)?    switch \(ctx->pc\) \{\r?\n)(.*?)'
                          r'(    default: (?:return;|\{ ctx->downcount = dc; return; \})\r?\n    \})', re.S | re.M)
CASE = re.compile(r'    case 0x([0-9A-F]{8})u: goto label_[0-9A-F]{8};\r?\n')
PC_CONST = re.compile(r'ctx->pc = 0x([0-9A-F]{8})u;')
ANY_CASE = re.compile(r'case 0x([0-9A-F]{8})u:')
INSN = re.compile(r'^    // ([0-9A-F]{8}): (\S+)', re.M)
FALLBACK = re.compile(r'ppc_fallback_instruction\(ctx, 0x[0-9A-F]{8}u, 0x([0-9A-F]{8})u\)')
LINK = {'bl', 'bla', 'bctrl', 'blrl', 'bcl', 'bcla', 'bcctrl', 'bclrl', 'sc'}


def collect(texts, symbols_path, dol_path, hle_path):
    entries = set()
    for text in texts:
        m = ENTRY_SWITCH.search(text)
        rest = text[m.end():] if m else text
        entries.update(int(x, 16) for x in PC_CONST.findall(text))
        entries.update(int(x, 16) for x in ANY_CASE.findall(rest))
        for addr, mnemonic in INSN.findall(text):
            if mnemonic in LINK:
                entries.add(int(addr, 16) + 4)
        entries.update(int(x, 16) + 4 for x in FALLBACK.findall(text))
    fn = re.compile(r'^\S+ = \.\w+:0x([0-9A-Fa-f]+); // type:function')
    for line in open(symbols_path, encoding='utf-8'):
        if m := fn.match(line):
            entries.add(int(m[1], 16))
    entries.update(int(x, 16) for x in re.findall(r'c->pc = 0x([0-9A-Fa-f]{8})', open(hle_path, encoding='utf-8').read()))
    # Code addresses stored in data.
    d = open(dol_path, 'rb').read()
    off = struct.unpack('>18I', d[0:72])
    addr = struct.unpack('>18I', d[72:144])
    size = struct.unpack('>18I', d[144:216])
    text_ranges = [(addr[i], addr[i] + size[i]) for i in range(7) if size[i]]
    for i in range(7, 18):
        if not size[i]:
            continue
        data = d[off[i]:off[i] + size[i]]
        for (w,) in struct.iter_unpack('>I', data[:len(data) & ~3]):
            if w & 3 == 0 and any(lo <= w < hi for lo, hi in text_ranges):
                entries.add(w)
    return entries


def prune(text, entries):
    """Returns (new text, cases kept, cases seen)."""
    m = ENTRY_SWITCH.search(text)
    if not m:
        return text, 0, 0
    cases = CASE.findall(m.group(2))
    eol = '\r\n' if '\r\n' in m.group(2) else '\n'
    kept = [c for c in cases if int(c, 16) in entries]
    body = ''.join(f'    case 0x{c}u: goto label_{c};{eol}' for c in kept)
    ret = '{ ctx->downcount = dc; return; }' if 'ctx->downcount = dc' in m.group(3) else 'return;'
    default = f'    default: dolrecomp_missing_entry(ctx); {ret}{eol}    }}'
    return text[:m.start()] + m.group(1) + body + default + text[m.end():], len(kept), len(cases)
