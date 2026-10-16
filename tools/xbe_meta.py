"""Reads the descriptive data a program carries about itself: its certificate (title, title id, regions, version, build date) and its title image (the 128x128
icon in the $$XTIMAGE section). Everything comes from the user's own executable; nothing is downloaded.
usage: python -I tools/xbe_meta.py default.xbe [icon.png]        (prints the data; writes the icon when a path is given)
As a module: read_meta(xbe_bytes) -> dict with name, title_id, regions, version, built, icon_png (bytes or None)."""
import struct, sys, time, zlib

REGIONS = ((1, 'NA'), (2, 'JP'), (4, 'EU'))


def _sections(d):
    base, = struct.unpack_from('<I', d, 0x104); n, = struct.unpack_from('<I', d, 0x11C); sa, = struct.unpack_from('<I', d, 0x120)
    out = {}
    for i in range(n):
        fl, va, vs, raw, rs, na = struct.unpack_from('<IIIIII', d, sa - base + i * 56)
        name = d[na - base:na - base + 24].split(b'\0')[0].decode('latin1')
        out.setdefault(name, d[raw:raw + rs])
    return out


def _png(w, h, rgba):
    rows = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(tag, data): return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')


def _c565(v): return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def _dxt(data, w, h, kind):
    out = bytearray(w * h * 4); bs = 8 if kind == 1 else 16; pos = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = data[pos:pos + bs]; pos += bs
            cb = blk[bs - 8:]; c0, c1 = struct.unpack_from('<HH', cb, 0); idx, = struct.unpack_from('<I', cb, 4)
            p0, p1 = _c565(c0), _c565(c1)
            if c0 > c1 or kind != 1: pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else: pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            alpha = None
            if kind == 3: alpha = [((blk[i // 2] >> (4 * (i & 1))) & 15) * 17 for i in range(16)]
            elif kind == 5:
                a0, a1 = blk[0], blk[1]; bits = int.from_bytes(blk[2:8], 'little')
                tab = [a0, a1] + ([(( 6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
                alpha = [tab[(bits >> (3 * i)) & 7] for i in range(16)]
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x >= w or y >= h: continue
                r, g, b, a = pal[(idx >> (2 * i)) & 3]
                if alpha: a = alpha[i]
                o = (y * w + x) * 4; out[o:o + 4] = bytes((r, g, b, a))
    return bytes(out)


def _unswizzle(x, y, w, h):
    """Position of texel (x, y) in a swizzled (Morton order) texture of power-of-two size: interleave x and y bits while both have bits left, then append the rest."""
    wl = w.bit_length() - 1; hl = h.bit_length() - 1; both = min(wl, hl); k = 0; shift = 0
    for i in range(both):
        k |= ((x >> i) & 1) << shift; shift += 1
        k |= ((y >> i) & 1) << shift; shift += 1
    if wl > hl: k |= (x >> both) << shift
    elif hl > wl: k |= (y >> both) << shift
    return k


def _image(sec):
    """Decodes an XPR0 title image section into (w, h, rgba) or None."""
    if len(sec) < 32 or sec[:4] != b'XPR0': return None
    total, hsize = struct.unpack_from('<II', sec, 4)
    fmt, = struct.unpack_from('<I', sec, 12 + 12)
    cf = (fmt >> 8) & 0xFF; w = 1 << ((fmt >> 20) & 15); h = 1 << ((fmt >> 24) & 15); data = sec[hsize:]
    if w > 256 or h > 256: return None
    if cf == 0x0C: return w, h, _dxt(data, w, h, 1)
    if cf == 0x0E: return w, h, _dxt(data, w, h, 3)
    if cf == 0x0F: return w, h, _dxt(data, w, h, 5)
    if cf in (0x06, 0x07, 0x12, 0x1E):
        out = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                o = (_unswizzle(x, y, w, h) if cf in (0x06, 0x07) else y * w + x) * 4; b, g, r, a = data[o:o + 4]
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, 255 if cf in (0x07, 0x1E) else a))
        return w, h, bytes(out)
    return None


def read_meta(xbe):
    meta = {'name': None, 'title_id': None, 'regions': [], 'version': None, 'built': None, 'icon_png': None}
    if len(xbe) < 0x200 or xbe[:4] != b'XBEH': return meta
    base, = struct.unpack_from('<I', xbe, 0x104); cert, = struct.unpack_from('<I', xbe, 0x118); co = cert - base
    if 0 < co < len(xbe) - 0xB0:
        ts, tid = struct.unpack_from('<II', xbe, co + 4)
        name = xbe[co + 0xC:co + 0xC + 80].decode('utf-16le', 'replace').split('\0')[0].strip()
        reg, = struct.unpack_from('<I', xbe, co + 0xA0); ver, = struct.unpack_from('<I', xbe, co + 0xAC)
        meta.update(name=name or None, title_id='%08X' % tid, regions=[n for bit, n in REGIONS if reg & bit], version=ver, built=time.strftime('%Y-%m-%d', time.gmtime(ts)) if ts else None)
    img = _image(_sections(xbe).get('$$XTIMAGE', b''))
    if img: meta['icon_png'] = _png(*img)
    return meta


if __name__ == '__main__':
    m = read_meta(open(sys.argv[1], 'rb').read())
    print({k: v for k, v in m.items() if k != 'icon_png'}, 'icon:', len(m['icon_png']) if m['icon_png'] else None, 'bytes')
    if len(sys.argv) > 2 and m['icon_png']: open(sys.argv[2], 'wb').write(m['icon_png']); print('wrote', sys.argv[2])
