"""The FPU against an independent reference.

    python tests/fpu_suite.py [--cases N] [--seed S] [--show K]

Random operands are run through `mc_cpu_tests fpu-eval` (the 68040 with the
full 68881 instruction set, through the CPU's decoder) and compared with
results computed here from the definitions alone:

  - arithmetic, square root, FMOD/FREM, FINT/FINTRZ, FGETEXP/FGETMAN, FSCALE,
    FSGLMUL/FSGLDIV, the 68040's single/double forms, FCMP, conversions in
    and out of every binary format: exact rational arithmetic (Python
    fractions and integers), rounded by this file's own implementation of
    the MC68881/MC68882 User's Manual rules (4.5.5.2: underflow detected
    before rounding, round to the precision and mode, overflow after; 6.1.4
    and 6.1.5 default results); result bits, condition codes, quotient byte
    and the EXC flags must match exactly;
  - transcendental functions and FMOVECR: mpmath at 256 bits, rounded the
    same way; the result must be within 1 ulp of the correctly rounded value
    (the accuracy claimed in fpu_trans.c), and the maximum error is reported;
  - packed decimal in and out: the exact decimal value and Python's decimal
    module.

Writes nothing. Exit status 1 on any mismatch, 77 if the build is missing.
"""
from __future__ import annotations

import argparse
import random
import subprocess
import sys
from fractions import Fraction
from math import isqrt
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVAL = ROOT / 'build' / 'x64' / 'mc_cpu_tests.exe'

RN, RZ, RM, RP = 0, 1, 2, 3
FMT = {'X': (64, -16383, 16383), 'D': (53, -1022, 1023), 'S': (24, -126, 127)}
INEX1, INEX2, DZ, UNFL, OVFL, OPERR, SNAN = 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000

# --- extended values ------------------------------------------------------------

def x_decode(se, m):
    """(se, m) -> ('num', Fraction) | ('inf', sign) | ('nan', m)."""
    s = se >> 15
    e = se & 0x7FFF
    if e == 0x7FFF:
        return ('nan', m) if m & 0x7FFFFFFFFFFFFFFF else ('inf', s)
    v = Fraction(m) * Fraction(2) ** (e - 16383 - 63)
    return ('num', -v if s else v, s)


def flog2(a: Fraction) -> int:
    """floor(log2(a)) for a > 0."""
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    elif Fraction(2) ** (e + 1) <= a:
        e += 1
    return e


def round_frac(v: Fraction, sign: int, fmt, mode):
    """Rounds |v| (sign separate) to fmt; returns (Fraction result or 'inf',
    flags)."""
    bits, emin, emax = fmt
    a = abs(v)
    if a == 0:
        return Fraction(0), 0
    e = flog2(a)
    flags = 0
    tiny = e < emin
    if tiny:
        flags |= UNFL
    q = (emin if tiny else e) - (bits - 1)
    n = a / Fraction(2) ** q
    i = n.numerator // n.denominator
    rem = n - i
    if rem:
        flags |= INEX2
        if mode == RN:
            up = rem > Fraction(1, 2) or (rem == Fraction(1, 2) and i & 1)
        elif mode == RZ:
            up = False
        elif mode == RM:
            up = bool(sign)
        else:
            up = not sign
        if up:
            i += 1
    r = Fraction(i) * Fraction(2) ** q
    if r and flog2(r) > emax:
        flags |= OVFL
        if mode == RN or (mode == RP and not sign) or (mode == RM and sign):
            return 'inf', flags
        return Fraction(2 ** bits - 1) * Fraction(2) ** (emax - bits + 1), flags
    return r, flags


def x_encode(r, sign):
    """A rounded magnitude (Fraction or 'inf') -> (se, m) in extended."""
    s = 0x8000 if sign else 0
    if r == 'inf':
        return s | 0x7FFF, 0
    if r == 0:
        return s, 0
    e = flog2(r)
    if e >= -16383:
        m = r / Fraction(2) ** (e - 63)
        assert m.denominator == 1
        return s | (e + 16383), int(m)
    m = r / Fraction(2) ** (-16383 - 63)
    assert m.denominator == 1
    return s, int(m)


def rand_x(rng: random.Random, wide=True):
    """A random extended operand: mostly normal, some denormal or tiny/huge."""
    k = rng.random()
    s = rng.randrange(2) << 15
    if k < 0.04:
        return s, rng.getrandbits(63) | 1                      # denormal
    if not wide or k < 0.85:
        e = 16383 + rng.randrange(-80, 80)
    else:
        e = rng.randrange(1, 0x7FFE)
    m = (1 << 63) | rng.getrandbits(63)
    if rng.random() < 0.15:
        m &= ~((1 << rng.randrange(1, 60)) - 1)               # short mantissas, exact cases
    return s | e, m


# --- the processor ------------------------------------------------------------------

class Fpu:
    def __init__(self):
        self.p = subprocess.Popen([str(EVAL), 'fpu-eval'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def batch(self, lines):
        out, _ = self.p.communicate('\n'.join(lines) + '\n', timeout=900)
        return [l.split() for l in out.splitlines()]


def fpcr(mode, prec=0):
    return (prec << 6) | (mode << 4)


PREC = {0: 'X', 1: 'S', 2: 'D'}

# --- exact reference operations ---------------------------------------------------------

def ref_general(op, dse, dm, sse, sm, mode, prec):
    """The result of opmode `op` on FP0 = (dse, dm), FP1 = (sse, sm) as
    (se, m, flags, quotient byte or None, cc) or None when the case is out of
    scope (specials are covered by the unit tests)."""
    d, s = x_decode(dse, dm), x_decode(sse, sm)
    if d[0] != 'num' or s[0] != 'num':
        return None
    dv, sv = d[1], s[1]
    fmt = FMT[PREC[prec]]
    if op >= 0x40:
        fmt = FMT['D'] if op & 4 else FMT['S']
    base = op
    if op >= 0x40:
        base = {0x40: 0x00, 0x41: 0x04, 0x58: 0x18, 0x5A: 0x1A, 0x60: 0x20, 0x62: 0x22, 0x63: 0x23,
                0x68: 0x28}[op & ~4]
    flags = 0
    q = None
    if base == 0x00:
        v = sv
    elif base == 0x18:
        v = abs(sv)
    elif base == 0x1A:
        v = -sv
    elif base == 0x22:
        v = dv + sv
    elif base == 0x28:
        v = dv - sv
    elif base == 0x23:
        v = dv * sv
    elif base == 0x20:
        if sv == 0:
            return None
        v = dv / sv
    elif base in (0x24, 0x27):
        def trunc24(x):
            if x == 0:
                return x
            e = flog2(abs(x))
            u = Fraction(2) ** (e - 23)
            t = abs(x) / u
            t = Fraction(t.numerator // t.denominator) * u
            return t if x > 0 else -t
        a, b = trunc24(dv), trunc24(sv)
        if base == 0x24:
            if b == 0:
                return None
            v = a / b
        else:
            v = a * b
        fmt = (24, -16383, 16383)
    elif base == 0x04:
        if sv < 0:
            return None
        if sv == 0:
            v = sv
        else:
            # floor(sqrt) with 80 extra bits and a sticky quarter
            num, den = sv.numerator, sv.denominator
            k = 0
            while den > 1:
                den //= 2
                k += 1
            # sv = num / 2^k
            if k & 1:
                num *= 2
                k += 1
            t = num << 200
            r = isqrt(t)
            v = Fraction(r) / Fraction(2) ** (100 + k // 2)
            if r * r != t:
                v += Fraction(1, 4) / Fraction(2) ** (100 + k // 2)
    elif base in (0x21, 0x25):
        if sv == 0:
            return None
        ratio = dv / sv
        if base == 0x21:
            qi = int(ratio)                                        # toward zero
        else:
            fl = ratio.numerator // ratio.denominator
            rem = ratio - fl
            qi = fl + 1 if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and fl & 1) else fl
        v = dv - qi * sv
        q = ((1 if (dv < 0) != (sv < 0) else 0) << 7) | (abs(qi) & 0x7F)
        if v == 0:
            se, m = x_encode(Fraction(0), dv < 0)
            return se, m, 0, q, 0x04000000 | (0x08000000 if dv < 0 else 0)
    elif base in (0x01, 0x03):
        fl = sv.numerator // sv.denominator
        rem = sv - fl
        if rem:
            flags |= INEX2
            if base == 0x03:
                fl = int(sv)
            elif mode == RN:
                fl = fl + 1 if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and fl & 1) else fl
            elif mode == RZ:
                fl = int(sv)
            elif mode == RP:
                fl = fl + 1
        v = Fraction(fl)
        if v == 0:
            se, m = x_encode(Fraction(0), sv < 0)
            return se, m, flags, None, 0x04000000 | (0x08000000 if sv < 0 else 0)
    elif base == 0x1E:
        v = Fraction(flog2(abs(sv)))
    elif base == 0x1F:
        e = flog2(abs(sv))
        v = sv / Fraction(2) ** e
    elif base == 0x26:
        n = int(sv)
        if abs(n) > 20000:
            return None
        v = dv * Fraction(2) ** n
    else:
        return None
    sign = 1 if v < 0 else 0
    if v == 0:
        # an exact cancellation of nonzero operands: -0 in RM alone; FGETEXP
        # of a number in [1, 2): +0
        neg = mode == RM and base != 0x1E
        se, m = x_encode(Fraction(0), neg)
        return se, m, flags, q, 0x04000000 | (0x08000000 if neg else 0)
    r, f2 = round_frac(v, sign, fmt, mode)
    flags |= f2
    se, m = x_encode(r, sign)
    cc = (0x08000000 if sign else 0) | (0x02000000 if r == 'inf' else 0) | (0x04000000 if r == 0 else 0)
    return se, m, flags, q, cc


TRANS = {0x02: 'sinh', 0x06: 'log1p', 0x08: 'expm1', 0x09: 'tanh', 0x0A: 'atan', 0x0C: 'asin', 0x0D: 'atanh',
         0x0E: 'sin', 0x0F: 'tan', 0x10: 'exp', 0x11: 'exp2', 0x12: 'exp10', 0x14: 'log', 0x15: 'log10',
         0x16: 'log2', 0x19: 'cosh', 0x1C: 'acos', 0x1D: 'cos'}


def mpf_frac(x) -> Fraction:
    import mpmath
    sign, man, exp, _ = mpmath.mpf(x)._mpf_
    if not man:
        return Fraction(0)
    v = Fraction(int(man)) * Fraction(2) ** int(exp)
    return -v if sign else v


def ulps(se1, m1, se2, m2):
    """Distance in units of the last place between two finite extended
    values of the same sign."""
    def ordinal(se, m):
        v = ((se & 0x7FFF) << 64) | m if (se & 0x7FFF) else m
        return -v if se & 0x8000 else v
    return abs(ordinal(se1, m1) - ordinal(se2, m2))


def main(argv) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=68881)
    ap.add_argument('--show', type=int, default=3)
    a = ap.parse_args(argv)
    if not EVAL.exists():
        print('fpu_suite: build first (pwsh -File tools/build.ps1)')
        return 77
    try:
        import mpmath
    except ImportError:
        print('fpu_suite: mpmath is not installed; skipped')
        return 77
    mpmath.mp.prec = 256
    rng = random.Random(a.seed)
    failures = []
    report = []

    # 1. exact operations
    ops = [0x00, 0x01, 0x03, 0x04, 0x18, 0x1A, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
           0x40, 0x41, 0x44, 0x45, 0x58, 0x5A, 0x5C, 0x5E, 0x60, 0x62, 0x63, 0x64, 0x66, 0x67, 0x68, 0x6C]
    cases, lines = [], []
    for _ in range(a.cases * 4):
        op = rng.choice(ops)
        mode, prec = rng.randrange(4), rng.choice([0, 0, 1, 2])
        d, s = rand_x(rng), rand_x(rng)
        if op in (0x21, 0x25) and rng.random() < 0.7:
            s = ((s[0] & 0x8000) | ((d[0] & 0x7FFF) - rng.randrange(0, 70)), s[1])
        if op == 0x26:
            n = rng.randrange(-300, 300)
            s = x_encode(Fraction(abs(n)), n < 0)
        if op in (0x22, 0x28) and rng.random() < 0.3:
            s = ((s[0] & 0x8000) | (d[0] & 0x7FFF), d[1] ^ rng.getrandbits(8))    # cancellation
        cases.append((op, mode, prec, d, s))
        lines.append(f'G {op:X} {fpcr(mode, prec):X} {d[0]:X} {d[1]:X} {s[0]:X} {s[1]:X}')
    out = Fpu().batch(lines)
    checked = 0
    for (op, mode, prec, d, s), res in zip(cases, out):
        ref = ref_general(op, d[0], d[1], s[0], s[1], mode, prec)
        if ref is None:
            continue
        checked += 1
        se, m, flags, q, cc = ref
        gse, gm, fpsr = int(res[0], 16), int(res[1], 16), int(res[4], 16)
        exc = fpsr & 0xFF00
        ok = gse == se and gm == m and exc == flags and (fpsr & 0x0F000000) == cc
        if q is not None:
            ok = ok and ((fpsr >> 16) & 0xFF) == q
        if not ok:
            failures.append(f'op {op:02X} mode {mode} prec {prec} {d[0]:04X}:{d[1]:016X} {s[0]:04X}:{s[1]:016X} -> '
                            f'{gse:04X}:{gm:016X} fpsr {fpsr:08X}; want {se:04X}:{m:016X} exc {flags:04X} cc {cc:08X}'
                            f'{"" if q is None else f" q {q:02X}"}')
    report.append(f'arithmetic and rounding: {checked} cases exact')

    # 2. conversions in and out
    lines, cases = [], []
    for _ in range(a.cases):
        mode, prec = rng.randrange(4), rng.choice([0, 1, 2])
        kind = rng.choice(['S', 'D', 'L', 'W', 'B'])
        if kind == 'S':
            w = (rng.getrandbits(32) & ~0x7F800000) | (rng.randrange(0, 255) << 23)
            words = (w, 0, 0)
            v = (Fraction(1) + Fraction(w & 0x7FFFFF, 1 << 23)) * Fraction(2) ** (((w >> 23) & 0xFF) - 127) \
                if (w >> 23) & 0xFF else Fraction(w & 0x7FFFFF) * Fraction(2) ** (-149)
            fmtc = 1
        elif kind == 'D':
            w = (rng.getrandbits(64) & ~(0x7FF << 52)) | (rng.randrange(0, 2047) << 52)
            words = (w >> 32, w & 0xFFFFFFFF, 0)
            ex = (w >> 52) & 0x7FF
            fr = w & ((1 << 52) - 1)
            v = (Fraction(1) + Fraction(fr, 1 << 52)) * Fraction(2) ** (ex - 1023) if ex else \
                Fraction(fr) * Fraction(2) ** (-1074)
            fmtc = 5
        else:
            bits = {'L': 32, 'W': 16, 'B': 8}[kind]
            iv = rng.getrandbits(bits)
            words = (iv, 0, 0)
            v = Fraction(iv - (1 << bits) if iv >> (bits - 1) else iv)
            fmtc = {'L': 0, 'W': 4, 'B': 6}[kind]
            w = iv
        neg = (words[0] >> 31) if kind in 'SDL' else (w >> ({'W': 15, 'B': 7}.get(kind, 31)) & 1)
        if v != 0 and kind in 'SD':
            v = -v if neg else v
        cases.append(('in', kind, mode, prec, v))
        lines.append(f'I {fmtc:X} {fpcr(mode, prec):X} {words[0]:X} {words[1]:X} {words[2]:X}')
        x = rand_x(rng, wide=rng.random() < 0.3)
        okind = rng.choice(['S', 'D', 'L', 'W', 'B', 'X'])
        cases.append(('out', okind, mode, 0, x))
        ofmt = {'L': 0, 'S': 1, 'X': 2, 'W': 4, 'D': 5, 'B': 6}[okind]
        lines.append(f'O {ofmt:X} 0 {fpcr(mode):X} {x[0]:X} {x[1]:X}')
    out = Fpu().batch(lines)
    nconv = 0
    for (direction, kind, mode, prec, val), res in zip(cases, out):
        if direction == 'in':
            v = val
            sign = 1 if v < 0 else 0
            r, flags = round_frac(v, sign, FMT[PREC[prec]], mode)
            se, m = x_encode(r, sign) if v != 0 else x_encode(Fraction(0), 0)
            gse, gm, fpsr = int(res[0], 16), int(res[1], 16), int(res[2], 16)
            if v == 0 and kind in 'SD':
                se = gse & 0x8000                          # signed zero follows the input's sign bit
            ok = (gse, gm) == (se, m) and (fpsr & 0xFF00) == flags
            if not ok:
                failures.append(f'FMOVE.{kind} in {v} mode {mode} prec {prec}: got {gse:04X}:{gm:016X} {fpsr:08X}, '
                                f'want {se:04X}:{m:016X} exc {flags:04X}')
            nconv += 1
            continue
        d = x_decode(val[0], val[1])
        v = d[1]
        sign = val[0] >> 15
        got = int(res[0], 16)
        fpsr = int(res[3], 16)
        if kind in 'LWB':
            bits = {'L': 32, 'W': 16, 'B': 8}[kind]
            a_ = abs(v)
            fl = a_.numerator // a_.denominator
            rem = a_ - fl
            flags = 0
            if rem:
                flags |= INEX2
                up = (mode == RN and (rem > Fraction(1, 2) or (rem == Fraction(1, 2) and fl & 1))) or \
                     (mode == RM and sign) or (mode == RP and not sign)
                fl += 1 if up else 0
            iv = -fl if sign else fl
            lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
            if iv < lo or iv > hi:
                flags = (flags & ~INEX2) | OPERR
                iv = lo if sign else hi
                want = iv & ((1 << bits) - 1)
                ok = got == want and (fpsr & OPERR)
            else:
                want = iv & ((1 << bits) - 1)
                ok = got == want and (fpsr & 0xFF00) == flags
        else:
            fmt = FMT[kind]
            r, flags = round_frac(v, sign, fmt, mode)
            if kind == 'X':
                se, m = x_encode(r, sign)
                want = (se << 16, m >> 32, m & 0xFFFFFFFF)
                got3 = (int(res[0], 16), int(res[1], 16), int(res[2], 16))
                ok = got3 == want and (fpsr & 0xFF00) == flags
                got = got3
            else:
                bits, emin, emax = fmt
                ebits = 8 if kind == 'S' else 11
                bias = (1 << (ebits - 1)) - 1
                if r == 'inf':
                    enc = ((1 << ebits) - 1) << (bits - 1)
                elif r == 0:
                    enc = 0
                else:
                    e = flog2(r)
                    if e < emin:
                        enc = int(r / Fraction(2) ** (emin - bits + 1))
                    else:
                        enc = ((e + bias) << (bits - 1)) | (int(r / Fraction(2) ** (e - bits + 1)) - (1 << (bits - 1)))
                enc |= sign << (bits - 1 + ebits)
                if kind == 'D':
                    want = (enc >> 32, enc & 0xFFFFFFFF)
                    got = (int(res[0], 16), int(res[1], 16))
                else:
                    want = enc
                ok = got == want and (fpsr & 0xFF00) == flags
        if not ok:
            failures.append(f'FMOVE.{kind} out {val[0]:04X}:{val[1]:016X} mode {mode}: got {got} {fpsr:08X}, want {want}')
        nconv += 1
    report.append(f'conversions in and out (S, D, X, L, W, B): {nconv} cases exact')

    # 3. transcendental functions: within 1 ulp of the correctly rounded value
    funcs = {
        'sinh': mpmath.sinh, 'log1p': mpmath.log1p, 'expm1': mpmath.expm1, 'tanh': mpmath.tanh, 'atan': mpmath.atan,
        'asin': mpmath.asin, 'atanh': mpmath.atanh, 'sin': mpmath.sin, 'tan': mpmath.tan, 'exp': mpmath.exp,
        'exp2': lambda x: mpmath.power(2, x), 'exp10': lambda x: mpmath.power(10, x), 'log': mpmath.log,
        'log10': mpmath.log10, 'log2': lambda x: mpmath.log(x, 2), 'cosh': mpmath.cosh, 'acos': mpmath.acos,
        'cos': mpmath.cos}
    lines, cases = [], []
    for op, name in TRANS.items():
        for i in range(a.cases // 6):
            mode = RN if i % 2 == 0 else rng.randrange(4)
            s = rng.randrange(2)
            if name in ('asin', 'acos', 'atanh'):
                e = 16383 - rng.randrange(1, 40)
            elif name in ('log', 'log10', 'log2'):
                s = 0
                e = 16383 + rng.randrange(-2000, 2000)
            elif name == 'log1p':
                e = 16383 + rng.randrange(-60, 20)
                if s:
                    e = 16383 - rng.randrange(1, 60)
            elif name in ('exp', 'expm1', 'exp2', 'exp10', 'sinh', 'cosh'):
                e = 16383 + rng.randrange(-70, 12)
            elif name in ('sin', 'cos', 'tan'):
                e = 16383 + (rng.randrange(-70, 70) if i % 5 else rng.randrange(70, 16383))
            else:
                e = 16383 + rng.randrange(-70, 70)
            m = (1 << 63) | rng.getrandbits(63)
            x = ((s << 15) | e, m)
            cases.append((op, name, mode, x))
            lines.append(f'G {op:X} {fpcr(mode):X} 3FFF 8000000000000000 {x[0]:X} {x[1]:X}')
    out = Fpu().batch(lines)
    worst = {}
    for (op, name, mode, x), res in zip(cases, out):
        v = x_decode(x[0], x[1])[1]
        if name == 'exp10' and abs(v) > 4930:
            continue
        ref = funcs[name](mpmath.mpf(v.numerator) / v.denominator)
        if mpmath.im(ref) != 0 if isinstance(ref, mpmath.mpc) else False:
            continue
        rf = mpf_frac(ref)
        # Where the function never reaches the value 256 bits rounded it to
        # (tanh below 1 in magnitude, expm1 above -1), the true value lies
        # just inside it.
        if name == 'tanh' and abs(rf) == 1:
            rf -= Fraction(1 if rf > 0 else -1, 2 ** 2000)
        if name == 'expm1' and rf == -1:
            rf += Fraction(1, 2 ** 2000)
        sign = 1 if rf < 0 else 0
        r, _ = round_frac(rf, sign, FMT['X'], mode)
        se, m = x_encode(r, sign)
        gse, gm = int(res[0], 16), int(res[1], 16)
        if r == 'inf' or (gse & 0x7FFF) == 0x7FFF:
            ok = (gse, gm) == (se, m)
            u = 0 if ok else 99
        else:
            u = ulps(gse, gm, se, m) if (gse ^ se) & 0x8000 == 0 else 99
        worst[name] = max(worst.get(name, 0), u)
        if u > 1:
            failures.append(f'{name}({x[0]:04X}:{x[1]:016X}) mode {mode}: {gse:04X}:{gm:016X}, '
                            f'correctly rounded {se:04X}:{m:016X} ({u} ulp)')
    report.append('transcendentals, maximum error in ulps (1 allowed): ' +
                  ', '.join(f'{k} {v}' for k, v in sorted(worst.items())))

    # 4. FMOVECR in every mode and precision
    consts = {0x00: mpmath.pi, 0x0B: mpmath.log10(2), 0x0C: mpmath.e, 0x0D: 1 / mpmath.log(2), 0x0E: mpmath.log10(mpmath.e),
              0x30: mpmath.log(2), 0x31: mpmath.log(10)}
    for i, n in enumerate([0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096]):
        consts[0x32 + i] = mpmath.mpf(10) ** n
    lines, cases = [], []
    for off in consts:
        for mode in range(4):
            for prec in range(3):
                cases.append((off, mode, prec))
                lines.append(f'C {off:X} {fpcr(mode, prec):X}')
    out = Fpu().batch(lines)
    for (off, mode, prec), res in zip(cases, out):
        rf = mpf_frac(consts[off])
        r, flags = round_frac(rf, 0, FMT[PREC[prec]], mode)
        se, m = x_encode(r, 0)
        gse, gm, fpsr = int(res[0], 16), int(res[1], 16), int(res[2], 16)
        if (gse, gm) != (se, m) or (fpsr & INEX2) != (flags & INEX2):
            failures.append(f'FMOVECR ${off:02X} mode {mode} prec {prec}: {gse:04X}:{gm:016X} {fpsr:08X}, '
                            f'want {se:04X}:{m:016X} inex {flags & INEX2:X}')
    report.append(f'FMOVECR: {len(cases)} constants x modes x precisions exact')

    # 5. packed decimal
    import decimal
    lines, cases = [], []
    for _ in range(a.cases // 2):
        digits = [rng.randrange(10) for _ in range(17)]
        if digits[0] == 0:
            digits[0] = 1
        ex = rng.randrange(0, 1000) if rng.random() < 0.2 else rng.randrange(0, 60)
        sm, se = rng.randrange(2), rng.randrange(2)
        w0 = sm << 31 | se << 30 | (ex // 100) << 24 | (ex // 10 % 10) << 20 | (ex % 10) << 16 | digits[0]
        w1 = int(''.join(map(str, digits[1:9])), 16)
        w2 = int(''.join(map(str, digits[9:17])), 16)
        mode = rng.randrange(4)
        cases.append(('in', digits, ex * (-1 if se else 1), sm, mode))
        lines.append(f'I 3 {fpcr(mode):X} {w0:X} {w1:X} {w2:X}')
        x = rand_x(rng, wide=False)
        k = rng.randrange(-8, 18)
        cases.append(('out', x, k, mode))
        lines.append(f'O 3 {k & 0x7F:X} {fpcr(mode):X} {x[0]:X} {x[1]:X}')
    out = Fpu().batch(lines)
    worst_in = 0
    npk = 0
    for c, res in zip(cases, out):
        if c[0] == 'in':
            _, digits, ex, sm, mode = c
            v = Fraction(int(''.join(map(str, digits)))) * Fraction(10) ** (ex - 16)
            r, flags = round_frac(v, sm, FMT['X'], mode)
            se, m = x_encode(r, sm)
            gse, gm, fpsr = int(res[0], 16), int(res[1], 16), int(res[2], 16)
            u = ulps(gse, gm, se, m) if not (gse ^ se) & 0x8000 else 99
            worst_in = max(worst_in, u)
            if u > 1 or (u == 0 and bool(fpsr & INEX1) != bool(flags & INEX2)):
                failures.append(f'FMOVE.P in {digits} e{ex} mode {mode}: {gse:04X}:{gm:016X} {fpsr:08X}, want {se:04X}:{m:016X}')
            npk += 1
            continue
        _, x, k, mode = c
        v = x_decode(x[0], x[1])[1]
        sign = 1 if v < 0 else 0
        av = abs(v)
        ctx = decimal.Context(prec=10000, rounding={RN: decimal.ROUND_HALF_EVEN, RZ: decimal.ROUND_DOWN,
                                                     RM: decimal.ROUND_CEILING if sign else decimal.ROUND_FLOOR,
                                                     RP: decimal.ROUND_FLOOR if sign else decimal.ROUND_CEILING}[mode])
        dv = ctx.divide(decimal.Decimal(av.numerator), decimal.Decimal(av.denominator))
        ilog = dv.adjusted()
        length = k if k > 0 else ilog + 1 - k
        length = max(1, min(17, length))
        q = ctx.quantize(dv, decimal.Decimal(1).scaleb(ilog - length + 1))
        if q.adjusted() > ilog:
            ilog += 1
            if k <= 0 and length < 17:
                length += 1
            q = ctx.quantize(dv, decimal.Decimal(1).scaleb(ilog - length + 1))
        dig = str(q.scaleb(length - 1 - ilog).to_integral_exact()).zfill(length)
        dig = (dig + '0' * 17)[:17]
        ae = abs(ilog)
        w0 = sign << 31 | (1 << 30 if ilog < 0 else 0) | (ae // 100 % 10) << 24 | (ae // 10 % 10) << 20 | (ae % 10) << 16 \
            | int(dig[0])
        if ae > 999:
            w0 |= (ae // 1000 % 10) << 12          # EXP3, with OPERR (MC68881 UM 4.3.3)
        want = (w0, int(dig[1:9], 16), int(dig[9:17], 16))
        got = (int(res[0], 16), int(res[1], 16), int(res[2], 16))
        if got != want:
            failures.append(f'FMOVE.P out {x[0]:04X}:{x[1]:016X} k {k} mode {mode}: '
                            f'{got[0]:08X} {got[1]:08X} {got[2]:08X}, want {want[0]:08X} {want[1]:08X} {want[2]:08X}')
        npk += 1
    report.append(f'packed decimal: {npk} conversions; decimal to extended within {worst_in} ulp, '
                  f'extended to decimal digit-exact')

    print(f'fpu_suite: seed {a.seed}')
    for r in report:
        print('  ' + r)
    if failures:
        print(f'  {len(failures)} MISMATCH(ES)')
        for f in failures[:a.show * 10]:
            print('    ' + f)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
