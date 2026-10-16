"""Reforged companion: builds a personal copy of the app for the game disc you own.

Put this program next to your disc image and run it (or drag the image onto it). It
  1. finds the game's executable on the disc image,
  2. gets the app (the latest release from the project's GitHub Releases page, checked against a signature, or a local file with --ipa),
  3. translates the game's code on this computer and builds it into a module with a plain compiler toolchain,
  4. writes the personal app next to the disc image.
Open that file in your sideloading tool (AltStore, SideStore, Sideloadly) to install it. Copy the disc image to the device (or pick it in the app).

Nothing this program produces is uploaded anywhere. The personal app contains code made from your own disc: do not share it.
"""
import argparse, glob, hashlib, io, json, os, platform, re, shutil, subprocess, sys, tempfile, time, urllib.request, zipfile

BASE = getattr(sys, '_MEIPASS', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # frozen builds unpack the repository layout (tools/, src/) here
HERE = os.path.join(BASE, 'tools')
sys.path.insert(0, HERE)
REPO = 'lPuffyy/reforged'
# Public key that release signatures are checked against (32 bytes, hex). Set when the project's release key is created; until then downloads are refused.
RELEASE_PUBLIC_KEY = '3c848690fff6afddbdf28070a351c72d6c3f9799d3eed261a3f5265f21230670'
APP_NAME = 'Reforged'


def say(step, total, text): print('[%d/%d] %s' % (step, total, text), flush=True)
def die(text): print('error: ' + text, file=sys.stderr); sys.exit(1)


def run_tool(script, args, cwd=None):
    """Runs one of the bundled tools (survey, translator, module builder) as a separate process."""
    path = os.path.join(HERE, script)
    cmd = [sys.executable, '-I', path] + args if not getattr(sys, 'frozen', False) else [sys.executable, '--tool', script] + args
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def apply_known_patches(xbe, title):
    """Writes patches.txt for builds listed in tools/title_patches.json (keyed by the SHA-256 of the main executable). Unlisted builds are translated unmodified."""
    import hashlib, json
    digest = hashlib.sha256(xbe).hexdigest()
    try: table = json.load(open(os.path.join(HERE, 'title_patches.json'), encoding='utf-8'))
    except (OSError, ValueError): table = {}
    entry = table.get(digest)
    print('  executable hash (SHA-256): %s' % digest)   # compatibility reports are keyed by this value
    if not entry:
        print('  note: this build is not one the project has tested (executable hash %s...); it is translated without patches and may not run' % digest[:12]); return
    with open(os.path.join(title, 'patches.txt'), 'w') as f:
        f.write('# addr  hex bytes written over the image after load (from title_patches.json)'+chr(10))
        for p in entry.get('patches', []): f.write('%x %s' % (int(p['address'], 16), p['bytes']) + chr(10))
    print('  known build: %d patch(es) applied' % len(entry.get('patches', [])))


# ---- release download and verification ----
def http_get(url, max_bytes=2_000_000_000):
    req = urllib.request.Request(url, headers={'User-Agent': 'reforged-companion'})
    with urllib.request.urlopen(req, timeout=60) as r: return r.read(max_bytes)

def download_release_ipa(pubkey_hex, version=None):
    import ed25519
    if not pubkey_hex: die('this build of the companion has no release key built in; use --ipa <file> with an app you trust')
    if version: rel = json.loads(http_get('https://api.github.com/repos/%s/releases/tags/%s' % (REPO, version)))
    else:
        # The newest versioned release that carries a signed app. (GitHub's "latest release" is not used: other releases, such as the compiler toolchain, can take that slot.)
        rels = json.loads(http_get('https://api.github.com/repos/%s/releases?per_page=30' % REPO))
        def signed_app(r): return not r.get('draft') and r.get('tag_name', '').startswith('v') and any(a['name'].endswith('.ipa.sig') for a in r['assets']) and any(a['name'].endswith('.ipa') for a in r['assets'])
        rel = next((r for r in rels if signed_app(r)), None)
        if not rel: die('no released app was found on the project page yet; use --ipa <file> with an app you trust')
    ipa = next((a for a in rel['assets'] if a['name'].endswith('.ipa')), None)
    sig = next((a for a in rel['assets'] if a['name'] == (ipa['name'] + '.sig' if ipa else '')), None)
    if not ipa or not sig: die('release %s has no signed app' % rel.get('tag_name'))
    print('  downloading %s (%.0f MB)' % (ipa['name'], ipa['size'] / 1e6))
    data = http_get(ipa['browser_download_url']); signature = http_get(sig['browser_download_url'])
    if not ed25519.verify(bytes.fromhex(pubkey_hex), data, signature): die('the downloaded app does not match its signature: refusing to use it')
    print('  signature ok (%s)' % rel.get('tag_name'))
    return data


# ---- toolchain pack (compiler, linker, headers, symbol scanner) ----
def host_id():
    system, machine = platform.system(), platform.machine().lower()
    arch = 'arm64' if machine in ('arm64', 'aarch64') else 'x64' if machine in ('amd64', 'x86_64') else machine
    return {'Windows': 'windows', 'Linux': 'linux', 'Darwin': 'macos'}.get(system, system.lower()) + '-' + arch

def cache_dir():
    base = os.environ.get('LOCALAPPDATA') if platform.system() == 'Windows' else os.environ.get('XDG_CACHE_HOME') or os.path.join(os.path.expanduser('~'), '.cache')
    return os.path.join(base or os.path.expanduser('~'), 'Reforged', 'toolchain')

def unpack_zip(data, dest):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in z.infolist():
            path = os.path.join(dest, info.filename)
            if not os.path.abspath(path).startswith(os.path.abspath(dest) + os.sep): die('unsafe path in the toolchain pack')
            if info.is_dir(): continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as f: f.write(z.read(info))
            mode = (info.external_attr >> 16) & 0o777
            if mode: os.chmod(path, mode)

def get_toolchain(pubkey_hex):
    """Downloads (once) and verifies the toolchain pack for this computer; returns the folder it is unpacked in."""
    import ed25519
    host = host_id(); dest = os.path.join(cache_dir(), host)
    if os.path.exists(os.path.join(dest, 'manifest.json')): return dest
    if not pubkey_hex: die('this build of the companion has no release key built in; use --toolchain <folder> or --clang/--lld')
    try: rel = json.loads(http_get('https://api.github.com/repos/%s/releases/tags/toolchain' % REPO))
    except Exception as e: die('could not find the published compiler toolchain (%s). If this is a brand-new release the maintainer may not have published it yet; check the project page and try again later, or pass --toolchain <folder>.' % e)
    name = 'toolchain-%s.zip' % host
    asset = next((x for x in rel['assets'] if x['name'] == name), None); sig = next((x for x in rel['assets'] if x['name'] == name + '.sig'), None)
    if not asset or not sig: die('no toolchain pack is published for this computer (%s)' % host)
    print('  downloading the compiler toolchain once (%.0f MB)' % (asset['size'] / 1e6))
    data = http_get(asset['browser_download_url']); signature = http_get(sig['browser_download_url'])
    if not ed25519.verify(bytes.fromhex(pubkey_hex), data, signature): die('the downloaded toolchain does not match its signature: refusing to use it')
    shutil.rmtree(dest, ignore_errors=True); os.makedirs(dest, exist_ok=True); unpack_zip(data, dest)
    return dest

def pack_paths(folder):
    exe = '.exe' if platform.system() == 'Windows' else ''
    return {'clang': os.path.join(folder, 'bin', 'clang' + exe), 'lld': os.path.join(folder, 'bin', 'lld' + exe),
            'scanner': os.path.join(folder, 'xbsym', 'XbSymbolDatabaseCLI' + exe), 'sse2neon': os.path.join(folder, 'include')}


# ---- steps ----
def find_iso(arg):
    if arg: return arg
    folder = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else __file__))
    for where in (os.getcwd(), folder):
        found = sorted(glob.glob(os.path.join(where, '*.iso')))
        if found: return found[0]
    die('no disc image found: put the .iso next to this program, or pass --iso <file>')

def scan_symbols(xbe_path, cli, out_path):
    p = subprocess.run([cli, xbe_path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode('utf-8', errors='ignore')
    open(out_path, 'w', encoding='utf-8').write(text)
    return len(re.findall(r'= 0x[0-9a-fA-F]+', text))

def write_personal_ipa(base_ipa_bytes, out_path, files):
    """Copies the app and adds `files` ({path inside the .app: bytes}); permissions of the original entries are preserved."""
    zin = zipfile.ZipFile(io.BytesIO(base_ipa_bytes))
    app = next((n.split('/')[1] for n in zin.namelist() if n.startswith('Payload/') and n.count('/') >= 2 and n.split('/')[1].endswith('.app')), None)
    if not app: die('the app file has no .app inside Payload/')
    with zipfile.ZipFile(out_path, 'w', zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist(): zout.writestr(info, zin.read(info.filename))
        for rel, data in files.items():
            zi = zipfile.ZipInfo('Payload/%s/%s' % (app, rel), date_time=time.localtime()[:6]); zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            zout.writestr(zi, data)
    return app


def main():
    ap = argparse.ArgumentParser(description='Build a personal Reforged app for your game disc.')
    ap.add_argument('--iso', help='disc image (default: the first .iso next to this program)')
    ap.add_argument('--ipa', help='use this app file instead of downloading the latest release')
    ap.add_argument('--out', help='where to write the personal app (default: next to the disc image)')
    ap.add_argument('--toolchain', default='', help='folder of an unpacked toolchain pack (default: download and cache the signed pack)')
    ap.add_argument('--clang', default='', help='use this clang instead of the toolchain pack')
    ap.add_argument('--lld', default='', help='use this lld instead of the toolchain pack')
    ap.add_argument('--ld', default='lld', choices=['lld', 'system'])
    ap.add_argument('--sse2neon', default='', help='directory containing sse2neon.h')
    ap.add_argument('--xbsym-cli', default='', help='symbol scanner (XbSymbolDatabaseCLI)')
    ap.add_argument('--skip-symbols', action='store_true', help='do not scan for audio/controller entry points (test programs)')
    ap.add_argument('--no-disc', action='store_true', help='the program needs no disc at run time (test programs, homebrew)')
    ap.add_argument('--title-data', default='', help='folder of per-title files (patches.txt, fs_patches.txt, ...) to add to the app; they replace generated files of the same name')
    ap.add_argument('--pubkey', default=RELEASE_PUBLIC_KEY, help='release signing public key (hex)')
    ap.add_argument('--work', default='', help='working directory (default: a temporary one)')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    steps = 7

    iso = find_iso(a.iso)
    say(1, steps, 'reading %s' % os.path.basename(iso))
    import xiso
    try: img = xiso.Image(iso)
    except Exception as e: die('%s: %s' % (iso, e))
    entry = img.find('default.xbe') or img.find('xboxdash.xbe')
    if not entry: die('no executable found on the disc image')
    xbe = img.read(entry[0]); img.close()
    print('  found %s (%.1f MB)' % (entry[0], len(xbe) / 1e6))

    work = os.path.abspath(a.work) if a.work else tempfile.mkdtemp(prefix='reforged-')
    os.makedirs(os.path.join(work, 'work', 'lift'), exist_ok=True)
    xbe_path = os.path.join(work, 'default.xbe'); open(xbe_path, 'wb').write(xbe)
    title = os.path.join(work, 'title'); os.makedirs(title, exist_ok=True)

    say(2, steps, 'getting the app')
    if a.ipa: base = open(a.ipa, 'rb').read()
    else: base = download_release_ipa(a.pubkey)

    say(3, steps, "preparing the toolchain")
    if a.clang or a.lld:
        tc = {'clang': a.clang or 'clang++', 'lld': a.lld, 'scanner': a.xbsym_cli, 'sse2neon': a.sse2neon}
    else:
        tc = pack_paths(a.toolchain or get_toolchain(a.pubkey))
        for k in ('clang', 'lld'):
            if not os.path.exists(tc[k]): die('the toolchain is missing %s' % tc[k])
    if a.xbsym_cli: tc['scanner'] = a.xbsym_cli
    if a.sse2neon: tc['sse2neon'] = a.sse2neon
    print('  ok')

    say(4, steps, "finding the game's audio and controller routines")
    if a.skip_symbols or not tc.get('scanner') or not os.path.exists(tc['scanner']): print('  skipped' + ('' if a.skip_symbols else ' (no symbol scanner available: the game will run without its own audio and controller hooks)'))
    else:
        n = scan_symbols(xbe_path, tc['scanner'], os.path.join(work, 'xbsym.txt'))
        print('  %d symbols' % n)
        rc, out = run_tool('gen_title_data.py', ['--xbe', xbe_path, '--xbsym', os.path.join(work, 'xbsym.txt'), '--out', title])
        if rc != 0: die('could not build the title data:\n' + out)
        print('  ' + out.strip().splitlines()[-1])

    apply_known_patches(xbe, title)
    if a.title_data:
        os.makedirs(title, exist_ok=True)
        for f in sorted(os.listdir(a.title_data)):
            src = os.path.join(a.title_data, f)
            if os.path.isfile(src) and f.lower().endswith('.txt'): shutil.copyfile(src, os.path.join(title, f)); print('  title data: %s' % f)
    say(5, steps, 'translating the game code on this computer')
    rc, out = run_tool('lift_survey.py', [xbe_path], cwd=work)
    if rc != 0: die('survey failed:\n' + out[-2000:])
    rc, out = run_tool('lift.py', [xbe_path, '--out', os.path.join('work', 'lift', 'gen'), '--patches', os.path.join(title, 'patches.txt')], cwd=work)   # the translated code must include the title's byte patches
    if rc != 0: die('translation failed:\n' + out[-2000:])
    print('  ' + [l for l in out.splitlines() if l.startswith('lifted')][-1])

    say(6, steps, 'building the game module (this is the slow step)')
    module = os.path.join(work, 'reforged_game.dylib')
    margs = ['--gen', os.path.join(work, 'work', 'lift', 'gen'), '--out', module, '--clang', tc['clang'], '--ld', a.ld]
    if tc.get('lld'): margs += ['--lld', tc['lld']]
    if tc.get('sse2neon'): margs += ['--sse2neon', tc['sse2neon']]
    rc, out = run_tool('build_module.py', margs)
    print(out[-600:].rstrip())
    if rc != 0: die('the module did not build')

    say(7, steps, 'writing your personal app')
    files = {'Frameworks/reforged_game.dylib': open(module, 'rb').read(), 'title/default.xbe': xbe}
    for f in os.listdir(title): files['title/' + f] = open(os.path.join(title, f), 'rb').read()
    if a.no_disc: files['title/no-disc.txt'] = b'no disc needed\n'
    out_path = a.out or os.path.join(os.path.dirname(os.path.abspath(iso)), os.path.splitext(os.path.basename(iso))[0] + '-personal.ipa')
    write_personal_ipa(base, out_path, files)
    print('\nDone: %s' % out_path)
    print('Open it in AltStore, SideStore or Sideloadly to install it, then copy %s to the device (Files app / Finder file sharing)' % os.path.basename(iso))
    print('and choose it in the app. This file contains code made from your disc: keep it to yourself. Nothing was uploaded.')
    if not a.keep and not a.work: shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--tool':   # frozen builds run the bundled tools through themselves
        import runpy; script = os.path.join(HERE, sys.argv[2]); sys.argv = [script] + sys.argv[3:]; runpy.run_path(script, run_name='__main__'); sys.exit(0)
    # Started by double-click (a frozen program with no arguments): keep the window open at the end so the result or the error can be read.
    pause = getattr(sys, 'frozen', False) and len(sys.argv) == 1
    code = 0
    try: main()
    except SystemExit as e: code = e.code if isinstance(e.code, int) else (1 if e.code else 0)
    except KeyboardInterrupt: print('\nstopped'); code = 1
    except Exception as e:
        import traceback; traceback.print_exc(); print('\nerror: %s\nIf this keeps happening, please report it (without the disc image or the files made from it).' % e); code = 1
    if pause:
        try: input('\nPress Enter to close this window.')
        except EOFError: pass
    sys.exit(code)
