"""Repository hygiene guard (run locally and in CI): fails if anything derived from a game or console, or private to a user, is tracked.
This project must never contain disc images, executables, lifted game code, keys, saves, captures or per-title extracted data.
usage: python -I tools/check_clean.py   (checks tracked files when inside a git checkout, else walks the tree)"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAD_EXT = {'.iso', '.xbe', '.xiso', '.bin', '.rom', '.pcap', '.pcapng', '.dylib', '.ipa', '.exe', '.obj', '.pdb', '.o', '.a', '.lib', '.map', '.xip', '.xbx', '.dll'}
BAD_DIRS = ('work/', 'hdd/', 'gen/', 'obj/', 'obj64/', 'build/', 'build64/')
BAD_NAMES = {'eeprom.bin', 'default.xbe', 'xboxdash.xbe', 'mcpx.bin', 'flash.bin'}
# Text that must not appear anywhere in tracked text files (game identity, lifted-code symbols, keys).
BAD_TEXT = [
    (re.compile(r'\bhalo\b', re.I), 'a game name'),
    (re.compile(r'\bf_00[0-9a-f]{6}\b'), 'a lifted-function symbol (generated, game-derived code)'),
]
ALLOW_TEXT_IN = {'tools/check_clean.py', 'docs/LEGAL.md'}   # these files describe the rules and may name them
SKIP_SCAN_EXT = {'.png', '.jpg', '.ico', '.gif', '.pdf'}

def tracked():
    try:
        out = subprocess.check_output(['git', 'ls-files'], cwd=ROOT, stderr=subprocess.DEVNULL).decode().split('\n')
        files = [f for f in out if f]
        if files: return files
    except Exception: pass
    files = []
    for d, ds, fs in os.walk(ROOT):
        ds[:] = [x for x in ds if x not in ('.git', 'node_modules')]
        for f in fs: files.append(os.path.relpath(os.path.join(d, f), ROOT).replace('\\', '/'))
    return files

def main():
    problems = []
    for rel in tracked():
        low = rel.lower()
        ext = os.path.splitext(low)[1]
        if ext in BAD_EXT: problems.append((rel, 'forbidden file type %s' % ext)); continue
        if os.path.basename(low) in BAD_NAMES: problems.append((rel, 'forbidden file name')); continue
        if any(low.startswith(d) or ('/' + d) in ('/' + low) for d in BAD_DIRS): problems.append((rel, 'forbidden directory')); continue
        if ext in SKIP_SCAN_EXT or rel in ALLOW_TEXT_IN: continue
        p = os.path.join(ROOT, rel)
        try:
            if os.path.getsize(p) > 4_000_000: problems.append((rel, 'file larger than 4 MB (generated or data?)')); continue
            txt = open(p, encoding='utf-8', errors='strict').read()
        except UnicodeDecodeError: problems.append((rel, 'binary file')); continue
        except OSError: continue
        for rx, what in BAD_TEXT:
            m = rx.search(txt)
            if m: problems.append((rel, 'contains %s (%r)' % (what, m.group(0))))
    if problems:
        print('check_clean: FAILED')
        for rel, why in problems: print('  %s: %s' % (rel, why))
        return 1
    print('check_clean: ok (%d files)' % len(tracked()))
    return 0

if __name__ == '__main__': sys.exit(main())
