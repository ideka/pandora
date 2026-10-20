"""Drop the per-instruction `ctx->pc = <addr>;` stores from DolRecomp's generated chunks.

The generated code stores the guest pc at every instruction, but nothing reads it there: branches
set ctx->pc themselves before returning to the dispatcher, exception helpers take the instruction
address as an argument, and interrupts are taken between dispatches. The compiler cannot drop the
stores itself (guest memory stores through a u8* may alias ctx), so they cost a store per guest
instruction. Blocks that hand control to code which may read ctx->pc (fallback instructions,
including hook markers; rfi, sc, program exceptions, mftb) keep theirs.

Also points the chunks' lfs/stfs bit conversions at the inline versions in fastfp.h.

Used by sync_dir.py --strip-pc.
"""
import re

LABEL = re.compile(r'^label_[0-9A-F]{8}:$')
PC_STORE = re.compile(r'^    ctx->pc = 0x[0-9A-F]{8}u;$')
KEEP = ('ppc_fallback_instruction', 'ppc_rfi', 'ppc_system_call', 'ppc_program_exception', 'ppc_mftb')


def strip(text):
    """Returns (new text, stores removed, stores seen)."""
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(eol)
    out = []
    total = removed = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        if LABEL.match(line) and i + 1 < len(lines) and PC_STORE.match(lines[i + 1]):
            total += 1
            j = i + 2
            keep = False
            while j < len(lines) and not LABEL.match(lines[j]):
                if any(k in lines[j] for k in KEEP):
                    keep = True
                    break
                j += 1
            if keep:
                out.append(lines[i + 1])
            else:
                removed += 1
            i += 2
            continue
        i += 1
    text = eol.join(out)
    # The float bit conversions: runtime/fastmem/cpu/fastfp.h inlines their common cases.
    text = text.replace('dolrecomp_f32_from_bits(', 'pandora_f32_from_bits(')
    text = text.replace('dolrecomp_f32_to_bits(', 'pandora_f32_to_bits(')
    return text, removed, total
