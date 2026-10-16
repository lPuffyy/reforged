"""Builds the companion as a single self-contained program for this operating system (PyInstaller): python -I tools/freeze_companion.py [--out dist]
The program carries the tools it runs (survey, translator, module builder) and the few source files the module build needs, in the repository layout."""
import argparse, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser(); ap.add_argument('--out', default='dist'); a = ap.parse_args()
sep = ';' if os.name == 'nt' else ':'
data = []
for f in ('lift.py', 'lift_survey.py', 'build_module.py', 'gen_title_data.py', 'title_patches.json'): data.append((os.path.join(ROOT, 'tools', f), 'tools'))
for f in ('lift_module_main.cpp', 'lift_rt.h', 'guestmem.h'): data.append((os.path.join(ROOT, 'src', f), 'src'))
for d, _, files in os.walk(os.path.join(ROOT, 'src', 'freestanding')):
    for f in files: data.append((os.path.join(d, f), os.path.relpath(d, ROOT)))
cmd = [sys.executable, '-m', 'PyInstaller', '--onefile', '--noconfirm', '--clean', '--name', 'reforged-companion', '--distpath', a.out,
       '--workpath', os.path.join(a.out, 'build'), '--specpath', os.path.join(a.out, 'build'), '--paths', os.path.join(ROOT, 'tools'),
       '--hidden-import', 'xiso', '--hidden-import', 'xbe_meta', '--hidden-import', 'ed25519', '--collect-all', 'capstone']
# the bundled tools run through runpy, so PyInstaller cannot see their imports
for m in ('concurrent.futures', 'argparse', 'glob', 're', 'shutil', 'subprocess', 'struct', 'hashlib', 'json', 'zipfile', 'urllib.request', 'tarfile', 'tempfile', 'collections', 'itertools', 'time'):
    cmd += ['--hidden-import', m]
for src, dst in data: cmd += ['--add-data', src + sep + dst]
cmd.append(os.path.join(ROOT, 'tools', 'companion.py'))
print(' '.join(cmd[:8]), '...')
sys.exit(subprocess.call(cmd))
