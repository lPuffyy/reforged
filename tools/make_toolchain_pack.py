"""Builds the toolchain pack the companion downloads on first use: just the parts of the official LLVM release needed to build the iOS game module
(clang, lld, the compiler's own headers) plus the symbol scanner, with their licenses, as one zip.
usage: python -I tools/make_toolchain_pack.py --llvm-archive clang+llvm-X-<host>.tar.xz --llvm-version 18 --xbsym-cli XbSymbolDatabaseCLI[.exe]
                                            --llvm-license-note NOTE --xbsym-license LICENSE --host windows-x64 --out toolchain-windows-x64.zip"""
import argparse, json, os, posixpath, tarfile, time, zipfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--llvm-archive', required=True); ap.add_argument('--llvm-version', required=True, help='major version, e.g. 18')
    ap.add_argument('--llvm-license', required=True); ap.add_argument('--xbsym-cli', required=True); ap.add_argument('--xbsym-license', required=True); ap.add_argument('--xbsym-commit', default='')
    ap.add_argument('--sse2neon-header', required=True); ap.add_argument('--sse2neon-license', required=True)
    ap.add_argument('--host', required=True); ap.add_argument('--out', required=True)
    a = ap.parse_args()
    exe = '.exe' if a.host.startswith('windows') else ''
    major = a.llvm_version
    # archive member (relative to its top directory) -> name in the pack. Linux/macOS archives name the compiler clang-<major>; Windows ships clang.exe.
    wanted = {'bin/clang-%s' % major: 'bin/clang' + exe, 'bin/clang' + exe: 'bin/clang' + exe, 'bin/lld' + exe: 'bin/lld' + exe}
    include_prefix = 'lib/clang/%s/include/' % major
    found = {}; headers = {}; seen_bin = []
    with tarfile.open(a.llvm_archive, 'r|xz') as tar:
        for m in tar:
            if m.issym() and m.name.count('/') == 2 and '/bin/' in m.name: seen_bin.append(m.name.split('/', 1)[1] + '->' + m.linkname)
            if not m.isfile(): continue
            rel = m.name.split('/', 1)[1] if '/' in m.name else m.name
            if rel.startswith('bin/') and rel.count('/') == 1: seen_bin.append(rel)
            if rel in wanted and wanted[rel] not in found: found[wanted[rel]] = (m.mode, tar.extractfile(m).read())
            elif rel.startswith(include_prefix): headers[rel] = tar.extractfile(m).read()
    need = ['bin/clang' + exe, 'bin/lld' + exe]
    missing = [n for n in need if n not in found]
    if missing or not headers: raise SystemExit('the LLVM archive did not contain: %s (headers found: %d); files in bin/: %s' % (missing, len(headers), ' '.join(sorted(seen_bin))[:1500]))
    manifest = {'llvm': a.llvm_version, 'host': a.host, 'xbsymbol_database_commit': a.xbsym_commit, 'built': time.strftime('%Y-%m-%d')}
    with zipfile.ZipFile(a.out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        def add(name, data, mode=0o644):
            zi = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0)); zi.compress_type = zipfile.ZIP_DEFLATED; zi.external_attr = (mode & 0o777) << 16; z.writestr(zi, data)
        for name, (mode, data) in sorted(found.items()): add(name, data, 0o755 if name.startswith('bin/') else 0o644)
        for rel, data in sorted(headers.items()): add(rel, data)
        add('licenses/LLVM-LICENSE.TXT', open(a.llvm_license, 'rb').read())
        add('xbsym/XbSymbolDatabaseCLI' + exe, open(a.xbsym_cli, 'rb').read(), 0o755)
        add('licenses/XbSymbolDatabase-LICENSE', open(a.xbsym_license, 'rb').read())
        add('include/sse2neon.h', open(a.sse2neon_header, 'rb').read())
        add('licenses/sse2neon-LICENSE', open(a.sse2neon_license, 'rb').read())
        add('manifest.json', json.dumps(manifest, indent=1).encode())
    print('wrote %s (%.0f MB): %d files' % (a.out, os.path.getsize(a.out) / 1e6, len(headers) + len(found) + 5))


if __name__ == '__main__': main()
