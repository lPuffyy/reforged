"""Generates tests/lift_test.xbe: a small original test program (hand assembled x86) in the original console's executable format, plus the output it must
produce. It exercises what the translator has to get right: loops, stack frames and calls, string instructions, 32-bit div/idiv, jump tables, indirect
calls, x87 and SSE math, flag handling (adc/rol/bswap/setcc), locked operations and 16-bit moves. Output goes through the kernel's DbgPrint.
usage: python make_test_xbe.py <out.xbe> <expected.txt>"""
import struct, sys

BASE = 0x10000
TEXT, DATA = 0x11000, 0x12000
DBGPRINT_THUNK = DATA            # kernel thunk table: [DbgPrint] [0]
F1, F2, F3, OUT_, N144, LOCKV, W16 = DATA + 0x10, DATA + 0x14, DATA + 0x18, DATA + 0x1C, DATA + 0x20, DATA + 0x24, DATA + 0x28
SRC, DST, DST2, JTAB = DATA + 0x30, DATA + 0x50, DATA + 0x70, DATA + 0xA0
STR0 = DATA + 0x100


class Asm:
    def __init__(self, base): self.base, self.code, self.labels, self.fix = base, bytearray(), {}, []
    def pc(self): return self.base + len(self.code)
    def e(self, *bs):
        for b in bs: self.code += (bytes(b) if isinstance(b, (bytes, bytearray, list)) else bytes([b]))
    def label(self, n): self.labels[n] = self.pc()
    def i32(self, v): self.code += struct.pack('<I', v & 0xFFFFFFFF)
    def abs(self, target):                       # 32-bit absolute address (number or label)
        if isinstance(target, str): self.fix.append((len(self.code), target, 'abs')); self.code += b'\0\0\0\0'
        else: self.i32(target)
    def rel8(self, op, label): self.e(op); self.fix.append((len(self.code), label, 'rel8')); self.e(0)
    def call(self, label): self.e(0xE8); self.fix.append((len(self.code), label, 'rel32')); self.i32(0)
    def resolve(self):
        for pos, name, kind in self.fix:
            t = self.labels[name]
            if kind == 'abs': self.code[pos:pos + 4] = struct.pack('<I', t)
            elif kind == 'rel32': self.code[pos:pos + 4] = struct.pack('<i', t - (self.base + pos + 4))
            else:
                d = t - (self.base + pos + 1); assert -128 <= d <= 127, (name, d); self.code[pos] = d & 0xFF


strings, expected = [], []
def string(s):
    addr = STR0 + sum(len(x) for x in strings); strings.append(s.encode() + b'\0'); return addr

a = Asm(TEXT)
def mov_imm(reg, v): a.e(0xB8 + reg); a.i32(v)            # reg: 0 eax 1 ecx 2 edx 3 ebx 6 esi 7 edi
def dbg(fmt, nargs=0):                                    # DbgPrint(fmt, ...): args were pushed (last first) by the caller
    a.e(0x68); a.abs(string(fmt)); a.e(0xFF, 0x15); a.i32(DBGPRINT_THUNK); a.e(0x83, 0xC4, 4 * (nargs + 1))
def push_eax(): a.e(0x50)
def push_ebx(): a.e(0x53)
def push_ecx(): a.e(0x51)
def push_edx(): a.e(0x52)
def push_esi(): a.e(0x56)
def push_edi(): a.e(0x57)

# ---- main ----
a.e(0x55, 0x89, 0xE5)                                       # push ebp; mov ebp, esp

# 1. fibonacci loop
a.e(0x31, 0xC0); mov_imm(3, 1); mov_imm(1, 20)              # xor eax,eax; mov ebx,1; mov ecx,20
a.label('fib'); a.e(0x89, 0xC2, 0x01, 0xDA, 0x89, 0xD8, 0x89, 0xD3, 0x49); a.rel8(0x75, 'fib')   # mov edx,eax; add edx,ebx; mov eax,ebx; mov ebx,edx; dec ecx; jnz
push_eax(); dbg('fib=%d', 1)
x, y = 0, 1
for _ in range(20): x, y = y, x + y
expected.append('fib=%d' % x)

# 2. rolling hash through a function with a stack frame
data = bytes((i * 37 + 11) & 0xFF for i in range(16))
a.e(0x31, 0xDB); mov_imm(6, SRC); mov_imm(7, 16)            # xor ebx,ebx; mov esi,SRC; mov edi,16
a.label('hash'); a.e(0x0F, 0xB6, 0x06, 0x50, 0x53); a.call('mix'); a.e(0x83, 0xC4, 0x08, 0x89, 0xC3, 0x46, 0x4F); a.rel8(0x75, 'hash')
push_ebx(); dbg('hash=%08x', 1)
def mix(h, b): return (((h * 31 + b) & 0xFFFFFFFF) ^ (h >> 3)) & 0xFFFFFFFF
h = 0
for b in data: h = mix(h, b)
expected.append('hash=%08x' % h)

# 3. rep movsd then a sum
mov_imm(6, SRC); mov_imm(7, DST); mov_imm(1, 4); a.e(0xFC, 0xF3, 0xA5)
a.e(0x31, 0xC0); mov_imm(6, DST); mov_imm(1, 4)
a.label('sum'); a.e(0x03, 0x06, 0x83, 0xC6, 0x04, 0x49); a.rel8(0x75, 'sum')
push_eax(); dbg('copy=%u', 1)
expected.append('copy=%u' % (sum(struct.unpack('<4I', data[:16])) & 0xFFFFFFFF))

# 4. div / idiv
mov_imm(0, 1000003); a.e(0x31, 0xD2); mov_imm(1, 97); a.e(0xF7, 0xF1, 0x89, 0xC3, 0x89, 0xD6)       # div ecx; mov ebx,eax; mov esi,edx
mov_imm(0, (-1000003) & 0xFFFFFFFF); a.e(0x99); mov_imm(1, 97); a.e(0xF7, 0xF9, 0x89, 0xC7)           # cdq; idiv ecx; mov edi,eax
push_edx(); push_edi(); push_esi(); push_ebx(); dbg('div=%d rem=%d sdiv=%d srem=%d', 4)
q, r = divmod(1000003, 97)
sq = -(1000003 // 97); sr = -(1000003 % 97)
expected.append('div=%d rem=%d sdiv=%d srem=%d' % (q, r, sq, sr))

# 5. jump table
mov_imm(0, 2); a.e(0x83, 0xF8, 0x03); a.rel8(0x77, 'sw_def'); a.e(0xFF, 0x24, 0x85); a.abs(JTAB)
for i, v in enumerate((100, 200, 300, 400)):
    a.label('sw%d' % i); mov_imm(3, v); a.rel8(0xEB, 'sw_end')
a.label('sw_def'); mov_imm(3, 999)
a.label('sw_end'); push_ebx(); dbg('switch=%d', 1)
expected.append('switch=300')

# 6. indirect call
a.e(0xB8); a.abs('mix'); a.e(0x6A, 5, 0x6A, 7, 0xFF, 0xD0, 0x83, 0xC4, 8); push_eax(); dbg('icall=%d', 1)
expected.append('icall=%d' % mix(7, 5))

# 7. x87 and SSE
a.e(0xD9, 0x05); a.i32(F1); a.e(0xD8, 0x0D); a.i32(F2); a.e(0xD8, 0x05); a.i32(F3); a.e(0xDB, 0x1D); a.i32(OUT_)       # fld f1; fmul f2; fadd f3; fistp out
a.e(0x8B, 0x1D); a.i32(OUT_)                                                                                     # mov ebx,[out]
a.e(0xF3, 0x0F, 0x10, 0x05); a.i32(F1); a.e(0xF3, 0x0F, 0x59, 0x05); a.i32(F2); a.e(0xF3, 0x0F, 0x58, 0x05); a.i32(F3); a.e(0xF3, 0x0F, 0x2C, 0xC0)   # movss; mulss; addss; cvttss2si eax,xmm0
push_eax(); push_ebx(); dbg('x87=%d sse=%d', 2)
expected.append('x87=14 sse=14')                      # 3.5*4+0.5 = 14.5: fistp rounds to even, cvttss2si truncates
a.e(0xDB, 0x05); a.i32(N144); a.e(0xD9, 0xFA, 0xDB, 0x1D); a.i32(OUT_); a.e(0xA1); a.i32(OUT_)                    # fild n; fsqrt; fistp out; mov eax,[out]
push_eax(); dbg('sqrt=%d', 1)
expected.append('sqrt=12')

# 8. flags: adc, bswap, rol, setcc
mov_imm(0, 0xFFFFFFFF); a.e(0x83, 0xC0, 0x01); mov_imm(3, 5); a.e(0x83, 0xD3, 0x03)                                # add eax,1 (sets CF); mov ebx,5; adc ebx,3
mov_imm(1, 0x11223344); a.e(0x0F, 0xC9); mov_imm(2, 0x80000001); a.e(0xC1, 0xC2, 0x04)                             # bswap ecx; rol edx,4
mov_imm(6, 3); a.e(0x31, 0xC0, 0x83, 0xFE, 0x05, 0x0F, 0x9C, 0xC0)                                                  # xor eax,eax; cmp esi,5; setl al
push_eax(); push_edx(); push_ecx(); push_ebx(); dbg('flags=%d bswap=%08x rol=%08x setl=%d', 4)
expected.append('flags=9 bswap=44332211 rol=00000018 setl=1')

# 9. locked operations
a.e(0xC7, 0x05); a.i32(LOCKV); a.i32(10)                                              # mov dword [lock], 10
a.e(0xF0, 0xFF, 0x05); a.i32(LOCKV)                                                   # lock inc dword [lock]            -> 11
mov_imm(0, 5); a.e(0xF0, 0x0F, 0xC1, 0x05); a.i32(LOCKV); a.e(0x89, 0xC3)             # lock xadd [lock], eax            -> mem 16, eax 11; mov ebx,eax
mov_imm(0, 7); a.e(0x87, 0x05); a.i32(LOCKV); a.e(0x89, 0xC1)                         # xchg [lock], eax                 -> mem 7, eax 16; mov ecx,eax
a.e(0x8B, 0x15); a.i32(LOCKV)                                                         # mov edx,[lock]
push_edx(); push_ecx(); push_ebx(); dbg('xadd_old=%d xchg_old=%d final=%d', 3)
expected.append('xadd_old=11 xchg_old=16 final=7')

# 10. rep stosd and 16-bit moves
mov_imm(7, DST2); mov_imm(0, 0xDEADBEEF); mov_imm(1, 8); a.e(0xFC, 0xF3, 0xAB); a.e(0xA1); a.i32(DST2 + 28)            # rep stosd; mov eax,[dst2+28]
push_eax(); dbg('stos=%08x', 1)
expected.append('stos=deadbeef')
a.e(0x66, 0xC7, 0x05); a.i32(W16); a.e(0xFE, 0xFF); a.e(0x0F, 0xBF, 0x05); a.i32(W16); a.e(0x0F, 0xB7, 0x1D); a.i32(W16)   # mov word [w],0xFFFE; movsx eax,[w]; movzx ebx,[w]
push_ebx(); push_eax(); dbg('w=%d %d', 2)
expected.append('w=-2 65534')

dbg('TEST DONE')
expected.append('TEST DONE')
a.e(0x5D, 0xC3)                                                                      # pop ebp; ret

# ---- mix(x, y) = ((x * 31 + y) ^ (x >> 3)); cdecl ----
a.label('mix')
a.e(0x55, 0x89, 0xE5, 0x8B, 0x45, 0x08, 0x6B, 0xC0, 0x1F, 0x03, 0x45, 0x0C, 0x8B, 0x4D, 0x08, 0xC1, 0xE9, 0x03, 0x31, 0xC8, 0x5D, 0xC3)
a.resolve()

# ---- .data ----
dat = bytearray(0x1000)
struct.pack_into('<II', dat, 0, 0x80000000 | 8, 0)                                   # kernel thunk table: DbgPrint
struct.pack_into('<fff', dat, 0x10, 3.5, 4.0, 0.5); struct.pack_into('<I', dat, 0x20, 144)
dat[0x30:0x40] = data
for i in range(4): struct.pack_into('<I', dat, 0xA0 + 4 * i, a.labels['sw%d' % i])
off = 0x100
for s in strings: dat[off:off + len(s)] = s; off += len(s)
assert off < 0x1000

# ---- XBE image ----
text = bytes(a.code); assert len(text) <= 0x1000
HDR = 0x1000
img = bytearray(HDR + 0x1000 + 0x1000)                                               # header page, .text page, .data page
img[0:4] = b'XBEH'
CERT, SECHDR, NAMES = 0x1D0, 0x200, 0x300
struct.pack_into('<I', img, 0x104, BASE); struct.pack_into('<I', img, 0x108, HDR); struct.pack_into('<I', img, 0x10C, 0x3000)
struct.pack_into('<I', img, 0x118, BASE + CERT); struct.pack_into('<I', img, 0x11C, 2); struct.pack_into('<I', img, 0x120, BASE + SECHDR)
struct.pack_into('<I', img, 0x128, TEXT ^ 0xA8FC57AB); struct.pack_into('<I', img, 0x12C, 0); struct.pack_into('<I', img, 0x130, 0x10000)
struct.pack_into('<I', img, 0x158, DATA ^ 0x5B6D40B6)
struct.pack_into('<I', img, CERT + 0, 0xE8); struct.pack_into('<I', img, CERT + 8, 0x52450001)       # certificate: size, title id
img[CERT + 0x0C:CERT + 0x0C + 16] = 'Lift test'.encode('utf-16le')
img[NAMES:NAMES + 6] = b'.text\0'; img[NAMES + 8:NAMES + 14] = b'.data\0'
struct.pack_into('<IIIIII', img, SECHDR, 0x06, TEXT, 0x1000, HDR, 0x1000, BASE + NAMES)
struct.pack_into('<IIIIII', img, SECHDR + 56, 0x03, DATA, 0x1000, HDR + 0x1000, 0x1000, BASE + NAMES + 8)
img[HDR:HDR + len(text)] = text
img[HDR + 0x1000:HDR + 0x2000] = dat
open(sys.argv[1], 'wb').write(img)
open(sys.argv[2], 'w').write('\n'.join(expected) + '\n')
print('wrote %s (%d bytes code), %d expected lines' % (sys.argv[1], len(text), len(expected)))
