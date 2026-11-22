"""Stage 3: ARM-ABI bridges for every import of the game (and for host Objective-C methods).

Usage: python -I bridgegen.py <executable> <host/core/imports.sig> <out_dir>

Writes out_dir/bridges.c: one  void I_<name>(CPU *c)  per import the binary has.  A bridge reads
the arguments the iOS ARM ABI put in r0-r3 and on the guest stack, calls the host function with
native C types and puts the result back in r0 (r0:r1 for 64-bit values).  Imports with no line in
the signature file get a stub that reports the name when the game reaches it.

Signature file, one import per line (# comments):
    name   ret  args  [flags]  [=crt_function]
  name   the Mach-O symbol without its first underscore (strlen, _Znwm, objc_msgSend)
  ret    v void, i 32-bit int/pointer, l 64-bit int, f float, d double
  args   one letter per argument (i l f d, R CGRect, P CGPoint, S CGSize, T CGAffineTransform),
         '-' for none; a trailing '+' means '...': the host function gets a pointer to the
         variadic words (laid out as an i386 va_list, so vsnprintf can take it)
  flags  C   the host function gets CPU *c first
         raw the host defines  void I_<name>(CPU *c)  itself (no bridge is generated)
  =crt   call this C library function instead of h_<name>
The iOS ABI (APCS): arguments are 32-bit words in r0-r3 then on the stack, 64-bit values take the
next two words with no alignment (they can be split between r3 and the stack), floats are passed
as their bits in core registers, structs by value as their words.  Structs returned by value
(bigger than 4 bytes) go through a hidden pointer in r0: write them as a first 'i' argument.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402

WORDS = {'i': 1, 'l': 2, 'f': 1, 'd': 2, 'R': 4, 'P': 2, 'S': 2, 'T': 6}
CTYPE = {'v': 'void', 'i': 'u32', 'l': 'u64', 'f': 'float', 'd': 'double',
         'R': 'GtRect', 'P': 'GtPoint', 'S': 'GtSize', 'T': 'GtAffine'}
STRUCT_FIELDS = {'R': 4, 'P': 2, 'S': 2, 'T': 6}


def cname(name):
    """Same C name as recomp/armc.py Program.import_cname."""
    return 'I_' + re.sub(r'[^0-9A-Za-z_]', lambda m: '_%02x' % ord(m.group(0)), name.lstrip('_'))


def parse_sig(path):
    sigs = {}
    for ln, line in enumerate(open(path, encoding='utf-8-sig'), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        name = '_' + parts[0]
        if len(parts) >= 2 and parts[1] == 'raw':
            sigs[name] = {'raw': True, 'line': ln}
            continue
        if len(parts) < 3:
            raise SystemExit('%s:%d: need name ret args' % (path, ln))
        ret, args = parts[1], parts[2]
        flags, crt = '', None
        for p in parts[3:]:
            if p.startswith('='):
                crt = p[1:]
            else:
                flags += p
        var = args.endswith('+')
        args = args.rstrip('+')
        if args == '-':
            args = ''
        if ret not in CTYPE or any(a not in WORDS for a in args):
            raise SystemExit('%s:%d: bad types %s %s' % (path, ln, ret, args))
        if name in sigs:
            raise SystemExit('%s:%d: %s twice' % (path, ln, name))
        sigs[name] = {'raw': False, 'ret': ret, 'args': args, 'var': var, 'cpu': 'C' in flags,
                      'crt': crt, 'line': ln}
    return sigs


def arg_expr(t, w):
    if t == 'i':
        return 'AW(%d)' % w
    if t == 'f':
        return 'bits2f(AW(%d))' % w
    if t == 'l':
        return '((u64)AW(%d) | (u64)AW(%d) << 32)' % (w, w + 1)
    if t == 'd':
        return 'bits2d((u64)AW(%d) | (u64)AW(%d) << 32)' % (w, w + 1)
    n = STRUCT_FIELDS[t]
    return '(%s){%s}' % (CTYPE[t], ', '.join('bits2f(AW(%d))' % (w + k) for k in range(n)))


def bridge(name, s):
    c = cname(name)
    if s['raw']:
        return 'void %s(CPU *c);\n' % c, None
    ret, args = s['ret'], s['args']
    params, exprs, w = [], [], 0
    if s['cpu']:
        params.append('CPU *')
        exprs.append('c')
    for t in args:
        params.append(CTYPE[t])
        exprs.append(arg_expr(t, w))
        w += WORDS[t]
    if s['var']:
        params.append('const u32 *')
        exprs.append('va_words(c, %d, va_)' % w)
    proto_params = ', '.join(params) or 'void'
    target = s['crt'] or 'h_' + c[2:]
    out = []
    if s['crt'] and not s['crt'].startswith('h_'):
        # a C library function: its 'i' words as crt_w (bridges.c HEAD: zero-extended to a 64-bit word on a 64-bit
        # host, where they are its pointers and sizes; the u32 itself on a 32-bit one)
        cparams = ', '.join('crt_w' if p == 'u32' else p for p in params) or 'void'
        cret = 'crt_w' if ret == 'i' else CTYPE[ret]
        call = '((%s (*)(%s))CRT(%s))(%s)' % (cret, cparams, target, ', '.join(exprs))
    else:
        out.append('%s %s(%s);' % (CTYPE[ret], target, proto_params))
        call = '%s(%s)' % (target, ', '.join(exprs))
    body = []
    if s['var']:
        body.append('u32 va_[VA_WORDS];')
    if ret == 'v' and not exprs:
        body.append('(void)c;')                  # no argument and no result: -Wextra's unused parameter
    if ret == 'v':
        body.append('%s;' % call)
    elif ret == 'i':
        body.append('c->r[0] = (u32)%s;' % call)
    elif ret == 'f':
        body.append('c->r[0] = f2bits(%s);' % call)
    elif ret == 'l':
        body.append('u64 r_ = %s; c->r[0] = (u32)r_; c->r[1] = (u32)(r_ >> 32);' % call)
    elif ret == 'd':
        body.append('u64 r_ = d2bits(%s); c->r[0] = (u32)r_; c->r[1] = (u32)(r_ >> 32);' % call)
    else:
        raise SystemExit('struct return for %s: use a pointer argument' % name)
    out.append('void %s(CPU *c) { %s }' % (c, ' '.join(body)))
    return '\n'.join(out) + '\n', target if not s['crt'] else None


HEAD = r'''/* generated by recomp/bridgegen.py from host/core/imports.sig: do not edit */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rt.h"
#include "abi.h"

#pragma GCC diagnostic ignored "-Wcast-function-type"
#define AW(i) arg_word(c, (i))
/* a C library function called with the guest's 32-bit words (on i686 the same bits as its own pointers and ints):
   its address goes through an empty asm, so gcc no longer matches the cast against the library's prototype (gcc 16
   warns "function called through a non-compatible type" for each, 44 lines a build, with no option to turn it
   off) */
static inline void *crt_fn(void *p) { __asm__("" : "+r"(p)); return p; }
#define CRT(f) crt_fn((void *)(uintptr_t)(f))
/* a C library call's word: on a 64-bit host its pointer and size arguments are 64 bits and the bits above a u32
   argument are not defined, so each word is passed zero-extended (the game's memory is below 4 GB there) */
#if defined(__LP64__)
typedef uintptr_t crt_w;
#else
typedef u32 crt_w;
#endif
'''


def main():
    exe, sigpath, out = sys.argv[1], sys.argv[2], sys.argv[3]
    m = MachO(exe)
    sigs = parse_sig(sigpath)
    names = sorted(set(m.imports))
    unknown = sorted(n for n in sigs if n not in names)
    os.makedirs(out, exist_ok=True)
    lines = [HEAD]
    table = []
    done = stub = 0
    seen_c = {}
    for n in names:
        c = cname(n)
        if c in seen_c:
            raise SystemExit('C name clash: %s and %s' % (seen_c[c], n))
        seen_c[c] = n
        s = sigs.get(n)
        if s is None:
            lines.append('void %s(CPU *c) { rt_missing_import(c, "%s"); }\n' % (c, n))
            stub += 1
        else:
            text, _ = bridge(n, s)
            lines.append(text)
            done += 1
        table.append((n, c))
    lines.append('const RtBridgeEntry rt_bridges[] = {')
    for n, c in sorted(table):
        lines.append('  {"%s", %s},' % (n, c))
    lines.append('};\nconst unsigned rt_bridge_count = %d;\n' % len(table))
    with open(os.path.join(out, 'bridges.c'), 'w', newline='\n') as fp:
        fp.write('\n'.join(lines))
    rep = ['imports %d: bridged %d, stubs %d' % (len(names), done, stub)]
    if unknown:
        rep.append('signature lines for names the binary does not import: ' + ', '.join(u[1:] for u in unknown))
    print('\n'.join(rep))
    with open(os.path.join(out, 'bridges_report.txt'), 'w') as fp:
        fp.write('\n'.join(rep) + '\n\nnot bridged yet:\n' +
                 '\n'.join('  ' + n for n in names if n not in sigs) + '\n')


if __name__ == '__main__':
    main()
