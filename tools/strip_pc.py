"""Source-level rewrites of DolRecomp's generated chunks (applied by sync_dir.py --strip-pc).

- Drop the per-instruction `ctx->pc = <addr>;` stores. The generated code stores the guest pc at
  every instruction, but nothing reads it there: branches set ctx->pc themselves before returning
  to the dispatcher, exception helpers take the instruction address as an argument, and
  interrupts are taken between dispatches. The compiler cannot drop the stores itself (guest
  memory stores through a u8* may alias ctx), so they cost a store per guest instruction. Blocks
  that hand control to code which may read ctx->pc (fallback instructions, including hook markers;
  rfi, sc, program exceptions, mftb) keep theirs.
- ctx is __restrict (guest memory never overlaps the CPU state).
- The chunks' lfs/stfs bit conversions point at the inline versions in fastfp.h.
- The cycle downcount lives in a local (PANDORA_DOWNCOUNT_MEM=1 keeps it in ctx).
- Chunk functions use the System V calling convention (PANDORA_MS_ABI=1 keeps the MS one).
"""
import os
import re

LABEL = re.compile(r'^label_[0-9A-F]{8}:$')
PC_STORE = re.compile(r'^    ctx->pc = 0x[0-9A-F]{8}u;$')
KEEP = ('ppc_fallback_instruction', 'ppc_rfi', 'ppc_system_call', 'ppc_program_exception', 'ppc_mftb')
FUNC_HEAD = re.compile(r'^(void func_[0-9A-F]{8}\(CPUState\*) ctx\) \{', re.M)
FUNC_HEAD_R = re.compile(r'^(void func_[0-9A-F]{8}\(CPUState\* (?:__restrict )?ctx\) \{)(?=\r?\n)', re.M)
CALL_SITE = re.compile(r'^([ \t]+)((?:func|loop)_[0-9A-F]{8}\(ctx\);)(?=\r?\n)', re.M)
LOOP_HEAD = re.compile(r'^(static void loop_[0-9A-F]{8}\(CPUState\* ctx\) \{)(?=\r?\n)', re.M)
# a loop helper's closing brace: the line after its last "ctx->pc = ...;" statement at one indent
LOOP_END = re.compile(r'^(    ctx->pc = 0x[0-9A-F]{8}u;\r?\n)\}(?=\r?\n)', re.M)
FUNC_DECL = re.compile(r'\bvoid (func_[0-9A-F]{8})\(CPUState\*( __restrict)? ctx\)')


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
    if not os.environ.get('PANDORA_NO_RESTRICT'):
        text = FUNC_HEAD.sub(lambda m: m.group(1) + ' __restrict ctx) {', text)
    text = text.replace('dolrecomp_f32_from_bits(', 'pandora_f32_from_bits(')
    text = text.replace('dolrecomp_f32_to_bits(', 'pandora_f32_to_bits(')
    if not os.environ.get('PANDORA_DOWNCOUNT_MEM'):
        text = downcount_local(text, eol)
    if not os.environ.get('PANDORA_MS_ABI'):
        text = sysv_abi(text)
    return text, removed, total


def downcount_local(text, eol):
    """The cycle downcount lives in a local for the whole function instead of ctx->downcount (a
    memory read-modify-write per basic block): loaded on entry, stored at every exit and around
    calls into other chunks (which load and store their own). Nothing else reads it meanwhile."""
    text = FUNC_HEAD_R.sub(lambda m: m.group(1) + eol + '    s64 dc = ctx->downcount;', text)
    # DolRecomp's loop helpers (static void loop_X(CPUState* ctx) { ... }) get their own local; they
    # fall off the end to return, so the closing brace stores it too.
    text = LOOP_HEAD.sub(lambda m: m.group(1) + eol + '    s64 dc = ctx->downcount;', text)
    text = LOOP_END.sub(lambda m: m.group(1) + '    ctx->downcount = dc;' + eol + '}', text)
    text = text.replace('ctx->downcount -= ', 'dc -= ')
    text = text.replace('ctx->downcount <= ', 'dc <= ')
    text = CALL_SITE.sub(lambda m: m.group(1) + 'ctx->downcount = dc; ' + m.group(2) + ' dc = ctx->downcount;', text)
    text = text.replace('return;', '{ ctx->downcount = dc; return; }')
    if not os.environ.get('PANDORA_INDIRECT_DISPATCH'):
        labels = set(re.findall(r'^label_([0-9A-F]{8}):', text, re.M))
        text = INDIRECT_CALL.sub(lambda m: indirect_call(m, labels), text)
    return text


INDIRECT_CALL = re.compile(r'^(\s+)ctx->lr = 0x([0-9A-F]{8})u;\r?\n\s+ctx->pc = target;\r?\n\s+\{ ctx->downcount = dc; return; \}(?=\r?\n)',
                           re.M)


def indirect_call(m, labels):
    """bctrl/blrl (C++ virtual calls, 34k sites): call the target's chunk function directly, as
    DolRecomp's direct cross-chunk bl does, instead of returning to the dispatcher twice (once to
    reach the callee, once more when it returns). The callee's blr leaves pc = lr; if that is our
    return address (and it is in this chunk), continue here, else fall back to the dispatcher."""
    indent, ret = m.group(1), m.group(2)
    eol = '\n'
    lines = [
        f'ctx->lr = 0x{ret}u;',
        'ctx->pc = target;',
        'if (dolrecomp_call_enter()) {',
        '    DolRecompFunction f = dolrecomp_find_original(target);',
        '    if (f) { ctx->downcount = dc; f(ctx); dc = ctx->downcount; }',
        '    dolrecomp_call_leave();',
    ]
    if ret in labels:
        lines.append(f'    if (ctx->pc == 0x{ret}u) goto label_{ret};')
    lines += ['}', '{ ctx->downcount = dc; return; }']
    return eol.join(indent + l for l in lines)


def sysv_abi(text):
    """Chunk functions use the System V calling convention: all XMM registers are caller-saved, so
    a chunk's prologue does not spill seven of them on every entry (the MS ABI's callee-saved
    xmm6-15), and the hot paths rarely call out."""
    text = FUNC_DECL.sub(lambda m: '__attribute__((sysv_abi)) void ' + m.group(1) + '(CPUState*' + (m.group(2) or '') + ' ctx)', text)
    # The dispatcher's function-pointer type (generated.h) must carry the same convention.
    return text.replace('typedef void (*DolRecompFunction)(CPUState* ctx);',
                        'typedef __attribute__((sysv_abi)) void (*DolRecompFunction)(CPUState* ctx);')
