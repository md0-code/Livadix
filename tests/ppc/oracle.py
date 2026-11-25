"""Original manual-derived programs; an installed qemu-ppc is a black box only.

MPC601UM chapters 2/3/10 and PEM chapters 2/4 define the encodings and state.
The wrapper is an ELF32 MSB executable using the Linux PPC write/exit ABI.
No cross compiler, emulator source, guest ROM, network or dependencies used.
601-only POWER instructions and 601 range-exception FPSCR differences are
covered by the manual tests, not compared against the later QEMU 604 model.
QEMU 10.2.1/604 also clears FR on an incremented negative quotient (the
exact rational fixture is in test_fpu.c). Only that status bit is excluded
from QEMU comparison, never from the manual-derived rounding tests.
"""
import argparse
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile

CODE = 0x11000
DATA = 0x20000


def d(op, target, base, immediate):
    return op << 26 | target << 21 | base << 16 | (immediate & 65535)


def x(xo, target, a, b, record=False):
    return 31 << 26 | target << 21 | a << 16 | b << 11 | xo << 1 | int(record)


def spr(xo, reg, number):
    return x(xo, reg, number & 31, number >> 5)


def const(reg, value):
    return [d(15, reg, 0, value >> 16), d(24, reg, reg, value)]


def fp(xo, target, a=0, b=0, c=0, record=False, primary=63):
    return primary << 26 | target << 21 | a << 16 | b << 11 | c << 6 | xo << 1 | int(record)


def words(values):
    return struct.pack(f'>{len(values)}I', *values)


def program(rng, count, rounding, model='601'):
    code = const(1, DATA)
    for reg in range(32):
        bits = rng.getrandbits(52) | rng.randrange(1013, 1034) << 52 | rng.getrandbits(1) << 63
        code += const(0, bits >> 32) + [d(36, 0, 1, 0)]
        code += const(0, bits & 0xffffffff) + [d(36, 0, 1, 4), d(50, reg, 1, 0)]
    code += [d(14, 0, 0, 0), spr(467, 0, 1), spr(467, 0, 8), spr(467, 0, 9),
             x(144, 0, 0, 0) | 255 << 12, fp(134, 28, b=rounding << 1)]
    for reg in range(32):
        if reg != 1:
            code += const(reg, rng.getrandbits(32))
    regs = [r for r in range(32) if r != 1]
    for _ in range(count):
        target, a, b = rng.choices(regs, k=3)
        record = bool(rng.getrandbits(1))
        group = rng.randrange(10)
        if group == 0:
            code.append(d(rng.choice([7, 8, 14, 15]), target, a, rng.getrandbits(16)))
        elif group == 1:
            code.append(d(rng.choice([24, 25, 26, 27, 28, 29]), a, target, rng.getrandbits(16)))
        elif group == 2:
            xo = rng.choice([8, 10, 40, 104, 136, 138, 200, 202, 232, 234, 235, 266])
            code.append(x(xo + (512 if rng.getrandbits(1) else 0), target, a, 0 if xo in (104, 200, 202, 232, 234) else b, record))
        elif group == 3:
            code.append(x(rng.choice([11, 75, 24, 26, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 922, 954]), target, a, b, record))
        elif group == 4:
            op = rng.choice([20, 21, 23])
            code.append(op << 26 | target << 21 | a << 16 | b << 11 | rng.randrange(32) << 6 | rng.randrange(32) << 1 | int(record))
        elif group == 5:
            offset = rng.randrange(0, 128) * 4
            code.append(d(36, target, 1, offset))
            code.append(d(rng.choice([32, 34, 40, 42]), a, 1, offset))
        elif group == 6:
            field = rng.randrange(8)
            code.append(x(rng.choice([0, 32]), field << 2, a, b))
            code += [16 << 26 | rng.choice([4, 12, 16, 18, 20]) << 21 | rng.randrange(32) << 16 | 8 | int(record), d(26, target, target, rng.getrandbits(16))]
        elif group == 7:
            code += [18 << 26 | 8 | int(record), d(14, target, target, 1)]
        else:
            target, a, b, c = rng.choices(range(32), k=4)
            xo = rng.choice([18, 20, 21, 25, 28, 29, 30, 31] + ([23] if model != '601' else []))
            code.append(fp(xo, target, a, 0 if xo == 25 else b, c if xo >= 25 or xo == 23 else 0, record))
    code += [spr(339, 30, 8), spr(339, 31, 9)]
    return code


def tail():
    code = [d(36, reg, 1, reg * 4) for reg in range(32)]
    code += [x(19, 0, 0, 0), d(36, 0, 1, 128), spr(339, 0, 1)]
    code += const(2, 0xe000007f) + [x(28, 0, 0, 2), d(36, 0, 1, 132)]
    code += [d(54, reg, 1, 136 + reg * 8) for reg in range(32)]
    code += [fp(583, 0), d(54, 0, 1, 408), d(32, 0, 1, 412), d(36, 0, 1, 392)]
    code += [d(14, 0, 0, 4), d(14, 3, 0, 1), d(24, 1, 4, 0), d(14, 5, 0, 396), 0x44000002,
             d(14, 0, 0, 1), d(14, 3, 0, 0), 0x44000002]
    return code


def elf(code):
    payload = words(code)
    ident = b'\x7fELF\x01\x02\x01' + bytes(9)
    header = ident + struct.pack('>HHIIIIIHHHHHH', 2, 20, 1, CODE, 52, 0, 0, 52, 32, 2, 0, 0, 0)
    segments = struct.pack('>8I', 1, 4096, CODE, CODE, len(payload), len(payload), 5, 4096)
    segments += struct.pack('>8I', 1, 0, DATA, DATA, 0, 4096, 6, 4096)
    return (header + segments).ljust(4096, b'\0') + payload


def linux_path(path):
    path = Path(path).resolve()
    if sys.platform != 'win32':
        return str(path)
    return '/mnt/' + path.drive[0].lower() + '/' + '/'.join(path.parts[1:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--livadix', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--qemu', default='/usr/local/bin/qemu-ppc')
    parser.add_argument('--model', choices=['601', '603', '603e', '604', '604e', '750'], default='601')
    parser.add_argument('--distribution', default='Debian')
    parser.add_argument('--programs', type=int, default=64)
    parser.add_argument('--instructions', type=int, default=128)
    parser.add_argument('--seed', type=int, default=601)
    args = parser.parse_args()
    if args.programs < 1 or args.instructions < 1:
        parser.error('program and instruction counts must be positive')
    launch =  ['wsl', '-d', args.distribution, '--', args.qemu] if sys.platform == 'win32' else [args.qemu]
    try:
        version = subprocess.run(launch + ['-version'], capture_output=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        print('SKIP: installed qemu-ppc oracle unavailable')
        return 77
    if version.returncode:
        print('SKIP: installed qemu-ppc oracle unavailable')
        return 77
    print(version.stdout.decode(errors='replace').splitlines()[0])
    args.work.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    fr_differences = 0
    with tempfile.TemporaryDirectory(prefix='ppc-oracle-', dir=args.work) as temporary:
        raw = Path(temporary) / 'program.bin'
        guest = Path(temporary) / 'program.elf'
        for index in range(args.programs):
            code = program(rng, args.instructions, index & 3, args.model)
            raw.write_bytes(words(code))
            guest.write_bytes(elf(code + tail()))
            native = subprocess.run([str(args.livadix.resolve()), str(raw), '1000000', args.model], capture_output=True, timeout=15)
            oracle_model = '604' if args.model == '601' else args.model
            oracle = subprocess.run(launch + ['-cpu', oracle_model, linux_path(guest)], capture_output=True, timeout=15)
            if native.returncode or oracle.returncode or len(native.stdout) != 396 or len(oracle.stdout) != 396:
                print(f'FAIL program {index}: Livadix={native.returncode}, QEMU={oracle.returncode}, sizes={len(native.stdout)}/{len(oracle.stdout)}')
                print(native.stderr.decode(errors='replace'), oracle.stderr.decode(errors='replace'))
                shutil.copyfile(raw, args.work / 'ppc-oracle-failed.bin')
                shutil.copyfile(guest, args.work / 'ppc-oracle-failed.elf')
                return 1
            got, expected = struct.unpack('>99I', native.stdout), struct.unpack('>99I', oracle.stdout)
            fr_differences += int(bool((got[-1] ^ expected[-1]) & 0x40000))
            differences = [(i, a, b) for i, (a, b) in enumerate(zip(got, expected))
                           if (a ^ b) & (0xfffbffff if i == 98 else 0xffffffff)]
            if differences:
                print(f'FAIL seed={args.seed} program={index} rounding={index & 3}: {differences}')
                shutil.copyfile(raw, args.work / 'ppc-oracle-failed.bin')
                shutil.copyfile(guest, args.work / 'ppc-oracle-failed.elf')
                return 1
    print(f'PASS {args.model}: {args.programs} programs, {args.instructions} generated operations each; GPR/CR/XER/FPR exact, FPSCR except oracle FR')
    print(f'FR-only oracle discrepancies: {fr_differences}; FR checked by manual/rational fixtures')
    return 0


if __name__ == '__main__':
    sys.exit(main())
