"""Writes a minimal disc image in the original console's format (XDVDFS) containing one file named default.xbe, for testing the companion tool without any
real disc. usage: python make_test_iso.py <in.xbe> <out.iso>"""
import struct, sys

S = 2048
xbe = open(sys.argv[1], 'rb').read()
name = b'default.xbe'
root_sector, file_sector = 33, 34
entry = struct.pack('<HHIIBB', 0, 0, file_sector, len(xbe), 0x20, len(name)) + name       # a single directory entry: no left / right children
entry += b'\xff' * ((4 - len(entry) % 4) % 4)
root = entry + b'\xff' * (S - len(entry))
vol = bytearray(S)
vol[0:20] = b'MICROSOFT*XBOX*MEDIA'; struct.pack_into('<II', vol, 20, root_sector, S); vol[S - 20:S] = b'MICROSOFT*XBOX*MEDIA'
data = xbe + b'\0' * ((S - len(xbe) % S) % S)
img = bytearray(32 * S) + vol + root + data
open(sys.argv[2], 'wb').write(img)
print('wrote %s (%d bytes)' % (sys.argv[2], len(img)))
