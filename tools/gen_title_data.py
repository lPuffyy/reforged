"""Builds the per-title data the runtime needs from the title's own executable and a symbol scan of it:
dsound_hle.txt: "<hex address> <name> <ret imm>" for every audio and controller entry point the title calls (the runtime replaces them with its own
implementations). The symbol scan is the output of XbSymbolDatabaseCLI (lines "Name = 0xADDRESS").
usage: python -I tools/gen_title_data.py --xbe default.xbe --xbsym xbsym.txt --out titledir"""
import argparse, os, re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ap = argparse.ArgumentParser()
ap.add_argument('--xbe', required=True); ap.add_argument('--xbsym', required=True); ap.add_argument('--out', required=True)
args = ap.parse_args()

d = open(args.xbe, 'rb').read()
base, = struct.unpack_from('<I', d, 0x104); n, = struct.unpack_from('<I', d, 0x11C); sa, = struct.unpack_from('<I', d, 0x120)
secs = []
for i in range(n):
    fl, va, vs, raw, rs, na = struct.unpack_from('<IIIIII', d, sa - base + i * 56); secs.append((va, vs, raw, rs))
def fo(v):
    for va, vs, raw, rs in secs:
        if va <= v < va + rs: return raw + v - va

want = re.compile(r'^(DSOUND__(IDirectSound\w*|DirectSound\w*|XAudio\w*|CDirectSoundStream_(AddRef|Release|GetInfo|GetStatus__r2|Process|Discontinuity|Flush))|XAPILIB__(XInput\w+|XGetDeviceChanges|XGetDevices))$')
md = Cs(CS_ARCH_X86, CS_MODE_32); out = []
for line in open(args.xbsym, encoding='utf-8', errors='ignore'):
    m = re.match(r'\W*(\w+) = 0x([0-9a-fA-F]+)', line.strip())
    if not m or not want.match(m.group(1)): continue
    a = int(m.group(2), 16); ret = None; cur = a
    for _hop in range(4):   # follow direct jumps: the wrappers are often a single 'jmp CDirectSound...'
        o = fo(cur); nxt = None
        if o is None: break
        for ins in md.disasm(d[o:o + 300], cur):
            if ins.mnemonic == 'ret': ret = int(ins.op_str, 0) if ins.op_str else 0; break
            if ins.mnemonic == 'jmp':
                if ins.op_str.startswith('0x'): nxt = int(ins.op_str, 16)
                break
        if ret is not None or nxt is None: break
        cur = nxt
    out.append((a, m.group(1).split('__', 1)[1], ret if ret is not None else -1))
os.makedirs(args.out, exist_ok=True)
with open(os.path.join(args.out, 'dsound_hle.txt'), 'w') as f:
    for a, nme, r in sorted(out): f.write('%x %s %d\n' % (a, nme, r))
print('%d entry points; %d without a visible ret' % (len(out), sum(1 for _, _, r in out if r < 0)))
