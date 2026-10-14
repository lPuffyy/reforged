"""List the files of an original-Xbox XDVDFS disc image and print basic XBE info. usage: xiso_ls.py image.iso [extract name outfile]"""
import struct, sys
S = 2048
def find_base(f):
    for off in (0, 0x18300000, 0xFD90000, 0x2080000):
        f.seek(off + 32 * S); m = f.read(20)
        if m == b'MICROSOFT*XBOX*MEDIA': return off
    raise SystemExit('not an XDVDFS image')
def walk(f, base, sector, size, path, out):
    seen = set(); stack = [0]
    data = None
    f.seek(base + sector * S); data = f.read(size)
    while stack:
        o = stack.pop()
        if o in seen or o + 14 > len(data): continue
        seen.add(o)
        l, r, sec, sz, attr, nl = struct.unpack_from('<HHIIBB', data, o)
        if l == 0xFFFF: continue
        name = data[o + 14:o + 14 + nl].decode('latin1')
        if l: stack.append(l * 4)
        if r: stack.append(r * 4)
        if attr & 0x10: walk(f, base, sec, sz, path + name + '/', out)
        else: out.append((path + name, sec, sz))
def main():
    f = open(sys.argv[1], 'rb'); base = find_base(f)
    f.seek(base + 32 * S + 20); root, rsize = struct.unpack('<II', f.read(8))
    files = []; walk(f, base, root, rsize, '', files); files.sort()
    if len(sys.argv) >= 5 and sys.argv[2] == 'extract':
        for n, sec, sz in files:
            if n.lower() == sys.argv[3].lower():
                f.seek(base + sec * S); open(sys.argv[4], 'wb').write(f.read(sz)); print('wrote', sys.argv[4], sz); return
        raise SystemExit('not found')
    print('base offset', hex(base), 'files', len(files), 'bytes', sum(x[2] for x in files))
    for n, sec, sz in files: print(f'{sz:>12}  {n}')
main()
