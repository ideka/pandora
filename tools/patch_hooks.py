"""Write a copy of main.dol with every hooked function's first instruction replaced by a marker.

Usage: patch_hooks.py <main.dol> <symbols.txt> <hooks.txt> <out.dol>

DolRecomp compiles direct calls as plain gotos/C calls, so the runtime cannot intercept them at
dispatch time. The marker (primary opcode 1, unused on Broadway) compiles to a fallback call, which
the runtime routes to the host implementation registered for that address. The runtime still
loads the original DOL into guest memory.
"""
import re, struct, sys

HOOK_MARKER = 0x04C0FFEE

dol_path, sym_path, hooks_path, out_path = sys.argv[1:5]
data = bytearray(open(dol_path, 'rb').read())
off = struct.unpack('>18I', data[0:72])
addr = struct.unpack('>18I', data[72:144])
size = struct.unpack('>18I', data[144:216])

fn_re = re.compile(r'^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // .*type:function')
symbols = {m[1]: int(m[2], 16) for m in map(fn_re.match, open(sym_path)) if m}

names = [l.split('#', 1)[0].strip() for l in open(hooks_path)]
names = [n for n in names if n]
missing = [n for n in names if n not in symbols]
if missing:
    sys.exit(f'unknown hook symbols: {", ".join(missing)}')

for name in names:
    a = symbols[name]
    for i in range(7):
        if addr[i] <= a < addr[i] + size[i]:
            p = off[i] + a - addr[i]
            data[p:p + 4] = struct.pack('>I', HOOK_MARKER)
            break
    else:
        sys.exit(f'{name} ({a:08X}) is not in a text section')

open(out_path, 'wb').write(data)
print(f'marked {len(names)} hooked functions')
