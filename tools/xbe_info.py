"""Print the header, sections and kernel imports of an original-Xbox .xbe. usage: xbe_info.py default.xbe"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'XBEH'
(base, hdrsz, imgsz, ihsz, ts, certaddr, nsec, secaddr, initflags, entry, tls, stack, pe_heap, pe_base, pe_img, pe_ts, libver, kthunk) = struct.unpack_from('<IIIIIIIIIIIIIIIIII', d, 0x104)
# entry/kernel thunk are XOR-obfuscated; retail keys (debug entry key 0x94859D4B, retail 0xA8FC57AB; thunk debug 0xEFB1F152, retail 0x5B6D40B6)
for name, ek, tk in (('retail', 0xA8FC57AB, 0x5B6D40B6), ('debug', 0x94859D4B, 0xEFB1F152)):
    e = entry ^ ek; t = kthunk ^ tk
    print(name, 'entry', hex(e), 'kernel thunk', hex(t))
print('base', hex(base), 'image', hex(imgsz), 'sections', nsec, 'initflags', hex(initflags))
c = certaddr - base
title = d[c + 0x0C:c + 0x0C + 80].decode('utf-16le').rstrip('\0')
print('title', title, 'id', hex(struct.unpack_from('<I', d, c + 8)[0]))
for i in range(nsec):
    fl, va, vs, raw, rs, name_addr = struct.unpack_from('<IIIIII', d, secaddr - base + i * 56)
    nm = d[name_addr - base:d.index(b'\0', name_addr - base)].decode()
    print(f'  {nm:<10} va={va:#010x} vsize={vs:#9x} raw={raw:#9x} rsize={rs:#9x} flags={fl:#x}')
