"""Prepare the Android build: the generated code and SDL 3, before Gradle builds the APK.

Usage: python -I android/prepare.py [--gen] [--app <GTRacingFreemiumHD.app>] [--sdl-tag release-3.4.8]

  --gen      run stage 2 again (after a change to recomp/)

  1. checks the game's program (GTRacingFreemiumHD.app/GTRacingFreemiumHD in the repository's folder, or --app):
     only the 1.3.4b build (SHA-1 below) is supported, as for the PC port;
  2. stage 1 (recomp/discover.py) into analysis/stage1, when it has not run yet;
  3. stage 2 (recomp/codegen.py) into build/gen, when it is missing or --gen is given;
  4. the import bridges (recomp/bridgegen.py) into build/host/gen/bridges.c;
  5. build/gen_android: build/gen with GNU ld's --wrap rule made explicit (below);
  6. SDL 3's sources into android/SDL (git clone of --sdl-tag), when they are not there yet;
  7. the port's menu pages (the PC menus, patched SWFs) into build/android/patch, for the phone's GTRacing/patch.

Then:  cd android && ./gradlew assembleRelease   (Windows: gradlew.bat assembleRelease)

Why step 5.  The PC build links with GNU ld, whose --wrap=F_x sends only the *other* objects' references to F_x to
__wrap_F_x: a call to F_x from the file that defines it stays a call to F_x.  Several port features count on that
("called from other generated files only", host/build.py).  LLVM's lld, the Android linker, wraps the references
inside the defining file as well.  So the defining file's own references are renamed to F_x__def here, and F_x is
made an alias of it: lld's --wrap then changes exactly what GNU ld's does.  The wrapped names are read from
host/build.py's link line, so the two builds never disagree.

Only Python 3 and the packages the PC build uses (capstone) are needed; no compiler.  The game is read, never
written; its files stay out of the APK (the player copies them to the phone)."""
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME_SHA1 = 'eaeca1915d8b0a250583afed587f61ecb85a728a'
GEN = os.path.join(ROOT, 'build', 'gen')
GEN_ANDROID = os.path.join(ROOT, 'build', 'gen_android')
HOST_GEN = os.path.join(ROOT, 'build', 'host', 'gen')
STAGE1 = os.path.join(ROOT, 'analysis', 'stage1', 'functions.json')
SDL_DIR = os.path.join(HERE, 'SDL')


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def run(cmd):
    print('>', ' '.join(os.path.relpath(c, ROOT) if os.path.isabs(c) else c for c in cmd))
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode:
        sys.exit(r.returncode)


def wrapped_names():
    """the F_ functions host/build.py links with --wrap (malloc and the rest are the C library's, not ours)"""
    src = open(os.path.join(ROOT, 'host', 'build.py'), encoding='utf-8').read()
    return sorted(set(re.findall(r'--wrap=(F_[0-9a-f]+)', src)))


def make_gen_android(wraps):
    os.makedirs(GEN_ANDROID, exist_ok=True)
    srcs = sorted(f for f in os.listdir(GEN) if f.endswith(('.c', '.h')))
    want = set(wraps)
    defined_in = {}
    texts = {}
    for f in srcs:
        t = open(os.path.join(GEN, f), encoding='utf-8').read()
        texts[f] = t
        if f.startswith('code_'):
            for name in re.findall(r'^void (F_[0-9a-f]+)\(CPU \*c\) \{', t, re.M):
                if name in want:
                    defined_in[name] = f
    missing = sorted(want - set(defined_in))
    if missing:
        sys.exit('wrapped functions not found in build/gen: %s' % ', '.join(missing))
    by_file = {}
    for name, f in defined_in.items():
        by_file.setdefault(f, []).append(name)
    kept = set()
    for f in srcs:
        out = os.path.join(GEN_ANDROID, f)
        kept.add(f)
        t = texts[f]
        names = by_file.get(f)
        if names:
            for n in names:
                t = re.sub(r'\b%s\b' % n, n + '__def', t)
            decl = ''.join('void %s__def(CPU *c);\n' % n for n in sorted(names))
            alias = ''.join('void %s(CPU *c) __attribute__((alias("%s__def")));\n' % (n, n) for n in sorted(names))
            t = t.replace('#include "imports.h"\n', '#include "imports.h"\n' + decl, 1) + \
                '\n/* android/prepare.py: GNU ld --wrap semantics under lld */\n' + alias
        if not os.path.exists(out) or open(out, encoding='utf-8').read() != t:
            with open(out, 'w', encoding='utf-8', newline='\n') as fp:
                fp.write(t)
    for f in os.listdir(GEN_ANDROID):                    # a file codegen no longer makes
        if f not in kept and f.endswith(('.c', '.h')):
            os.remove(os.path.join(GEN_ANDROID, f))
    print('build/gen_android: %d files, %d wrapped functions in %d files' % (len(srcs), len(defined_in),
                                                                            len(by_file)))


def patch_menus(app):
    """the port's menu pages (Options > Display, the Touch Screen switch, the Exit and Name tiles): the game's own
    menu SWFs patched as host/build.py does, into build/android/patch; the player copies that folder next to the
    game on the phone (GTRacing/patch).  Without it the game has its own menus, and gtracing.ini the settings."""
    out = os.path.join(ROOT, 'build', 'android', 'patch')
    tools = os.path.join(ROOT, 'tools')
    py = sys.executable
    for name in ('MZlN_MiNH_XiZEKXiiMlHM.FAK', 'MZlN_MiNH_XiZELBKXiiMlHM.FAK', 'MZlN_MiNH_XiZEKXiiMlHM_EdAXiF.FAK',
                 'LHBMiNHFKXiiMlHM.FAK', 'LHBMiNHFLBKXiiMlHM.FAK', 'LHBMiNHFKXiiMlHM_EdAXiF.FAK'):
        swf = os.path.join(app, name)
        if not os.path.exists(swf):
            continue
        dst = os.path.join(out, name)
        os.makedirs(out, exist_ok=True)
        steps = ['swfpatch_controls']
        if name in ('MZlN_MiNH_XiZEKXiiMlHM.FAK', 'MZlN_MiNH_XiZELBKXiiMlHM.FAK'):
            steps += ['swfpatch_exit', 'swfpatch_name']
        if not name.endswith('_EdAXiF.FAK'):
            steps.append('swfpatch_display')
        if name.startswith('MZlN_MiNH_'):
            steps.append('swfpatch_cashnew')
        src = swf
        for st in steps:
            run([py, '-I', os.path.join(tools, st + '.py'), src, dst])
            src = dst
    if os.path.isdir(out):
        print('the menu pages: copy build/android/patch to the phone, as GTRacing/patch (next to the game)')


def main():
    app = arg('--app', os.path.join(ROOT, 'GTRacingFreemiumHD.app'))
    exe = os.path.join(app, 'GTRacingFreemiumHD')
    if not os.path.isfile(exe):
        sys.exit('no game program at %s (put your GTRacingFreemiumHD.app in the repository folder, or give --app)'
                 % exe)
    sha = hashlib.sha1(open(exe, 'rb').read()).hexdigest()
    if sha != GAME_SHA1:
        sys.exit('%s has SHA-1 %s, not %s: only the 1.3.4b build is supported' % (exe, sha, GAME_SHA1))
    py = sys.executable
    if not os.path.exists(STAGE1):
        run([py, '-I', os.path.join(ROOT, 'recomp', 'discover.py'), exe, os.path.dirname(STAGE1)])
    if '--gen' in sys.argv or not os.path.exists(os.path.join(GEN, 'dispatch.c')):
        run([py, '-I', os.path.join(ROOT, 'recomp', 'codegen.py'), exe, STAGE1, GEN])
    run([py, '-I', os.path.join(ROOT, 'recomp', 'bridgegen.py'), exe, os.path.join(ROOT, 'host', 'core', 'imports.sig'),
         HOST_GEN])
    make_gen_android(wrapped_names())
    if not os.path.exists(os.path.join(SDL_DIR, 'CMakeLists.txt')):
        tag = arg('--sdl-tag', 'release-3.4.8')
        run(['git', 'clone', '--depth', '1', '--branch', tag, 'https://github.com/libsdl-org/SDL', SDL_DIR])
    patch_menus(app)
    print('ready: cd android && ./gradlew assembleRelease')


if __name__ == '__main__':
    main()
