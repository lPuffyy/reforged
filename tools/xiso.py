"""Reader for the original console's disc image format (XDVDFS). Used by the companion tool to find and extract the game's executable.
    import xiso; img = xiso.Image('game.iso'); img.files() -> [(path, sector, size)]; img.read('default.xbe') -> bytes"""
import struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
BASES = (0, 0x18300000, 0xFD90000, 0x2080000)   # plain images, and the offsets of the data partition in full-disc dumps


class Image:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.base = None
        for off in BASES:
            self.f.seek(off + 32 * SECTOR)
            if self.f.read(20) == MAGIC: self.base = off; break
        if self.base is None: raise ValueError('not a disc image in the expected format')
        self.f.seek(self.base + 32 * SECTOR + 20)
        self.root, self.root_size = struct.unpack('<II', self.f.read(8))
        self._files = None

    def _walk(self, sector, size, prefix, out):
        self.f.seek(self.base + sector * SECTOR); data = self.f.read(size)
        seen, stack = set(), [0]
        while stack:
            o = stack.pop()
            if o in seen or o + 14 > len(data): continue
            seen.add(o)
            left, right, sec, sz, attr, nl = struct.unpack_from('<HHIIBB', data, o)
            if left == 0xFFFF: continue
            name = data[o + 14:o + 14 + nl].decode('latin1')
            if left: stack.append(left * 4)
            if right: stack.append(right * 4)
            if attr & 0x10: self._walk(sec, sz, prefix + name + '/', out)
            else: out.append((prefix + name, sec, sz))

    def files(self):
        if self._files is None:
            out = []; self._walk(self.root, self.root_size, '', out); self._files = sorted(out)
        return self._files

    def find(self, name):
        for p, sec, sz in self.files():
            if p.lower() == name.lower(): return p, sec, sz
        return None

    def read(self, name):
        e = self.find(name)
        if e is None: raise KeyError(name)
        _, sec, sz = e
        self.f.seek(self.base + sec * SECTOR)
        return self.f.read(sz)

    def close(self): self.f.close()
