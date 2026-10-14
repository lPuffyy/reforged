"""Static x86 (32-bit, Xbox/P3) -> C lifter. usage: python -I tools/lift.py <default.xbe> [--limit N] [--only addr,addr] [--out work/lift/gen]
Reads work/lift/funcs.txt (from tools/lift_survey.py), lifts every function whose instructions are all supported to `void f_XXXXXXXX(LiftCpu* c)`
(see src/lift_rt.h) and writes chunked .cpp files plus lift_table.cpp (address -> function). Functions with an unsupported instruction are left out:
the runtime runs those natively. Writes work/lift/report.txt with the unsupported mnemonic census."""
import struct, sys, os, collections, argparse
from capstone import *
from capstone.x86 import *

ap = argparse.ArgumentParser(); ap.add_argument('xbe'); ap.add_argument('--limit', type=int, default=0); ap.add_argument('--only', default='')
ap.add_argument('--out', default='work/lift/gen'); ap.add_argument('--chunk', type=int, default=400); ap.add_argument('--funcs', default='work/lift/funcs.txt'); ap.add_argument('--patches', default='work/title/patches.txt'); ap.add_argument('--count', action='store_true')
args = ap.parse_args()
d = open(args.xbe, 'rb').read()
base, = struct.unpack_from('<I', d, 0x104); nsec, = struct.unpack_from('<I', d, 0x11C); sa, = struct.unpack_from('<I', d, 0x120)
secs = []
for i in range(nsec):
    fl, v, vs, raw, rs, na = struct.unpack_from('<IIIIII', d, sa - base + i * 56); secs.append((fl, v, vs, raw, rs))
d = bytearray(d)
if args.patches and os.path.exists(args.patches):   # the host's byte patches (hardware-wait shortcuts) are part of the image we lift
    for ln in open(args.patches):
        ln = ln.split('#')[0].split()
        if len(ln) == 2:
            va = int(ln[0], 16)
            for (fl, v, vs, raw, rs) in secs:
                if v <= va < v + rs:
                    b = bytes.fromhex(ln[1]); d[raw + va - v: raw + va - v + len(b)] = b; print('patched %x (%d bytes)' % (va, len(b))); break
d = bytes(d)
def sec_of(a):
    for (fl, v, vs, raw, rs) in secs:
        if v <= a < v + rs: return (fl, v, vs, raw, rs)
def is_code(a): s = sec_of(a); return bool(s and (s[0] & 4))
def rd(a, n):
    s = sec_of(a)
    if not s: return b''
    fl, v, vs, raw, rs = s; return d[raw + a - v: raw + a - v + min(n, v + rs - a)]
def rd32(a):
    b = rd(a, 4); return struct.unpack('<I', b)[0] if len(b) == 4 else None

md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
CT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}; ST_ = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}; MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
class Unsupported(Exception): pass

def regname(r): return md.reg_name(r)
def reg_parts(n):   # -> (kind, base32, size)
    if n in R32: return ('r', n, 4)
    if n in ('ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'): return ('r', 'e' + n, 2)
    if n in ('al', 'cl', 'dl', 'bl'): return ('r', 'e' + n[0] + 'x', 1)
    if n in ('ah', 'ch', 'dh', 'bh'): return ('h', 'e' + n[0] + 'x', 1)
    raise Unsupported('reg ' + n)

def ea(op):
    m = op.mem; parts = []
    for r_ in (m.base, m.index):
        if r_ and regname(r_) in ('bx', 'si', 'di', 'bp', 'sp', 'cx', 'dx', 'ax'): raise Unsupported('16-bit address')
    if m.base: parts.append(regname(m.base))
    if m.index: parts.append('%s*%d' % (regname(m.index), m.scale) if m.scale != 1 else regname(m.index))
    if m.disp or not parts: parts.append('0x%xu' % (m.disp & 0xFFFFFFFF))
    if m.segment == X86_REG_FS: parts.append('c->fsbase')
    elif m.segment not in (0, X86_REG_DS, X86_REG_ES, X86_REG_SS, X86_REG_CS): raise Unsupported('segment')
    return '(uint32_t)(' + '+'.join(parts) + ')'

def read(op, size=None):
    size = size or op.size
    if op.type == X86_OP_REG:
        k, b, s = reg_parts(regname(op.reg))
        if k == 'h': return '(uint8_t)(%s>>8)' % b
        return b if s == 4 else '(%s)%s' % (CT[s], b)
    if op.type == X86_OP_IMM: return '((%s)0x%xu)' % (CT[size], op.imm & MASK[size])
    if op.type == X86_OP_MEM: return 'RD%d(%s)' % (size * 8, ea(op))
    raise Unsupported('operand type')
def write(op, val, size=None):
    size = size or op.size
    if op.type == X86_OP_REG:
        k, b, s = reg_parts(regname(op.reg))
        if s == 4: return '%s=(uint32_t)(%s);' % (b, val)
        if k == 'h': return '%s=(%s&0xFFFF00FFu)|((uint32_t)(uint8_t)(%s)<<8);' % (b, b, val)
        m = 0xFFFFFFFF ^ MASK[s]
        return '%s=(%s&0x%xu)|(uint32_t)(%s)(%s);' % (b, b, m, CT[s], val)
    if op.type == X86_OP_MEM: return 'WR%d(%s,%s);' % (size * 8, ea(op), val)
    raise Unsupported('write operand')

COND = {'e': 'zf', 'z': 'zf', 'ne': '!zf', 'nz': '!zf', 'b': 'cf', 'c': 'cf', 'nae': 'cf', 'ae': '!cf', 'nb': '!cf', 'nc': '!cf', 'be': '(cf|zf)', 'na': '(cf|zf)',
        'a': '(!cf&&!zf)', 'nbe': '(!cf&&!zf)', 'l': '(sf!=of)', 'nge': '(sf!=of)', 'ge': '(sf==of)', 'nl': '(sf==of)', 'le': '(zf||sf!=of)', 'ng': '(zf||sf!=of)',
        'g': '(!zf&&sf==of)', 'nle': '(!zf&&sf==of)', 's': 'sf', 'ns': '!sf', 'o': 'of', 'no': '!of', 'p': 'pf', 'pe': 'pf', 'np': '!pf', 'po': '!pf'}

class Func:
    def __init__(self, entry):
        self.entry = entry; self.ins = {}; self.targets = set(); self.tables = {}; self.out = []
def walk(entry):
    f = Func(entry); stack = [entry]
    while stack:
        a = stack.pop()
        while a not in f.ins:
            if not is_code(a): raise Unsupported('flow into non-code %x' % a)
            i = next(md.disasm(rd(a, 16), a, 1), None)
            if i is None: raise Unsupported('undecodable %x' % a)
            f.ins[a] = i; nxt = a + i.size
            if i.id == X86_INS_RET: break
            if i.id == X86_INS_JMP:
                op = i.operands[0]
                if op.type == X86_OP_IMM:
                    f.targets.add(op.imm)
                    if is_code(op.imm) and abs(op.imm - entry) < 0x4000: stack.append(op.imm)   # near jumps stay in the function; far ones are tail calls
                elif op.type == X86_OP_MEM and not op.mem.base and op.mem.index and op.mem.disp:   # jmp [table + idx*4]
                    tbl = op.mem.disp & 0xFFFFFFFF; ents = []; k = 0
                    while k < 512:
                        t = rd32(tbl + 4 * k); k += 1
                        if k == 1 and (t is None or t == 0 or not is_code(t) or abs(t - entry) > 0x4000): continue   # an unused first slot (index 0 is handled before the jump; it may even overlap code)
                        if t is None or not is_code(t) or abs(t - entry) > 0x4000: break
                        ents.append(t)
                    kb = 1   # tables are sometimes indexed with a negative register: also take the valid entries just before the displacement
                    while kb <= 128:
                        t = rd32(tbl - 4 * kb)
                        if t is None or not is_code(t) or abs(t - entry) > 0x4000: break
                        ents.append(t); kb += 1
                    f.tables[a] = ents
                    for t in ents: f.targets.add(t); stack.append(t)
                break
            if X86_GRP_JUMP in i.groups or i.mnemonic.startswith('loop'):
                op = i.operands[0]
                if op.type != X86_OP_IMM: raise Unsupported('indirect jcc')
                f.targets.add(op.imm)
                if abs(op.imm - entry) < 0x4000: stack.append(op.imm)
            if i.id in (X86_INS_HLT, X86_INS_INT3, X86_INS_UD2): break
            a = nxt
    return f

# ---------------------------------------------------------------- instruction emitters
def flagwrap(T, expr_lines): return expr_lines

def emit(f, a, i):
    mn = i.mnemonic; ops = i.operands; L = []; E = L.append; nxt = a + i.size
    lock = False
    if mn.startswith('lock '): lock = True; mn = mn[5:]
    rep = None
    for p in ('repe ', 'repne ', 'rep '):
        if mn.startswith(p): rep = p.strip(); mn = mn[len(p):]
    n = len(ops); sz = ops[0].size if n else 4
    T = CT.get(sz)
    def dst_rmw(expr_fn):   # read-modify-write helper: expr_fn(a_) returns (result_expr, flag_stmt); handles memory dst with a single address evaluation
        pass
    # ---- data movement
    if mn == 'mov' and n == 2:
        if ops[0].type == X86_OP_REG and regname(ops[0].reg) in ('cs', 'ds', 'es', 'fs', 'gs', 'ss') or ops[1].type == X86_OP_REG and regname(ops[1].reg) in ('cs', 'ds', 'es', 'fs', 'gs', 'ss'): raise Unsupported('seg mov')
        if ops[0].type == X86_OP_REG and regname(ops[0].reg).startswith(('cr', 'dr')): raise Unsupported('cr mov')
        E(write(ops[0], read(ops[1], sz), sz)); return L
    if mn in ('movzx', 'movsx') and n == 2:
        s = ops[1].size
        v = read(ops[1], s); v = '(uint32_t)(%s)' % v if mn == 'movzx' else '(uint32_t)(int32_t)(%s)(%s)' % (ST_[s], v)
        E(write(ops[0], v if ops[0].size == 4 else '(%s)%s' % (CT[ops[0].size], v), ops[0].size)); return L
    if mn == 'lea': E(write(ops[0], ea(ops[1]), 4)); return L
    if mn == 'xchg':
        if ops[0].type == X86_OP_MEM or ops[1].type == X86_OP_MEM:
            m, r = (ops[0], ops[1]) if ops[0].type == X86_OP_MEM else (ops[1], ops[0])
            if sz != 4: raise Unsupported('xchg mem')
            E('{ uint32_t ea_=%s; uint32_t t_=%s; %s LiftXchg32(ea_, &t_); %s }' % (ea(m), read(r, 4), '', write(r, 't_', 4))); return L
        E('{ %s t_=%s; %s %s }' % (T, read(ops[0]), write(ops[0], read(ops[1])), write(ops[1], 't_'))); return L
    if mn == 'push':
        v = read(ops[0], 4) if ops[0].type != X86_OP_IMM else '0x%xu' % (ops[0].imm & 0xFFFFFFFF)
        if ops[0].type == X86_OP_REG and ops[0].size != 4: raise Unsupported('push16')
        E('{ uint32_t v_=%s; esp-=4; WR32(esp,v_); }' % v); return L
    if mn == 'pop':
        E('{ uint32_t v_=RD32(esp); esp+=4; %s }' % write(ops[0], 'v_', 4)); return L
    if mn == 'leave': E('esp=ebp; ebp=RD32(esp); esp+=4;'); return L
    if mn == 'pushal' or mn == 'pusha': E('{ uint32_t o_=esp; esp-=4;WR32(esp,eax); esp-=4;WR32(esp,ecx); esp-=4;WR32(esp,edx); esp-=4;WR32(esp,ebx); esp-=4;WR32(esp,o_); esp-=4;WR32(esp,ebp); esp-=4;WR32(esp,esi); esp-=4;WR32(esp,edi); }'); return L
    if mn == 'popal' or mn == 'popa': E('edi=RD32(esp);esp+=4; esi=RD32(esp);esp+=4; ebp=RD32(esp);esp+=4; esp+=4; ebx=RD32(esp);esp+=4; edx=RD32(esp);esp+=4; ecx=RD32(esp);esp+=4; eax=RD32(esp);esp+=4;'); return L
    if mn == 'cdq': E('edx=((int32_t)eax<0)?0xFFFFFFFFu:0;'); return L
    if mn == 'cwde': E('eax=(uint32_t)(int32_t)(int16_t)eax;'); return L
    if mn == 'cwd': E('edx=(edx&0xFFFF0000u)|(((int16_t)eax<0)?0xFFFFu:0);'); return L
    if mn == 'cbw': E('eax=(eax&0xFFFF0000u)|(uint16_t)(int16_t)(int8_t)eax;'); return L
    if mn == 'nop' or mn == 'fnop' or mn == 'wait' or mn == 'fwait' or mn == 'pause': return L
    if mn in ('sfence', 'mfence', 'lfence', 'prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'prefetchw', 'prefetch'): return L
    if mn == 'bswap': E(write(ops[0], '__builtin_bswap32(%s)' % read(ops[0], 4), 4)); return L
    if mn in ('cli', 'sti', 'hlt', 'wbinvd', 'invd', 'lfence', 'clflush'): return L
    if mn == 'pushfd': E('{ uint32_t v_=cf|2|(pf<<2)|(af<<4)|(zf<<6)|(sf<<7)|(df<<10)|(of<<11)|0x200u|xf; esp-=4; WR32(esp,v_); }'); return L
    if mn == 'popfd': E('{ uint32_t v_=RD32(esp); esp+=4; cf=v_&1; pf=(v_>>2)&1; af=(v_>>4)&1; zf=(v_>>6)&1; sf=(v_>>7)&1; df=(v_>>10)&1; of=(v_>>11)&1; xf=v_&0x240000u; }'); return L   # AC and ID stick (CPUID detection toggles ID)
    if mn == 'in' and n == 2:
        port = ('%du' % (ops[1].imm & 0xFF)) if ops[1].type == X86_OP_IMM else '(edx&0xFFFF)'
        E(write(ops[0], 'PortIn(%s,%d)' % (port, ops[0].size), ops[0].size)); return L
    if mn == 'out' and n == 2:
        port = ('%du' % (ops[0].imm & 0xFF)) if ops[0].type == X86_OP_IMM else '(edx&0xFFFF)'
        E('PortOut(%s,%s,%d);' % (port, read(ops[1], ops[1].size), ops[1].size)); return L
    if mn in ('rcl', 'rcr') and n == 2:
        sz = ops[0].size; T = CT[sz]; B = sz * 8; mem = ops[0].type == X86_OP_MEM
        pre = '{ '
        if mem: pre += 'uint32_t ea_=%s; ' % ea(ops[0]); A = 'RD%d(ea_)' % B; W = lambda v: 'WR%d(ea_,%s);' % (B, v)
        else: A = read(ops[0]); W = lambda v: write(ops[0], v)
        cnt = ('%du' % (ops[1].imm & 0xFF)) if ops[1].type == X86_OP_IMM else '(ecx&0xFF)'
        if mn == 'rcr': step = 'uint32_t nc_=a_&1; a_=(%s)((a_>>1)|((%s)cf<<%d)); cf=nc_;' % (T, T, B - 1)
        else: step = 'uint32_t nc_=(a_>>%d)&1; a_=(%s)((a_<<1)|cf); cf=nc_;' % (B - 1, T)
        E(pre + '%s a_=%s; uint32_t n_=(%s&31)%s; for(uint32_t i_=0;i_<n_;i_++){ %s } %s }' % (T, A, cnt, ('%%%d' % (B + 1)) if B < 32 else '', step, W('a_'))); return L
    if mn == 'imul' and n == 1 and ops[0].size == 1:
        E('{ int32_t p_=(int32_t)(int8_t)eax*(int32_t)(int8_t)%s; eax=(eax&0xFFFF0000u)|(uint16_t)p_; cf=of=(p_!=(int32_t)(int8_t)p_); }' % read(ops[0], 1)); return L
    if mn == 'sahf': E('{ uint32_t ah_=(eax>>8)&0xFF; cf=ah_&1; pf=(ah_>>2)&1; af=(ah_>>4)&1; zf=(ah_>>6)&1; sf=(ah_>>7)&1; }'); return L
    if mn == 'lahf': E('eax=(eax&0xFFFF00FFu)|((cf|2|(pf<<2)|(af<<4)|(zf<<6)|(sf<<7))<<8);'); return L
    if mn == 'stc': E('cf=1;'); return L
    if mn == 'clc': E('cf=0;'); return L
    if mn == 'cmc': E('cf^=1;'); return L
    # ---- ALU
    if mn in ('add', 'adc', 'sub', 'sbb', 'cmp', 'and', 'or', 'xor', 'test') and n == 2:
        sz = ops[0].size; T = CT[sz]; mem = ops[0].type == X86_OP_MEM
        if mem and lock and mn in ('add', 'sub', 'and', 'or', 'xor') and sz == 4:
            opn = ['add', 'sub', 'and', 'or', 'xor'].index(mn)
            fl = ['uint64_t t_=(uint64_t)a_+b_; uint32_t r_=(uint32_t)t_; F_ADD(uint32_t,a_,b_,r_,t_);', 'uint32_t r_=a_-b_; F_SUB(uint32_t,a_,b_,r_,0);', 'uint32_t r_=a_&b_; F_LOGIC(uint32_t,r_);', 'uint32_t r_=a_|b_; F_LOGIC(uint32_t,r_);', 'uint32_t r_=a_^b_; F_LOGIC(uint32_t,r_);'][opn]
            E('{ uint32_t b_=%s; uint32_t a_=LiftLock32(%d, %s, b_); %s }' % (read(ops[1], 4), opn, ea(ops[0]), fl)); return L   # the atomic returns the old value; the flags follow from it
        pre = '{ '
        if mem: pre += 'uint32_t ea_=%s; ' % ea(ops[0]); A = 'RD%d(ea_)' % (sz * 8); W = lambda v: 'WR%d(ea_,%s);' % (sz * 8, v)
        else: A = read(ops[0]); W = lambda v: write(ops[0], v)
        pre += '%s a_=%s; %s b_=%s; ' % (T, A, T, read(ops[1], sz))
        if mn == 'add': E(pre + 'uint64_t t_=(uint64_t)a_+b_; %s r_=(%s)t_; %s F_ADD(%s,a_,b_,r_,t_); }' % (T, T, W('r_'), T))
        elif mn == 'adc': E(pre + 'uint64_t t_=(uint64_t)a_+b_+cf; %s r_=(%s)t_; %s F_ADD(%s,a_,b_,r_,t_); }' % (T, T, W('r_'), T))
        elif mn == 'sub': E(pre + '%s r_=a_-b_; %s F_SUB(%s,a_,b_,r_,0); }' % (T, W('r_'), T))
        elif mn == 'cmp': E(pre + '%s r_=a_-b_; F_SUB(%s,a_,b_,r_,0); }' % (T, T))
        elif mn == 'sbb': E(pre + 'uint32_t cin_=cf; %s r_=a_-b_-cin_; %s F_SUB(%s,a_,b_,r_,cin_); }' % (T, W('r_'), T))
        elif mn == 'and': E(pre + '%s r_=a_&b_; %s F_LOGIC(%s,r_); }' % (T, W('r_'), T))
        elif mn == 'or': E(pre + '%s r_=a_|b_; %s F_LOGIC(%s,r_); }' % (T, W('r_'), T))
        elif mn == 'xor': E(pre + '%s r_=a_^b_; %s F_LOGIC(%s,r_); }' % (T, W('r_'), T))
        elif mn == 'test': E(pre + '%s r_=a_&b_; F_LOGIC(%s,r_); }' % (T, T))
        return L
    if mn in ('inc', 'dec', 'neg', 'not') and n == 1:
        sz = ops[0].size; T = CT[sz]; mem = ops[0].type == X86_OP_MEM
        if mem and lock and sz == 4 and mn in ('inc', 'dec'):
            E('{ uint32_t a_=LiftLock32(%d, %s, 1); uint32_t r_=a_%s1; %s }' % (0 if mn == 'inc' else 1, ea(ops[0]), '+' if mn == 'inc' else '-', 'F_INC(uint32_t,a_,r_);' if mn == 'inc' else 'F_DEC(uint32_t,a_,r_);')); return L
        pre = '{ '
        if mem: pre += 'uint32_t ea_=%s; ' % ea(ops[0]); A = 'RD%d(ea_)' % (sz * 8); W = lambda v: 'WR%d(ea_,%s);' % (sz * 8, v)
        else: A = read(ops[0]); W = lambda v: write(ops[0], v)
        pre += '%s a_=%s; ' % (T, A)
        if mn == 'inc': E(pre + '%s r_=a_+1; %s F_INC(%s,a_,r_); }' % (T, W('r_'), T))
        elif mn == 'dec': E(pre + '%s r_=a_-1; %s F_DEC(%s,a_,r_); }' % (T, W('r_'), T))
        elif mn == 'neg': E(pre + '%s r_=(%s)(0-a_); %s F_SUB(%s,(%s)0,a_,r_,0); }' % (T, T, W('r_'), T, T))
        else: E(pre + '%s r_=~a_; %s }' % (T, W('r_')))
        return L
    if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd') and n >= 2:
        sz = ops[0].size; T = CT[sz]; B = sz * 8; mem = ops[0].type == X86_OP_MEM
        pre = '{ '
        if mem: pre += 'uint32_t ea_=%s; ' % ea(ops[0]); A = 'RD%d(ea_)' % (sz * 8); W = lambda v: 'WR%d(ea_,%s);' % (sz * 8, v)
        else: A = read(ops[0]); W = lambda v: write(ops[0], v)
        cnt = '(uint32_t)%s' % read(ops[-1] if mn in ('shld', 'shrd') else ops[1], 1) if (ops[-1].type != X86_OP_REG or regname(ops[-1].reg) == 'cl') else None
        if ops[-1].type == X86_OP_REG: cnt = '(ecx&0xFF)'
        elif ops[-1].type == X86_OP_IMM: cnt = '%du' % (ops[-1].imm & 0xFF)
        pre += '%s a_=%s; uint32_t n_=%s&31; ' % (T, A, cnt)
        if mn in ('shl', 'sal'): body = 'if(n_){ %s r_=(%s)(a_<<n_); %s cf=(uint32_t)((a_>>(%d-n_))&1); of=((uint32_t)(r_>>%d)&1)^cf; af=0; SETZSP(%s,r_); }' % (T, T, W('r_'), B, B - 1, T) if B == 32 else 'if(n_){ %s r_=(%s)(((uint32_t)a_)<<n_); %s cf=(uint32_t)((((uint32_t)a_)>>(%d-n_))&1); of=((uint32_t)(r_>>%d)&1)^cf; af=0; SETZSP(%s,r_); }' % (T, T, W('r_'), B, B - 1, T)
        elif mn == 'shr': body = 'if(n_){ %s r_=(%s)(a_>>n_); %s cf=(uint32_t)((a_>>(n_-1))&1); of=(uint32_t)(a_>>%d)&1; af=0; SETZSP(%s,r_); }' % (T, T, W('r_'), B - 1, T)
        elif mn == 'sar': body = 'if(n_){ %s r_=(%s)((%s)a_>>(n_>%d?%d:n_)); %s cf=(uint32_t)(((%s)a_>>(n_-1>%d?%d:n_-1))&1); of=0; af=0; SETZSP(%s,r_); }' % (T, T, ST_[sz], B - 1, B - 1, W('r_'), ST_[sz], B - 1, B - 1, T)
        elif mn == 'rol': body = 'if(n_){ uint32_t m_=n_&%d; %s r_=m_?(%s)((a_<<m_)|(a_>>(%d-m_))):a_; %s cf=r_&1; of=((uint32_t)(r_>>%d)&1)^cf; }' % (B - 1, T, T, B, W('r_'), B - 1)
        elif mn == 'ror': body = 'if(n_){ uint32_t m_=n_&%d; %s r_=m_?(%s)((a_>>m_)|(a_<<(%d-m_))):a_; %s cf=(uint32_t)(r_>>%d)&1; of=((uint32_t)(r_>>%d)^(uint32_t)(r_>>%d))&1; }' % (B - 1, T, T, B, W('r_'), B - 1, B - 1, B - 2)
        elif mn == 'shld':
            body = 'if(n_){ %s s_=(%s)%s; %s r_=(%s)((a_<<n_)|(s_>>(%d-n_))); %s cf=(uint32_t)((a_>>(%d-n_))&1); of=((uint32_t)(r_>>%d)&1)^((uint32_t)(a_>>%d)&1); SETZSP(%s,r_); }' % (T, T, read(ops[1]), T, T, B, W('r_'), B, B - 1, B - 1, T)
        elif mn == 'shrd':
            body = 'if(n_){ %s s_=(%s)%s; %s r_=(%s)((a_>>n_)|(s_<<(%d-n_))); %s cf=(uint32_t)((a_>>(n_-1))&1); of=((uint32_t)(r_>>%d)&1)^((uint32_t)(a_>>%d)&1); SETZSP(%s,r_); }' % (T, T, read(ops[1]), T, T, B, W('r_'), B - 1, B - 1, T)
        else: raise Unsupported(mn)
        E(pre + body + ' }'); return L
    if mn in ('mul', 'imul', 'div', 'idiv') and n == 1:
        sz = ops[0].size
        if sz == 4:
            s = read(ops[0], 4)
            if mn == 'mul': E('{ uint64_t p_=(uint64_t)eax*%s; eax=(uint32_t)p_; edx=(uint32_t)(p_>>32); cf=of=(edx!=0); }' % s)
            elif mn == 'imul': E('{ int64_t p_=(int64_t)(int32_t)eax*(int32_t)%s; eax=(uint32_t)p_; edx=(uint32_t)((uint64_t)p_>>32); cf=of=(p_!=(int64_t)(int32_t)eax); }' % s)
            elif mn == 'div': E('{ uint32_t s_=%s; uint64_t n_=((uint64_t)edx<<32)|eax; if(!s_) LiftUnsupported(0x%x,"div0"); else { eax=(uint32_t)(n_/s_); edx=(uint32_t)(n_%%s_); } }' % (s, a))
            else: E('{ int32_t s_=(int32_t)%s; int64_t n_=(int64_t)(((uint64_t)edx<<32)|eax); if(!s_) LiftUnsupported(0x%x,"idiv0"); else { eax=(uint32_t)(int32_t)(n_/s_); edx=(uint32_t)(int32_t)(n_%%s_); } }' % (s, a))
            return L
        if sz == 2 and mn == 'imul': raise Unsupported('imul16 1op')
        if sz == 1 and mn in ('mul', 'div'):
            s = read(ops[0], 1)
            if mn == 'mul': E('{ uint32_t p_=(eax&0xFF)*(uint32_t)%s; eax=(eax&0xFFFF0000u)|p_; cf=of=((p_>>8)!=0); }' % s)
            else: E('{ uint32_t s_=%s; uint32_t n_=eax&0xFFFF; if(!s_) LiftUnsupported(0x%x,"div0"); else eax=(eax&0xFFFF0000u)|((n_%%s_)<<8)|((n_/s_)&0xFF); }' % (s, a))
            return L
        raise Unsupported('%s%d' % (mn, sz * 8))
    if mn == 'imul' and n >= 2:
        sz = ops[0].size
        if sz == 2:
            s1, s2 = (read(ops[1], 2), read(ops[2], 2)) if n == 3 else (read(ops[0], 2), read(ops[1], 2))
            E('{ int32_t p_=(int32_t)(int16_t)%s*(int16_t)%s; %s cf=of=(p_!=(int32_t)(int16_t)p_); }' % (s1, s2, write(ops[0], '(uint16_t)p_', 2))); return L
        if sz != 4: raise Unsupported('imul8')
        s1, s2 = (read(ops[1], 4), read(ops[2], 4)) if n == 3 else (read(ops[0], 4), read(ops[1], 4))
        E('{ int64_t p_=(int64_t)(int32_t)%s*(int32_t)%s; %s cf=of=(p_!=(int64_t)(int32_t)p_); }' % (s1, s2, write(ops[0], '(uint32_t)p_', 4))); return L
    if mn in ('bt', 'bts', 'btr', 'btc') and n == 2 and ops[0].type == X86_OP_MEM and ops[0].size == 4:
        if ops[1].type == X86_OP_IMM: bit = '0'; off = '%du' % (ops[1].imm & 31); eaw = ea(ops[0])
        else: off = '((uint32_t)%s&31)' % read(ops[1], 4); eaw = '(uint32_t)(%s+((int32_t)%s>>5)*4)' % (ea(ops[0]), read(ops[1], 4))
        newv = {'bt': None, 'bts': 'a_|(1u<<b_)', 'btr': 'a_&~(1u<<b_)', 'btc': 'a_^(1u<<b_)'}[mn]
        E('{ uint32_t ea_=%s; uint32_t b_=%s; uint32_t a_=RD32(ea_); cf=(a_>>b_)&1; %s }' % (eaw, off, ('WR32(ea_,%s);' % newv) if newv else '')); return L
    if mn in ('bt', 'bts', 'btr', 'btc') and n == 2 and ops[0].type == X86_OP_REG and ops[0].size == 4:
        b = '(%s&31)' % read(ops[1], 4)
        E('{ uint32_t a_=%s; uint32_t b_=%s; cf=(a_>>b_)&1; %s }' % (read(ops[0], 4), b, {'bt': '', 'bts': write(ops[0], 'a_|(1u<<b_)', 4), 'btr': write(ops[0], 'a_&~(1u<<b_)', 4), 'btc': write(ops[0], 'a_^(1u<<b_)', 4)}[mn])); return L
    if mn in ('bsf', 'bsr') and ops[0].size == 4:
        E('{ uint32_t s_=%s; zf=(s_==0); if(s_){ %s } }' % (read(ops[1], 4), write(ops[0], '__builtin_ctz(s_)' if mn == 'bsf' else '(31-__builtin_clz(s_))', 4))); return L
    if mn.startswith('set') and n == 1:
        c = mn[3:]; E(write(ops[0], '(uint8_t)(%s)' % COND[c], 1)); return L
    if mn.startswith('cmov') and n == 2:
        c = mn[4:]; E('if(%s){ %s }' % (COND[c], write(ops[0], read(ops[1], ops[0].size), ops[0].size))); return L
    if mn in ('cmpxchg', 'xadd') and ops[0].type == X86_OP_MEM and ops[0].size == 4:
        if mn == 'xadd': E('{ uint32_t o_=LiftXadd32(%s,%s); %s }' % (ea(ops[0]), read(ops[1], 4), write(ops[1], 'o_', 4)))
        else: E('{ uint32_t a_=eax; uint32_t o_=LiftCmpxchg32(%s,&a_,%s); zf=(o_==eax); eax=o_; }' % (ea(ops[0]), read(ops[1], 4)))
        return L
    # ---- string ops
    if mn in ('movsd', 'movsb', 'movsw', 'stosd', 'stosb', 'stosw', 'cmpsd', 'cmpsb', 'cmpsw', 'scasb', 'scasw', 'scasd', 'lodsb', 'lodsw', 'lodsd') and n == 0 or (mn in ('movsd', 'movsb', 'movsw', 'stosd', 'stosb', 'stosw', 'cmpsd', 'cmpsb', 'cmpsw', 'scasb', 'scasw', 'scasd', 'lodsb', 'lodsw', 'lodsd') and n == 2 and ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_MEM) or (mn in ('stosd', 'stosb', 'stosw', 'scasb', 'scasw', 'scasd', 'lodsb', 'lodsw', 'lodsd') and n >= 1):
        w = 4 if mn.endswith('d') else 2 if mn.endswith('w') else 1; B = w * 8
        kind = mn[:4]
        body = {'movs': 'WR%d(edi,RD%d(esi)); esi+=(df?-%d:%d); edi+=(df?-%d:%d);' % (B, B, w, w, w, w), 'stos': 'WR%d(edi,(%s)eax); edi+=(df?-%d:%d);' % (B, CT[w], w, w),
                'lods': 'eax=(eax&~0x%xu)|(%s)RD%d(esi); esi+=(df?-%d:%d);' % (MASK[w], CT[w], B, w, w),
                'cmps': '{ %s x_=RD%d(esi),y_=RD%d(edi); %s r_=x_-y_; F_SUB(%s,x_,y_,r_,0); esi+=(df?-%d:%d); edi+=(df?-%d:%d); }' % (CT[w], B, B, CT[w], CT[w], w, w, w, w),
                'scas': '{ %s x_=(%s)eax,y_=RD%d(edi); %s r_=x_-y_; F_SUB(%s,x_,y_,r_,0); edi+=(df?-%d:%d); }' % (CT[w], CT[w], B, CT[w], CT[w], w, w)}[kind]
        if rep is None: E(body)
        elif rep == 'rep' and kind in ('movs', 'stos', 'lods'):
            if kind == 'movs' and w == 4: E('if(df){ while(ecx){ WR32(edi,RD32(esi)); esi-=4; edi-=4; ecx--; } } else { uint32_t n_=ecx; for(uint32_t i_=0;i_<n_;i_++) WR32(edi+i_*4,RD32(esi+i_*4)); esi+=n_*4; edi+=n_*4; ecx=0; } ')   # forward copy: overlapping regions replicate, unlike memmove
            elif kind == 'stos' and w == 4: E('if(df){ while(ecx){ WR32(edi,eax); edi-=4; ecx--; } } else { uint32_t n_=ecx; for(uint32_t i_=0;i_<n_;i_++) WR32(edi+i_*4,eax); edi+=n_*4; ecx=0; }')
            elif kind == 'stos' and w == 1: E('if(df){ while(ecx){ WR8(edi,eax); edi--; ecx--; } } else { uint32_t n_=ecx; if(n_){ memset(GP(edi),(int)(eax&0xFF),n_); edi+=n_; ecx=0; } }')
            else: E('while(ecx){ %s ecx--; }' % body)
        elif rep in ('repe', 'repne') and kind in ('cmps', 'scas'): E('while(ecx){ %s ecx--; if(%s) break; }' % (body, '!zf' if rep == 'repe' else 'zf'))
        else: raise Unsupported(rep + ' ' + mn)
        return L
    if mn == 'std': E('df=1;'); return L
    if mn == 'cld': E('df=0;'); return L
    # ---- control flow
    if mn == 'jmp':
        op = ops[0]
        if op.type == X86_OP_IMM:
            t = op.imm
            if t in f.ins: E('goto L_%x;' % t)
            else: E('FL_PACK(); c->esp=esp; WBACK; LiftDispatch(c,0x%xu); return;' % t)
        elif a in f.tables:
            E('switch(%s){' % ('RD32(%s)' % ea(op)))
            for t in sorted(set(f.tables[a])): E(' case 0x%xu: goto L_%x;' % (t, t))
            E(' default: FL_PACK(); WBACK; c->esp=esp; LiftDispatch(c,RD32(%s)); return; }' % ea(op))
        else: E('{ uint32_t t_=%s; FL_PACK(); WBACK; c->esp=esp; LiftDispatch(c,t_); return; }' % read(op, 4))
        return L
    if X86_GRP_JUMP in i.groups and mn.startswith('j') and mn != 'jmp':
        c = mn[1:]; t = ops[0].imm
        if mn in ('jecxz', 'jcxz'): cond = '(ecx==0)'
        else: cond = COND[c]
        if t in f.ins: E('if(%s) goto L_%x;' % (cond, t))
        else: E('if(%s){ FL_PACK(); WBACK; c->esp=esp; LiftDispatch(c,0x%xu); return; }' % (cond, t))
        return L
    if mn in ('loop', 'loope', 'loopne'):
        t = ops[0].imm
        if t not in f.ins: raise Unsupported('loop out')
        E('ecx--; if(ecx%s) goto L_%x;' % ({'loop': '', 'loope': '&&zf', 'loopne': '&&!zf'}[mn] if mn == 'loop' else '!=0' + {'loope': '&&zf', 'loopne': '&&!zf'}[mn], t)); return L
    if mn == 'call':
        op = ops[0]
        if op.type == X86_OP_IMM: E('esp-=4; WR32(esp,0x%xu); WBACK; c->esp=esp;' % nxt); E('LiftDispatch(c,0x%xu);' % op.imm)
        else: E('{ uint32_t t_=%s; esp-=4; WR32(esp,0x%xu); WBACK; c->esp=esp; LiftDispatch(c,t_); }' % (read(op, 4), nxt))   # the target is evaluated before the push (it may be [esp+n])
        E('RELOAD;'); return L
    if mn == 'ret':
        E('FL_PACK(); esp+=%d; c->esp=esp; WBACK; return;' % (4 + (ops[0].imm if n else 0))); return L
    if mn == 'int3' or mn == 'ud2' or mn == 'hlt': E('LiftUnsupported(0x%x,"%s");' % (a, mn)); return L
    if mn == 'rdtsc': E('{ uint64_t t_=LiftRdtsc(); eax=(uint32_t)t_; edx=(uint32_t)(t_>>32); }'); return L
    if mn == 'cpuid': E('{ uint32_t l_=eax; if(l_==0){ eax=2; ebx=0x756e6547u; edx=0x49656e69u; ecx=0x6c65746eu; } else if(l_==1){ eax=0x686u; ebx=0; ecx=0; edx=0x0383FBFFu; } else { eax=ebx=ecx=edx=0; } }'); return L
    if mn == 'xlatb': E('eax=(eax&0xFFFFFF00u)|RD8(ebx+(eax&0xFF));'); return L
    return emit_fp(f, a, i, mn, ops, n, sz, L)

# ---------------------------------------------------------------- x87 / SSE
def fmem(op):   # float load expression from a memory operand
    return {4: 'RDF32(%s)', 8: 'RDF64(%s)'}[op.size] % ea(op)
def emit_fp(f, a, i, mn, ops, n, sz, L):
    E = L.append
    def sti(op): return 'ST(%d)' % (op.reg - X86_REG_ST0)
    def isst(op): return op.type == X86_OP_REG and X86_REG_ST0 <= op.reg <= X86_REG_ST7
    if mn == 'fld':
        if isst(ops[0]): E('{ double v_=%s; x87_push(c,v_); }' % sti(ops[0]))
        elif ops[0].size in (4, 8): E('x87_push(c,%s);' % fmem(ops[0]))
        else: E('x87_push(c,x87_ld80(%s));' % ea(ops[0]))
        return L
    if mn in ('fst', 'fstp'):
        if isst(ops[0]): E('ST(%d)=ST(0);' % (ops[0].reg - X86_REG_ST0) if mn == 'fst' else '{ double v_=ST(0); ST(%d)=v_; }' % (ops[0].reg - X86_REG_ST0))
        elif ops[0].size == 4: E('WRF32(%s,(float)ST(0));' % ea(ops[0]))
        elif ops[0].size == 8: E('WRF64(%s,ST(0));' % ea(ops[0]))
        else: E('x87_st80(%s,ST(0));' % ea(ops[0]))
        if mn == 'fstp': E('x87_pop(c);')
        return L
    if mn == 'fild': E('x87_push(c,(double)(%s)RD%d(%s));' % ({2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}[ops[0].size], ops[0].size * 8, ea(ops[0])) if ops[0].size != 8 else 'x87_push(c,(double)(int64_t)(((uint64_t)RD32(%s+4)<<32)|RD32(%s)));' % (ea(ops[0]), ea(ops[0]))); return L
    if mn in ('fist', 'fistp', 'fisttp'):
        r = 'x87_round(c,ST(0))' if mn != 'fisttp' else '(int64_t)trunc(ST(0))'
        if ops[0].size == 2: E('WR16(%s,(uint16_t)(int16_t)%s);' % (ea(ops[0]), r))
        elif ops[0].size == 4: E('{ int64_t v_=%s; WR32(%s,(v_>2147483647LL||v_<-2147483648LL)?0x80000000u:(uint32_t)(int32_t)v_); }' % (r, ea(ops[0])))
        elif ops[0].size == 8: E('{ uint32_t a_=%s; int64_t v_=%s; WR32(a_,(uint32_t)v_); WR32(a_+4,(uint32_t)((uint64_t)v_>>32)); }' % (ea(ops[0]), r))
        else: raise Unsupported('fist')
        if mn != 'fist': E('x87_pop(c);')
        return L
    if mn in ('fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr', 'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fiadd', 'fisub', 'fisubr', 'fimul', 'fidiv', 'fidivr'):
        base = mn.rstrip('p') if mn.endswith('p') and mn not in ('fadd',) and not mn.startswith('fi') else mn
        pop = mn.endswith('p') and mn not in ('fadd',)
        if mn == 'faddp': base, pop = 'fadd', True
        if mn.startswith('fi'): base = 'f' + mn[2:]
        opch = {'fadd': '+', 'fsub': '-', 'fsubr': '-', 'fmul': '*', 'fdiv': '/', 'fdivr': '/'}[base]; rev = base in ('fsubr', 'fdivr')
        if n == 0:   # implicit st(1), st(0): faddp etc.
            dst, a_, b_ = 'ST(1)', 'ST(1)', 'ST(0)'
        elif n == 1 and pop and isst(ops[0]):   # faddp st(i): st(i) = st(i) op st(0), then pop
            dst, a_, b_ = sti(ops[0]), sti(ops[0]), 'ST(0)'
        elif n == 1:
            dst, a_ = 'ST(0)', 'ST(0)'
            if mn.startswith('fi'): b_ = '(double)(%s)RD%d(%s)' % ({2: 'int16_t', 4: 'int32_t'}[ops[0].size], ops[0].size * 8, ea(ops[0]))
            elif isst(ops[0]): b_ = sti(ops[0])
            else: b_ = fmem(ops[0])
        else:
            if isst(ops[0]) and ops[0].reg == X86_REG_ST0: dst, a_, b_ = 'ST(0)', 'ST(0)', sti(ops[1])
            else: dst, a_, b_ = sti(ops[0]), sti(ops[0]), 'ST(0)'
        if rev: a_, b_ = b_, a_
        E('{ double a_=%s, b_=%s; %s=x87_pc(c,a_%sb_); }' % (a_, b_, dst, opch))
        if pop: E('x87_pop(c);')
        return L
    if mn == 'fchs': E('ST(0)=-ST(0);'); return L
    if mn == 'fabs': E('ST(0)=fabs(ST(0));'); return L
    if mn == 'fsqrt': E('ST(0)=x87_pc(c,sqrt(ST(0)));'); return L
    if mn == 'fsin': E('ST(0)=sin(ST(0)); c->fsw&=~0x400;'); return L
    if mn == 'fcos': E('ST(0)=cos(ST(0)); c->fsw&=~0x400;'); return L
    if mn == 'fptan': E('ST(0)=tan(ST(0)); x87_push(c,1.0);'); return L
    if mn == 'fpatan': E('{ double v_=atan2(ST(1),ST(0)); x87_pop(c); ST(0)=v_; }'); return L
    if mn == 'frndint': E('ST(0)=(double)x87_round(c,ST(0));'); return L
    if mn == 'fscale': E('ST(0)=ldexp(ST(0),(int)trunc(ST(1)));'); return L
    if mn == 'f2xm1': E('ST(0)=pow(2.0,ST(0))-1.0;'); return L
    if mn == 'fyl2x': E('{ double v_=ST(1)*log2(ST(0)); x87_pop(c); ST(0)=v_; }'); return L
    if mn == 'fprem': E('{ double q_=trunc(ST(0)/ST(1)); ST(0)=ST(0)-q_*ST(1); c->fsw&=~0x4700; }'); return L
    if mn == 'fld1': E('x87_push(c,1.0);'); return L
    if mn == 'fldz': E('x87_push(c,0.0);'); return L
    if mn == 'fsincos': E('{ double v_=ST(0); ST(0)=sin(v_); x87_push(c,cos(v_)); c->fsw&=~0x400; }'); return L
    if mn == 'fnsave' or mn == 'fsave': E('{ uint32_t a_=%s; WR32(a_,c->fcw); WR32(a_+4,(c->fsw&~0x3800)|((uint32_t)c->top<<11)); WR32(a_+8,0xFFFF); for(int i_=0;i_<8;i_++) x87_st80(a_+28+i_*10,ST(i_)); c->fcw=0x37F; c->fsw=0; c->top=0; }' % ea(ops[0])); return L
    if mn == 'frstor': E('{ uint32_t a_=%s; c->fcw=RD16(a_); c->fsw=RD16(a_+4); c->top=(c->fsw>>11)&7; for(int i_=0;i_<8;i_++) ST(i_)=x87_ld80(a_+28+i_*10); }' % ea(ops[0])); return L
    if mn == 'fldlg2': E('x87_push(c,0.30102999566398119521);'); return L
    if mn == 'fldl2e': E('x87_push(c,1.44269504088896340736);'); return L
    if mn == 'fldln2': E('x87_push(c,0.69314718055994530942);'); return L
    if mn == 'fldl2t': E('x87_push(c,3.32192809488736234787);'); return L
    if mn == 'fldpi': E('x87_push(c,3.14159265358979323846);'); return L
    if mn == 'fxch':
        o = (ops[1] if n >= 2 else ops[0]) if n else None   # capstone lists `fxch st(1)` as (st(0), st(1))
        t = sti(o) if o is not None else 'ST(1)'
        E('{ double t_=ST(0); ST(0)=%s; %s=t_; }' % (t, t)); return L
    if mn in ('fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp', 'fucompp', 'ficom', 'ficomp'):
        if mn in ('fcompp', 'fucompp'): E('x87_cmp(c,ST(0),ST(1)); x87_pop(c); x87_pop(c);'); return L
        if n == 0: b_ = 'ST(1)'
        elif isst(ops[0]): b_ = sti(ops[0])
        elif mn.startswith('fi'): b_ = '(double)(%s)RD%d(%s)' % ({2: 'int16_t', 4: 'int32_t'}[ops[0].size], ops[0].size * 8, ea(ops[0]))
        else: b_ = fmem(ops[0])
        E('x87_cmp(c,ST(0),%s);' % b_)
        if mn.endswith('p'): E('x87_pop(c);')
        return L
    if mn in ('fcomi', 'fcomip', 'fucomi', 'fucomip', 'fcompi', 'fucompi'):
        E('{ double a_=ST(0), b_=%s; if(a_!=a_||b_!=b_){ zf=pf=cf=1; } else { zf=(a_==b_); pf=0; cf=(a_<b_); } of=sf=af=0; }' % sti(ops[-1]) if n >= 1 else '{ double a_=ST(0), b_=ST(1); if(a_!=a_||b_!=b_){ zf=pf=cf=1; } else { zf=(a_==b_); pf=0; cf=(a_<b_); } of=sf=af=0; }')
        if mn.endswith('p') or mn.endswith('pi'): E('x87_pop(c);')
        return L
    if mn == 'ftst': E('x87_cmp(c,ST(0),0.0);'); return L
    if mn == 'fxam': E('{ double v_=ST(0); uint16_t b_=signbit(v_)?0x200:0; if(v_!=v_) b_|=0x100; else if(isinf(v_)) b_|=0x500; else if(v_==0) b_|=0x4000; else b_|=0x400; c->fsw=(uint16_t)((c->fsw&~0x4700)|b_); }'); return L
    if mn in ('fnstsw', 'fstsw'):
        if ops[0].type == X86_OP_REG: E('eax=(eax&0xFFFF0000u)|(uint32_t)((c->fsw&~0x3800)|((uint32_t)c->top<<11));')
        else: E('WR16(%s,(uint16_t)((c->fsw&~0x3800)|((uint32_t)c->top<<11)));' % ea(ops[0]))
        return L
    if mn in ('fnstcw', 'fstcw'): E('WR16(%s,c->fcw);' % ea(ops[0])); return L
    if mn == 'fldcw': E('c->fcw=RD16(%s);' % ea(ops[0])); return L
    if mn in ('fnclex', 'fclex'): E('c->fsw&=0x7F00;'); return L
    if mn in ('fninit', 'finit'): E('c->fcw=0x37F; c->fsw=0; c->top=0;'); return L
    if mn == 'ffree' or mn == 'ffreep': return L if mn == 'ffree' else (L.append('x87_pop(c);') or L)
    if mn == 'fcmovbe' or mn.startswith('fcmov'): raise Unsupported(mn)
    if mn == 'stmxcsr': E('WR32(%s,0x1F80u);' % ea(ops[0])); return L
    if mn == 'ldmxcsr': return L
    return emit_sse(f, a, i, mn, ops, n, L)

XMM = {X86_REG_XMM0 + k: 'c->xmm[%d]' % k for k in range(8)}
def xr(op): return XMM[op.reg]
def emit_sse(f, a, i, mn, ops, n, L):
    E = L.append
    isx = lambda o: o.type == X86_OP_REG and o.reg in XMM
    def src_ps(o): return xr(o) if isx(o) else '_mm_loadu_ps((const float*)GP(%s))' % ea(o)
    def src_ss(o): return xr(o) if isx(o) else '_mm_load_ss((const float*)GP(%s))' % ea(o)
    if mn in ('movaps', 'movups', 'movapd', 'movdqa', 'movdqu'):
        if isx(ops[0]): E('%s=%s;' % (xr(ops[0]), src_ps(ops[1])))
        else: E('_mm_storeu_ps((float*)GP(%s),%s);' % (ea(ops[0]), xr(ops[1])))
        return L
    if mn == 'movss':
        if isx(ops[0]) and isx(ops[1]): E('%s=_mm_move_ss(%s,%s);' % (xr(ops[0]), xr(ops[0]), xr(ops[1])))
        elif isx(ops[0]): E('%s=_mm_load_ss((const float*)GP(%s));' % (xr(ops[0]), ea(ops[1])))
        else: E('_mm_store_ss((float*)GP(%s),%s);' % (ea(ops[0]), xr(ops[1])))
        return L
    if mn in ('movlps', 'movhps'):
        if isx(ops[0]): E('%s=_mm_%s_pi(%s,(const __m64*)GP(%s));' % (xr(ops[0]), 'loadl' if mn == 'movlps' else 'loadh', xr(ops[0]), ea(ops[1])) if False else '%s=_mm_%s_pi(%s,(const __m64*)GP(%s));' % (xr(ops[0]), 'loadl' if mn == 'movlps' else 'loadh', xr(ops[0]), ea(ops[1])))
        else: E('_mm_%s_pi((__m64*)GP(%s),%s);' % ('storel' if mn == 'movlps' else 'storeh', ea(ops[0]), xr(ops[1])))
        return L
    if mn == 'movhlps': E('%s=_mm_movehl_ps(%s,%s);' % (xr(ops[0]), xr(ops[0]), xr(ops[1]))); return L
    if mn == 'movlhps': E('%s=_mm_movelh_ps(%s,%s);' % (xr(ops[0]), xr(ops[0]), xr(ops[1]))); return L
    if mn == 'movmskps': E(write(ops[0], '(uint32_t)_mm_movemask_ps(%s)' % xr(ops[1]), 4)); return L
    for nm, fn in (('add', 'add'), ('sub', 'sub'), ('mul', 'mul'), ('div', 'div'), ('min', 'min'), ('max', 'max')):
        if mn == nm + 'ss': E('%s=_mm_%s_ss(%s,%s);' % (xr(ops[0]), fn, xr(ops[0]), src_ss(ops[1]))); return L
        if mn == nm + 'ps': E('%s=_mm_%s_ps(%s,%s);' % (xr(ops[0]), fn, xr(ops[0]), src_ps(ops[1]))); return L
    if mn == 'sqrtss': E('%s=_mm_sqrt_ss(%s);' % (xr(ops[0]), src_ss(ops[1])) if False else '%s=_mm_move_ss(%s,_mm_sqrt_ss(%s));' % (xr(ops[0]), xr(ops[0]), src_ss(ops[1]))); return L
    if mn == 'sqrtps': E('%s=_mm_sqrt_ps(%s);' % (xr(ops[0]), src_ps(ops[1]))); return L
    if mn == 'rcpss': E('%s=_mm_move_ss(%s,_mm_rcp_ss(%s));' % (xr(ops[0]), xr(ops[0]), src_ss(ops[1]))); return L
    if mn == 'rcpps': E('%s=_mm_rcp_ps(%s);' % (xr(ops[0]), src_ps(ops[1]))); return L
    if mn == 'rsqrtss': E('%s=_mm_move_ss(%s,_mm_rsqrt_ss(%s));' % (xr(ops[0]), xr(ops[0]), src_ss(ops[1]))); return L
    if mn == 'rsqrtps': E('%s=_mm_rsqrt_ps(%s);' % (xr(ops[0]), src_ps(ops[1]))); return L
    for nm in ('and', 'andn', 'or', 'xor'):
        if mn == nm + 'ps': E('%s=_mm_%s_ps(%s,%s);' % (xr(ops[0]), 'andnot' if nm == 'andn' else nm, xr(ops[0]), src_ps(ops[1]))); return L
    if mn == 'shufps': E('%s=_mm_shuffle_ps(%s,%s,0x%x);' % (xr(ops[0]), xr(ops[0]), src_ps(ops[1]), ops[2].imm & 0xFF)); return L
    if mn == 'unpcklps': E('%s=_mm_unpacklo_ps(%s,%s);' % (xr(ops[0]), xr(ops[0]), src_ps(ops[1]))); return L
    if mn == 'unpckhps': E('%s=_mm_unpackhi_ps(%s,%s);' % (xr(ops[0]), xr(ops[0]), src_ps(ops[1]))); return L
    if mn in ('comiss', 'ucomiss'):
        E('{ float a_=_mm_cvtss_f32(%s), b_=_mm_cvtss_f32(%s); if(a_!=a_||b_!=b_){ zf=pf=cf=1; } else { zf=(a_==b_); pf=0; cf=(a_<b_); } of=sf=af=0; }' % (xr(ops[0]), src_ss(ops[1]))); return L
    if mn == 'cvtsi2ss': E('%s=_mm_cvtsi32_ss(%s,(int32_t)%s);' % (xr(ops[0]), xr(ops[0]), read(ops[1], 4))); return L
    if mn == 'cvtss2si': E(write(ops[0], '(uint32_t)_mm_cvt_ss2si(%s)' % src_ss(ops[1]), 4)); return L
    if mn == 'cvttss2si': E(write(ops[0], '(uint32_t)_mm_cvtt_ss2si(%s)' % src_ss(ops[1]), 4)); return L
    if mn.startswith('cmp') and mn.endswith(('ss', 'ps')) and len(mn) > 5:
        pred = {'eq': 0, 'lt': 1, 'le': 2, 'unord': 3, 'neq': 4, 'nlt': 5, 'nle': 6, 'ord': 7}.get(mn[3:-2])
        if pred is not None: E('%s=_mm_cmp%s_%s(%s,%s);' % (xr(ops[0]), mn[3:-2], mn[-2:], xr(ops[0]), (src_ss if mn.endswith('ss') else src_ps)(ops[1]))); return L
    return emit_mmx(f, a, i, mn, ops, n, L)

MM = {X86_REG_MM0 + k: 'c->mm[%d]' % k for k in range(8)}
MMX2 = {'paddb': 'mx_add_pi8', 'paddw': 'mx_add_pi16', 'paddd': 'mx_add_pi32', 'paddsb': 'mx_adds_pi8', 'paddsw': 'mx_adds_pi16', 'paddusb': 'mx_adds_pu8', 'paddusw': 'mx_adds_pu16',
        'psubb': 'mx_sub_pi8', 'psubw': 'mx_sub_pi16', 'psubd': 'mx_sub_pi32', 'psubsb': 'mx_subs_pi8', 'psubsw': 'mx_subs_pi16', 'psubusb': 'mx_subs_pu8', 'psubusw': 'mx_subs_pu16',
        'pand': 'mx_and_si64', 'pandn': 'mx_andnot_si64', 'por': 'mx_or_si64', 'pxor': 'mx_xor_si64', 'pcmpeqb': 'mx_cmpeq_pi8', 'pcmpeqw': 'mx_cmpeq_pi16', 'pcmpeqd': 'mx_cmpeq_pi32',
        'pcmpgtb': 'mx_cmpgt_pi8', 'pcmpgtw': 'mx_cmpgt_pi16', 'pcmpgtd': 'mx_cmpgt_pi32', 'pmullw': 'mx_mullo_pi16', 'pmulhw': 'mx_mulhi_pi16', 'pmaddwd': 'mx_madd_pi16',
        'packsswb': 'mx_packs_pi16', 'packssdw': 'mx_packs_pi32', 'packuswb': 'mx_packs_pu16', 'punpcklbw': 'mx_unpacklo_pi8', 'punpcklwd': 'mx_unpacklo_pi16', 'punpckldq': 'mx_unpacklo_pi32',
        'punpckhbw': 'mx_unpackhi_pi8', 'punpckhwd': 'mx_unpackhi_pi16', 'punpckhdq': 'mx_unpackhi_pi32', 'pavgb': 'mx_avg_pu8', 'pavgw': 'mx_avg_pu16', 'psadbw': 'mx_sad_pu8',
        'pmaxub': 'mx_max_pu8', 'pminub': 'mx_min_pu8', 'pmaxsw': 'mx_max_pi16', 'pminsw': 'mx_min_pi16', 'pmulhuw': 'mx_mulhi_pu16'}
MMSH = {'psllw': ('mx_slli_pi16', 'mx_sll_pi16'), 'pslld': ('mx_slli_pi32', 'mx_sll_pi32'), 'psllq': ('mx_slli_si64', 'mx_sll_si64'), 'psrlw': ('mx_srli_pi16', 'mx_srl_pi16'),
        'psrld': ('mx_srli_pi32', 'mx_srl_pi32'), 'psrlq': ('mx_srli_si64', 'mx_srl_si64'), 'psraw': ('mx_srai_pi16', 'mx_sra_pi16'), 'psrad': ('mx_srai_pi32', 'mx_sra_pi32')}
def emit_mmx(f, a, i, mn, ops, n, L):
    E = L.append
    ismm = lambda o: o.type == X86_OP_REG and o.reg in MM
    def src(o): return MM[o.reg] if ismm(o) else 'LD64(%s)' % ea(o)
    if mn == 'emms': E('mx_empty();'); return L
    isx = lambda o: o.type == X86_OP_REG and o.reg in XMM
    if mn == 'cvtpi2ps' and n == 2 and isx(ops[0]): E('%s=mx_cvtpi32_ps(%s,%s);' % (XMM[ops[0].reg], XMM[ops[0].reg], src(ops[1]))); return L
    if mn in ('cvtps2pi', 'cvttps2pi') and n == 2 and ismm(ops[0]):
        sv = XMM[ops[1].reg] if isx(ops[1]) else '_mm_loadl_pi(_mm_setzero_ps(),(const __m64*)GP(%s))' % ea(ops[1])
        E('%s=%s(%s);' % (MM[ops[0].reg], 'mx_cvtps_pi32' if mn == 'cvtps2pi' else 'mx_cvttps_pi32', sv)); return L
    if mn == 'movntq' and n == 2 and ismm(ops[1]): E('ST64(%s,%s);' % (ea(ops[0]), MM[ops[1].reg])); return L
    if mn in ('movntps', 'movntdq') and n == 2 and isx(ops[1]): E('_mm_storeu_ps((float*)GP(%s),%s);' % (ea(ops[0]), XMM[ops[1].reg])); return L
    if mn == 'pmovmskb' and n == 2 and ismm(ops[1]): E(write(ops[0], '(uint32_t)mx_movemask_pi8(%s)' % MM[ops[1].reg], 4)); return L
    if mn == 'pextrw' and n == 3 and ismm(ops[1]): E(write(ops[0], '(uint32_t)mx_extract_pi16(%s,%d)' % (MM[ops[1].reg], ops[2].imm & 3), 4)); return L
    if mn == 'pinsrw' and n == 3 and ismm(ops[0]): E('%s=mx_insert_pi16(%s,(int)%s,%d);' % (MM[ops[0].reg], MM[ops[0].reg], read(ops[1], 4), ops[2].imm & 3)); return L
    if n == 2 and ismm(ops[0]):
        d = MM[ops[0].reg]
        if mn in MMX2: E('%s=%s(%s,%s);' % (d, MMX2[mn], d, src(ops[1]))); return L
        if mn == 'movq': E('%s=%s;' % (d, src(ops[1]))); return L
        if mn == 'movd':
            E('%s=mx_cvtsi32_si64((int)%s);' % (d, read(ops[1], 4))); return L
        if mn in MMSH:
            if ops[1].type == X86_OP_IMM: E('%s=%s(%s,%d);' % (d, MMSH[mn][0], d, ops[1].imm & 0xFF)); return L
            E('%s=%s(%s,%s);' % (d, MMSH[mn][1], d, src(ops[1]))); return L
    if mn == 'movq' and n == 2 and ops[0].type == X86_OP_MEM and ismm(ops[1]): E('ST64(%s,%s);' % (ea(ops[0]), MM[ops[1].reg])); return L
    if mn == 'movd' and n == 2 and ismm(ops[1]): E(write(ops[0], '(uint32_t)mx_cvtsi64_si32(%s)' % MM[ops[1].reg], 4)); return L
    if mn == 'pshufw' and n == 3 and ismm(ops[0]): E('%s=mx_shuffle_pi16(%s,%d);' % (MM[ops[0].reg], src(ops[1]), ops[2].imm & 0xFF)); return L
    raise Unsupported(mn)

# ---------------------------------------------------------------- function emission
HDR = '#include "lift_rt.h"\nextern "C" {\nuint32_t LiftXadd32(uint32_t,uint32_t); uint32_t LiftCmpxchg32(uint32_t,uint32_t*,uint32_t); void LiftXchg32(uint32_t,uint32_t*); uint32_t LiftLock32(int,uint32_t,uint32_t); uint64_t LiftRdtsc();\n}\n'
DECL = '  uint32_t eax=c->eax,ecx=c->ecx,edx=c->edx,ebx=c->ebx,esp=c->esp,ebp=c->ebp,esi=c->esi,edi=c->edi; uint32_t cf,zf,sf,of,pf,af,df=0,xf=0; FL_UNPACK(); (void)df;(void)xf; (void)cf;(void)zf;(void)sf;(void)of;(void)pf;(void)af;\n'
DEFS = '#define WBACK { c->eax=eax;c->ecx=ecx;c->edx=edx;c->ebx=ebx;c->ebp=ebp;c->esi=esi;c->edi=edi; }\n#define RELOAD { eax=c->eax;ecx=c->ecx;edx=c->edx;ebx=c->ebx;esp=c->esp;ebp=c->ebp;esi=c->esi;edi=c->edi; FL_UNPACK(); }\n'
LAST = None; HI = {}; FLAGS = {}
def testable(f):
    # a leaf with no side effects outside registers/stack/pointed-to memory: safe to run natively and lifted side by side with random inputs
    for a, i in f.ins.items():
        if i.id in (X86_INS_CALL, X86_INS_RDTSC, X86_INS_IN, X86_INS_OUT, X86_INS_CPUID): return False
        if i.id == X86_INS_RET and i.operands and i.operands[0].imm > 0x40: return False   # garbage decode (a stray seed)
        if i.bytes == bytes([0, 0]): return False   # add [eax], al: padding / data decoded as code
        if i.id == X86_INS_JMP and i.operands[0].type != X86_OP_IMM and a not in f.tables: return False
        if i.id == X86_INS_JMP and i.operands[0].type == X86_OP_IMM and i.operands[0].imm not in f.ins: return False
        if i.id != X86_INS_RET and a + i.size not in f.ins and i.id != X86_INS_JMP: return False
        for k, o in enumerate(i.operands):
            if o.type == X86_OP_REG and X86_REG_XMM0 <= o.reg <= X86_REG_XMM7: return False
            if o.type == X86_OP_MEM:
                if o.mem.segment == X86_REG_FS: return False
                if k == 0 and not o.mem.base and not o.mem.index and i.id not in (X86_INS_CMP, X86_INS_TEST, X86_INS_PUSH): return False   # absolute store/RMW: global state
    return True
def lift_func(entry):
    global LAST
    f = walk(entry); LAST = f; lines = []
    if entry != min(f.ins): lines.append('  goto L_%x;   /* the entry is not the lowest address */' % entry)
    for a in sorted(f.ins):
        i = f.ins[a]
        if a in f.targets or a == entry:
            lines.append('L_%x:;' % a)
            if args.count:
                n_ = 0; b_ = a
                while b_ in f.ins and (n_ == 0 or b_ not in f.targets):
                    n_ += 1; ii = f.ins[b_]; b_ += ii.size
                    if ii.id in (X86_INS_RET, X86_INS_JMP) or X86_GRP_JUMP in ii.groups: break
                lines.append('  LiftCount(%d);' % n_)
        body = emit(f, a, i)
        lines.extend('  ' + l for l in body)
        nxt = a + i.size
        # fallthrough into an address that is not part of this function: tail-dispatch
        if i.id not in (X86_INS_RET, X86_INS_JMP) and nxt not in f.ins and i.id not in (X86_INS_HLT, X86_INS_INT3, X86_INS_UD2):
            lines.append('  FL_PACK(); c->esp=esp; WBACK; LiftDispatch(c,0x%xu); return;' % nxt)
    return 'void f_%08x(LiftCpu* c) {\n%s%s\n}\n' % (entry, DECL, '\n'.join(lines))

funcs = [int(x, 16) for x in open(args.funcs).read().split()]
if args.only: funcs = [int(x, 16) for x in args.only.split(',')]
if args.limit: funcs = funcs[:args.limit]
os.makedirs(args.out, exist_ok=True)
for fn in os.listdir(args.out):
    if fn.endswith('.cpp'): os.remove(os.path.join(args.out, fn))
ok = []; bad = collections.Counter(); badfuncs = 0; chunk = []; nchunk = 0; body_size = 0
def flush():
    global chunk, nchunk
    if not chunk: return
    open(os.path.join(args.out, 'lift_%03d.cpp' % nchunk), 'w').write(HDR + DEFS + '\n'.join(chunk)); nchunk += 1; chunk = []
queue = list(funcs); seen_f = set(queue); qi = 0
def far_targets(f):   # code addresses this function branches to without containing them (tail jumps and far conditional jumps)
    out = set()
    for a_, i_ in f.ins.items():
        if (X86_GRP_JUMP in i_.groups or i_.mnemonic.startswith('loop')) and i_.operands and i_.operands[0].type == X86_OP_IMM:
            t_ = i_.operands[0].imm
            if t_ not in f.ins and is_code(t_): out.add(t_)
    return out
while qi < len(queue):
    e = queue[qi]; qi += 1
    try: src = lift_func(e)
    except Unsupported as ex: bad[str(ex)] += 1; badfuncs += 1; continue
    except Exception as ex: bad['crash: %s' % ex] += 1; badfuncs += 1; continue
    for t_ in far_targets(LAST):
        if t_ not in seen_f: seen_f.add(t_); queue.append(t_)
    ok.append(e); chunk.append(src); HI[e] = max(a + i.size for a, i in LAST.ins.items()); FLAGS[e] = 1 if testable(LAST) else 0
    if len(chunk) >= args.chunk: flush()
flush()
tbl = ['#include "lift_rt.h"', 'struct LiftEntry { uint32_t addr; LiftFn fn; uint32_t hi; uint32_t flags; };']
tbl += ['void f_%08x(LiftCpu*);' % e for e in ok]
tbl += ['extern "C" const LiftEntry g_liftTable[] = {'] + ['  {0x%xu, f_%08x, 0x%xu, %du},' % (e, e, HI[e], FLAGS[e]) for e in sorted(ok)] + ['  {0, 0, 0, 0}', '};', 'extern "C" const unsigned g_liftCount = %d;' % len(ok)]
open(os.path.join(args.out, 'lift_table.cpp'), 'w').write('\n'.join(tbl) + '\n')
rep = ['lifted %d of %d functions (%d skipped)' % (len(ok), len(funcs), badfuncs)] + ['%6d  %s' % (c, k) for k, c in bad.most_common(80)]
open('work/lift/report.txt', 'w').write('\n'.join(rep)); print('\n'.join(rep[:40]))
