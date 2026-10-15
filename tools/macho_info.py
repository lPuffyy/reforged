"""Prints the essentials of a 64-bit Mach-O file (architecture, type, platform, minimum OS) with no external tools, and checks the module's entry point.
usage: python -I tools/macho_info.py module.dylib [--expect-ios] [--expect-symbol _reforged_module_init]"""
import struct, sys

CPU = {0x0100000C: 'arm64', 0x01000007: 'x86_64'}
FILETYPE = {1: 'object', 2: 'executable', 6: 'dylib', 8: 'bundle'}
PLATFORM = {1: 'macos', 2: 'ios', 3: 'tvos', 4: 'watchos', 6: 'maccatalyst', 7: 'ios-simulator', 8: 'tvos-simulator'}


def info(path):
    d = open(path, 'rb').read()
    magic, cpu, sub, ftype, ncmds, size, flags, _ = struct.unpack_from('<IIIIIIII', d, 0)
    if magic != 0xFEEDFACF: raise SystemExit('%s: not a 64-bit Mach-O file' % path)
    out = {'cpu': CPU.get(cpu, hex(cpu)), 'type': FILETYPE.get(ftype, ftype), 'platform': None, 'minos': None}
    off = 32
    for _ in range(ncmds):
        cmd, csize = struct.unpack_from('<II', d, off)
        if cmd == 0x32:   # LC_BUILD_VERSION
            plat, minos, sdk, ntools = struct.unpack_from('<IIII', d, off + 8)
            out['platform'] = PLATFORM.get(plat, plat); out['minos'] = '%d.%d' % (minos >> 16, (minos >> 8) & 0xFF)
        off += csize
    out['bytes'] = d
    return out


if __name__ == '__main__':
    path = sys.argv[1]; i = info(path); data = i.pop('bytes')
    print('%s: %s %s, platform %s, minimum OS %s' % (path, i['cpu'], i['type'], i['platform'], i['minos']))
    if '--expect-ios' in sys.argv and not (i['cpu'] == 'arm64' and i['platform'] in ('ios', 'ios-simulator')): raise SystemExit('not an arm64 iOS binary')
    if '--expect-symbol' in sys.argv:
        sym = sys.argv[sys.argv.index('--expect-symbol') + 1].encode()
        if sym not in data: raise SystemExit('symbol %s not found' % sym.decode())
        print('symbol %s present' % sym.decode())
