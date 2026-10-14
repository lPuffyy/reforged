"""usage: python -I tools/dis.py xbe addr [len] [back] -- disassemble guest code at VA (starting `back` bytes before; may desync, pick a function start)."""
import struct, sys
from capstone import *
d = open(sys.argv[1], 'rb').read(); va = int(sys.argv[2], 16); ln = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x60; back = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
base, = struct.unpack_from('<I', d, 0x104); n, = struct.unpack_from('<I', d, 0x11C); sa, = struct.unpack_from('<I', d, 0x120)
def off(a):
    for i in range(n):
        fl, v, vs, raw, rs, na = struct.unpack_from('<IIIIII', d, sa - base + i * 56)
        if v <= a < v + rs: return raw + a - v
a = va - back; o = off(a)
for i in Cs(CS_ARCH_X86, CS_MODE_32).disasm(d[o:o + ln + back], a): print(hex(i.address), i.mnemonic, i.op_str)
