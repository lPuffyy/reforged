"""Feasibility survey for the static x86 lift: python -I tools/lift_survey.py <default.xbe>
Recursive-descent disassembly from the entry point plus every call target found, then a census of mnemonics and
indirect control flow. Prints what a lifter has to support; writes work/lift/funcs.txt (function entry list)."""
import struct, sys, collections, os
from capstone import *
from capstone.x86 import *

d = open(sys.argv[1], 'rb').read()
base, = struct.unpack_from('<I', d, 0x104); nsec, = struct.unpack_from('<I', d, 0x11C); sa, = struct.unpack_from('<I', d, 0x120)
entry_enc, = struct.unpack_from('<I', d, 0x128)
secs = []
for i in range(nsec):
    fl, v, vs, raw, rs, na = struct.unpack_from('<IIIIII', d, sa - base + i * 56)
    nm_va, = struct.unpack_from('<I', d, sa - base + i * 56 + 20)
    nm = d[nm_va - base: nm_va - base + 16].split(b'\0')[0].decode('latin1') if nm_va >= base else '?'
    secs.append((nm, fl, v, vs, raw, rs))
for s in secs: print('section %-8s flags=%08x va=%08x vsize=%07x raw=%07x rsize=%07x' % s)
# entry point: retail key XOR (retail 0xA8FC57AB, debug 0x94859D4B, chihiro 0x40B5C16E)
entry = None
for k in (0xA8FC57AB, 0x94859D4B, 0x40B5C16E):
    e = entry_enc ^ k
    if any(v <= e < v + vs for (_, _, v, vs, _, _) in secs): entry = e; break
print('entry', hex(entry) if entry else 'unknown')
code = [(n, v, vs, raw, rs) for (n, fl, v, vs, raw, rs) in secs if (fl & 4) or n.lower() in ('.text', 'text')]   # XBE_SEC_EXECUTABLE = 4
def in_code(a): return any(v <= a < v + rs for (_, v, vs, raw, rs) in code)
def rd(a, n):
    for (_, v, vs, raw, rs) in code:
        if v <= a < v + rs: return d[raw + a - v: raw + a - v + min(n, v + rs - a)]
    return b''

md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
# Seeds: the entry point, every aligned dword in the data sections that points into code (vtables, callback tables), and every direct call target found by a linear sweep.
seeds = set([entry]) if entry else set()
dsecs = [(v, d[raw:raw + rs]) for (n, fl, v, vs, raw, rs) in secs if not ((fl & 4) or n.lower() in ('.text', 'text')) and n not in ('$$XTIMAGE', '$$XSIMAGE', 'DSPImage')]
nptr = 0
for (v, blob) in dsecs:
    for i in range(0, len(blob) - 3, 4):
        t, = struct.unpack_from('<I', blob, i)
        if in_code(t): seeds.add(t); nptr += 1
print('data->code pointers: %d (%d distinct)' % (nptr, len(seeds)))
# Vtables and callback tables also sit inside sections the loader marks executable (DSOUND, XONLINE, ...): scan those too, but only accept targets that look like function starts.
mdl = Cs(CS_ARCH_X86, CS_MODE_32); mdl.detail = False
def looks_like_start(t):
    b0 = rd(t, 3)
    if not b0: return False
    first = next(mdl.disasm(rd(t, 16), t, 1), None)
    if first is None: return False
    return (b0[:1] in (bytes([0x55]), bytes([0x53]), bytes([0x56]), bytes([0x57]), bytes([0x51]), bytes([0x52]), bytes([0xB8])) or b0[:2] in (bytes([0x83, 0xEC]), bytes([0x81, 0xEC]), bytes([0x33, 0xC0]), bytes([0x8B, 0x44]), bytes([0x8B, 0x4C]), bytes([0x8B, 0x54]), bytes([0x8B, 0x45])) or first.mnemonic in ('mov', 'sub', 'push', 'xor', 'cmp', 'test', 'lea', 'call'))
ncode = 0
for (n_, v_, vs_, raw_, rs_) in code:
    blob = d[raw_:raw_ + rs_]
    for i_ in range(0, len(blob) - 3, 4):
        t_, = struct.unpack_from('<I', blob, i_)
        if t_ != v_ + i_ and in_code(t_) and t_ not in seeds and looks_like_start(t_): seeds.add(t_); ncode += 1
print('code-section pointer table seeds: +%d' % ncode)

mdl = Cs(CS_ARCH_X86, CS_MODE_32); mdl.detail = False
ncall = 0
for (n, v, vs, raw, rs) in code:
    blob = d[raw:raw + rs]; i = 0
    while i < len(blob) - 4:
        if blob[i] == 0xE8:   # call rel32: accept only if the target is in code and the byte before the call isn't mid-immediate garbage (cheap filter: target is in code)
            t = (v + i + 5 + struct.unpack_from('<i', blob, i + 1)[0]) & 0xFFFFFFFF
            if in_code(t): seeds.add(t); ncall += 1
        i += 1
print('linear-sweep call targets: %d; total seeds %d' % (ncall, len(seeds)))
# Dynamic discovery: native fallback targets recorded by the runtime (work/lift/native_targets.txt, XBOX_LIFT_STATS=1) are real call targets the static pass missed.
for extra in ('work/lift/native_targets.txt', 'work/lift/extra_funcs.txt'):
    if os.path.exists(extra):
        n0 = len(seeds)
        for ln in open(extra).read().split():
            try: v = int(ln, 16)
            except ValueError: continue
            if in_code(v): seeds.add(v)
        print('%s: +%d seeds' % (extra, len(seeds) - n0))
work = list(seeds)
funcs = set(work); seen = set(); mn = collections.Counter(); ind_calls = ind_jmps = rets = 0; bad = 0; insns = 0
jt_cands = set(); unk = collections.Counter(); groups = collections.Counter()
term_ends = set(); btargets = set(); jtargets = set()
def descend():
    global ind_calls, ind_jmps, rets, bad, insns
    while work:
        f = work.pop(); stack = [f]
        while stack:
            a = stack.pop()
            while in_code(a) and a not in seen:
                buf = rd(a, 16); ins = next(md.disasm(buf, a, 1), None)
                if ins is None: bad += 1; break
                seen.add(a); insns += 1; mn[ins.mnemonic] += 1
                if ins.id in (X86_INS_RET, X86_INS_JMP, X86_INS_IRET, X86_INS_HLT, X86_INS_INT3): term_ends.add(a + ins.size)
                if ins.id not in (X86_INS_CALL, X86_INS_JMP) and X86_GRP_JUMP not in ins.groups:
                    for o in ins.operands:
                        if o.type == X86_OP_IMM and o.size == 4 and in_code(o.imm & 0xFFFFFFFF) and (o.imm & 0xFFFFFFFF) not in funcs: funcs.add(o.imm & 0xFFFFFFFF); work.append(o.imm & 0xFFFFFFFF)
                for g in ins.groups: groups[md.group_name(g)] += 1
                nxt = a + ins.size
                if ins.id in (X86_INS_CALL,):
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm
                        if in_code(t) and t not in funcs: funcs.add(t); work.append(t)
                    else: ind_calls += 1
                    a = nxt; continue
                if ins.id == X86_INS_RET or ins.id == X86_INS_IRET: rets += 1; break
                if ins.id == X86_INS_JMP:
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM: btargets.add(op.imm); jtargets.add(op.imm); a = op.imm; continue
                    ind_jmps += 1
                    if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index != 0 and op.mem.disp: jt_cands.add(op.mem.disp)   # jmp [table + idx*4]
                    break
                if X86_GRP_JUMP in ins.groups:   # conditional / loop / jcxz
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM: btargets.add(op.imm); stack.append(op.imm)
                    a = nxt; continue
                if ins.id in (X86_INS_HLT, X86_INS_INT3, X86_INS_UD2): break
                a = nxt
descend()
# Gap filling: stretches of code that no call/jump reached (functions only reached through vtables, callbacks or tail jumps) start after padding; seed their first real instruction.
for rnd in range(4):
    new = []
    for (n_, v_, vs_, raw_, rs_) in code:
        a_ = v_; end_ = v_ + rs_
        while a_ < end_:
            if a_ in seen: a_ += 1; continue
            s_ = a_
            while a_ < end_ and a_ not in seen: a_ += 1
            p_ = next((t_ for t_ in range(s_, min(s_ + 4, a_)) if t_ in term_ends), s_)   # operand bytes of the final ret/jmp are not in `seen`: start after the terminator
            while p_ < a_ and rd(p_, 1) in (bytes([0xcc]), bytes([0x90]), bytes([0])): p_ += 1
            if not any(t_ in term_ends for t_ in range(s_, p_ + 1)) or a_ - p_ < 4 or p_ in funcs: continue   # only directly after a function terminator
            b0 = rd(p_, 3); first = next(md.disasm(rd(p_, 16), p_, 1), None)
            if first is None: continue
            pro = (b0[:1] in (bytes([0x55]), bytes([0x53]), bytes([0x56]), bytes([0x57]), bytes([0x51]), bytes([0x52]), bytes([0xB8])) or b0[:2] in (bytes([0x83, 0xEC]), bytes([0x81, 0xEC]), bytes([0x33, 0xC0]), bytes([0x8B, 0x44]), bytes([0x8B, 0x4C]), bytes([0x8B, 0x54]), bytes([0x8B, 0x45])) or first.mnemonic not in ('ret', 'int3', 'nop', 'hlt', 'iret', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'cli', 'sti', 'pop', 'leave', 'int', 'into', 'popal', 'popfd', 'sahf', 'lahf', 'cld', 'std', 'clc', 'stc', 'les', 'lds', 'arpl', 'bound', 'aaa', 'aas', 'daa', 'das', 'adc', 'sbb', 'salc', 'xlatb', 'wait', 'fwait', 'enter', 'retf', 'lret') and not (first.mnemonic == 'add' and first.op_str.startswith('byte ptr [e')) and not (first.mnemonic == 'or' and first.op_str.startswith('byte ptr [e')))
            if pro: new.append(p_)
    if not new: break
    for t_ in new: funcs.add(t_); work.append(t_)
    print('gap fill round %d: +%d seeds' % (rnd, len(new)))
    descend()
# An address right after a terminator that descent reached but nothing branches to is a function entry (tiny leaf functions packed without padding, reached through indirect calls).
leaf_new = [t_ for t_ in term_ends if t_ in seen and t_ not in funcs and (t_ not in btargets or t_ in jtargets)]
for t_ in leaf_new: funcs.add(t_); work.append(t_)
print('unbranched post-terminator function entries: +%d' % len(leaf_new))
descend()
# Tiny leaf functions (<= 6 instructions ending in ret/jmp) packed back to back and reached through jcc/indirect calls: every such post-terminator address becomes an entry (cheap duplicates).
small_new = []
for t_ in term_ends:
    if t_ not in seen or t_ in funcs: continue
    n_ = 0; ok_ = False
    for i_ in md.disasm(rd(t_, 48), t_):
        n_ += 1
        if i_.mnemonic in ('ret', 'jmp'): ok_ = True; break
        if n_ > 6: break
    if ok_ and n_ <= 6: small_new.append(t_)
for t_ in small_new: funcs.add(t_); work.append(t_)
print('small leaf function entries: +%d' % len(small_new))
descend()
# MSVC pads between functions with 0xCC: an address right after padding that descent reached only as the middle of another function (tail jmp / vtable thunk) is really a function entry.
pad_new = []
BAD_ = ('in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'hlt', 'cli', 'sti', 'int', 'into', 'iret', 'les', 'lds', 'arpl', 'bound', 'aaa', 'aas', 'daa', 'das', 'aam', 'aad', 'salc', 'retf', 'lret', 'ljmp', 'lcall', 'enter', 'wbinvd', 'rdmsr', 'wrmsr', 'clts', 'ltr', 'lar')
def decodes_to_return_(t0):   # unseen candidate: plain code that runs to a ret/jmp within 400 bytes without undecodable or privileged instructions
    n_ = 0; end_ = t0 + 400
    for i2_ in md.disasm(rd(t0, 400), t0):
        n_ += 1
        if i2_.mnemonic in BAD_ or i2_.mnemonic == 'int3': return False
        if i2_.mnemonic in ('ret', 'jmp'): return n_ >= 2 or i2_.mnemonic == 'ret'
    return False
for (n_, v_, vs_, raw_, rs_) in code:
    blob_ = rd(v_, rs_)
    for i_ in range(1, len(blob_)):
        t_ = v_ + i_
        if blob_[i_ - 1] == 0xCC and blob_[i_] != 0xCC and t_ not in funcs and (t_ in seen or ((t_ not in seen) and decodes_to_return_(t_))): pad_new.append(t_)
for t_ in pad_new: funcs.add(t_); work.append(t_)
print('padding-boundary function entries: +%d' % len(pad_new))
descend()
print('\nreached instructions: %d in %d functions; undecodable stops: %d' % (insns, len(funcs), bad))
print('indirect calls %d, indirect jumps %d (jump-table candidates %d), returns %d' % (ind_calls, ind_jmps, len(jt_cands), rets))
tot = sum(len(rd(v, rs)) for (_, v, vs, raw, rs) in code)
print('code bytes %d, covered by recursive descent: ~%d%% (rest = data, padding, indirect-only targets)' % (tot, 0 if not tot else 100 * sum(1 for _ in seen) * 4 // tot))
print('\ninstruction groups:', dict(groups.most_common(14)))
cls = lambda m: ('x87' if m.startswith('f') and m not in ('fs',) else 'sse/mmx' if (m.endswith(('ps', 'ss', 'pd', 'sd')) or m.startswith(('p', 'mov', 'cvt')) and any(c in m for c in ('xmm',))) else None)
fp = {m: c for m, c in mn.items() if m.startswith('f')}
print('x87 mnemonics: %d distinct, %d uses: %s' % (len(fp), sum(fp.values()), dict(sorted(fp.items(), key=lambda x: -x[1])[:25])))
sse = {m: c for m, c in mn.items() if m.endswith(('ps', 'ss', 'pd', 'sd')) or m in ('pxor', 'movq', 'movd', 'emms', 'paddd', 'psubd', 'pand', 'por', 'pmullw', 'pmaddwd', 'packuswb', 'punpcklbw', 'movaps', 'movups', 'cvtsi2ss', 'cvttss2si', 'movmskps', 'shufps', 'unpcklps', 'unpckhps', 'andps', 'xorps', 'andnps', 'orps')}
print('sse/mmx mnemonics: %d distinct, %d uses: %s' % (len(sse), sum(sse.values()), dict(sorted(sse.items(), key=lambda x: -x[1])[:25])))
print('top 40 overall:', dict(mn.most_common(40)))
rare = [m for m, c in mn.items() if c <= 3]
print('rare mnemonics (<=3 uses): %d: %s' % (len(rare), ' '.join(sorted(rare))))
priv = {m: mn[m] for m in ('in', 'out', 'cli', 'sti', 'rdtsc', 'rdmsr', 'wrmsr', 'cpuid', 'lgdt', 'lidt', 'mov', 'int', 'sysenter', 'wbinvd', 'invlpg') if m in ('in', 'out', 'cli', 'sti', 'rdtsc', 'rdmsr', 'wrmsr', 'cpuid', 'lgdt', 'lidt', 'int', 'sysenter', 'wbinvd', 'invlpg') and m in mn}
print('privileged/system:', priv)
os.makedirs('work/lift', exist_ok=True)
open('work/lift/funcs.txt', 'w').write('\n'.join('%08x' % f for f in sorted(funcs)))
print('wrote work/lift/funcs.txt (%d functions)' % len(funcs))
