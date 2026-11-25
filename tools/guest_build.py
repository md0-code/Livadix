"""Builds Livadix's guest code with the GNU assembler.

    python tools/guest_build.py fetch   get binutils for m68k and PowerPC into
                                        work/tools/binutils
    python tools/guest_build.py build   assemble src/hw/guest/drivers/*.s and
                                        probes into src/hw/guest/bin/*.bin,
                                        build each declaration ROM
                                        src/hw/guest/declrom/*.yaml (tools/declrom.py),
                                        assemble the PCI ndrv and build each PCI
                                        expansion ROM src/hw/guest/pci/*.yaml
                                        (tools/pcirom.py)
    python tools/guest_build.py check   build into work/guest and compare with the
                                        committed images (exit 77 when no assembler
                                        is available)

The assemblers are GNU as for m68k and PowerPC (binutils, GPL; its output is
not encumbered). They are looked for as:
  1. LIVADIX_M68K_AS / LIVADIX_M68K_OBJCOPY and LIVADIX_PPC_AS / LIVADIX_PPC_OBJCOPY, or
     m68k-elf-as / m68k-linux-gnu-as and powerpc-linux-gnu-as with matching
     objcopy on PATH;
  2. the Debian packages binutils-m68k-linux-gnu and
     binutils-powerpc-linux-gnu unpacked under work/tools/binutils by `fetch`
     and run in WSL (no installation, no administrator rights: apt-get
     download and dpkg -x as the user).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / 'src' / 'hw' / 'guest' / 'drivers'
PROBES = ROOT / 'src' / 'hw' / 'guest' / 'probes'    # test programs for the 68040 (tests/verify.py)
DECLROM = ROOT / 'src' / 'hw' / 'guest' / 'declrom'  # code of the Livadix Display Card's declaration ROM
PCI = ROOT / 'src' / 'hw' / 'guest' / 'pci'          # PowerPC ndrv and PCI expansion ROM
BIN = ROOT / 'src' / 'hw' / 'guest' / 'bin'
WORK = ROOT / 'work' / 'guest'
TOOLS = ROOT / 'work' / 'tools' / 'binutils'
PACKAGE_M68K = 'binutils-m68k-linux-gnu'
PACKAGE_PPC = 'binutils-powerpc-linux-gnu'
SKIP = 77


def wsl_path(p: Path) -> str:
    p = p.resolve()
    drive = p.drive.rstrip(':').lower()
    return f'/mnt/{drive}' + p.as_posix()[len(p.drive):]


def have_wsl() -> bool:
    return shutil.which('wsl') is not None


def wsl(command: str, timeout=300) -> subprocess.CompletedProcess:
    return subprocess.run(['wsl', '-e', 'sh', '-c', command], capture_output=True, text=True, timeout=timeout)


class Assembler:
    """Runs as + objcopy natively or in WSL for one target triple."""

    def __init__(self, prefixes: tuple[str, ...], env_as: str, env_oc: str, package: str, as_name: str):
        self.native = None
        self.wsl_root = None
        self.package = package
        self.as_name = as_name
        a_env, o_env = os.environ.get(env_as), os.environ.get(env_oc)
        if a_env and o_env:
            self.native = (a_env, o_env)
            return
        for prefix in prefixes:
            a, o = shutil.which(prefix + 'as'), shutil.which(prefix + 'objcopy')
            if a and o:
                self.native = (a, o)
                return
        exe = TOOLS / 'root' / 'usr' / 'bin' / as_name
        if exe.exists() and have_wsl():
            self.wsl_root = TOOLS / 'root'

    def available(self) -> bool:
        return bool(self.native or self.wsl_root)

    def describe(self) -> str:
        if self.native:
            return self.native[0]
        return f'{self.package} in WSL ({TOOLS})'

    def build(self, source: Path, out: Path, as_flags: list[str]):
        out.parent.mkdir(parents=True, exist_ok=True)
        obj = WORK / (source.stem + '.o')
        WORK.mkdir(parents=True, exist_ok=True)
        if self.native:
            a, o = self.native
            subprocess.run([a, *as_flags, f'-I{source.parent}', '-o', str(obj), str(source)],
                           check=True, capture_output=True, text=True)
            subprocess.run([o, '-O', 'binary', str(obj), str(out)], check=True, capture_output=True, text=True)
            return
        root = wsl_path(self.wsl_root)
        lib = f'{root}/usr/lib/x86_64-linux-gnu'
        flags = ' '.join(as_flags)
        cmd = (f'LD_LIBRARY_PATH={lib} {root}/usr/bin/{self.as_name} {flags} '
               f'-I{wsl_path(source.parent)} -o {wsl_path(obj)} {wsl_path(source)} && '
               f'LD_LIBRARY_PATH={lib} {root}/usr/bin/{self.as_name.replace("-as", "-objcopy")} '
               f'-O binary {wsl_path(obj)} {wsl_path(out)}')
        p = wsl(cmd)
        if p.returncode != 0:
            raise RuntimeError(f'{source.name}:\n{p.stdout}{p.stderr}')


def m68k_assembler() -> Assembler:
    return Assembler(('m68k-elf-', 'm68k-linux-gnu-'), 'LIVADIX_M68K_AS', 'LIVADIX_M68K_OBJCOPY',
                     PACKAGE_M68K, 'm68k-linux-gnu-as')


def ppc_assembler() -> Assembler:
    return Assembler(('powerpc-linux-gnu-', 'powerpc-eabi-'), 'LIVADIX_PPC_AS', 'LIVADIX_PPC_OBJCOPY',
                     PACKAGE_PPC, 'powerpc-linux-gnu-as')


def fetch() -> int:
    if not have_wsl():
        print('WSL is not available: install binutils for m68k and PowerPC and set '
              'LIVADIX_M68K_AS / LIVADIX_M68K_OBJCOPY and LIVADIX_PPC_AS / LIVADIX_PPC_OBJCOPY', file=sys.stderr)
        return 1
    TOOLS.mkdir(parents=True, exist_ok=True)
    folder = wsl_path(TOOLS)
    for package in (PACKAGE_M68K, PACKAGE_PPC):
        p = wsl(f'cd {folder} && rm -f {package}_*.deb && apt-get download {package} && '
                f'dpkg -x {package}_*.deb root')
        if p.returncode != 0:
            print(p.stdout + p.stderr, file=sys.stderr)
            return 1
        print(f'{package} unpacked in {TOOLS}')
    return 0


def m68k_sources():
    """(source, cpu): the drivers for any 68k, the probes for the 68040, the
    declaration ROMs' code for the 68020 and later (NuBus machines)."""
    return ([(s, '68000') for s in sorted(DRIVERS.glob('*.s'))] +
            [(s, '68040') for s in sorted(PROBES.glob('*.s'))] +
            [(s, '68020') for s in sorted(DECLROM.glob('*.s'))])


def m68k_sources_before_declrom():
    """68k guest code that does not .incbin PowerPC blobs (built before ppc)."""
    return ([(s, '68000') for s in sorted(DRIVERS.glob('*.s'))] +
            [(s, '68040') for s in sorted(PROBES.glob('*.s'))])


def m68k_declrom_sources():
    return [(s, '68020') for s in sorted(DECLROM.glob('*.s'))]


def ppc_sources():
    """The PowerPC sources, the native QuickDraw patch first: its fragment
    (fragments) is carried by the native driver and the declaration ROM's
    SecondaryInit (.incbin), assembled after it."""
    return sorted(PCI.glob('*.s'), key=lambda s: (s.stem != 'livadixcard_accel_ppc', s.name))


def is_fragment(source: Path) -> bool:
    import yaml
    with open(source, encoding='utf-8') as f:
        return 'fragment' in yaml.safe_load(f)


def fragments(bins: Path, out: Path) -> list:
    """Builds the PEF code fragments src/hw/guest/pci/*.yaml with a
    `fragment` (tools/pcirom.py) from the code in `bins` into `out`."""
    sys.path.insert(0, str(ROOT / 'tools'))
    import pcirom
    import yaml
    built = []
    for source in sorted(PCI.glob('*.yaml')):
        if not is_fragment(source):
            continue
        with open(source, encoding='utf-8') as f:
            image = pcirom.build(yaml.safe_load(f), bins)
        target = out / (source.stem + '.bin')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(image)
        built.append(target)
    return built


def declroms(bins: Path, out: Path) -> list:
    """Builds the declaration ROMs from the code in `bins` into `out`."""
    sys.path.insert(0, str(ROOT / 'tools'))
    import declrom
    import yaml
    built = []
    for source in sorted(DECLROM.glob('*.yaml')):
        with open(source, encoding='utf-8') as f:
            image = declrom.build(yaml.safe_load(f), bins)
        target = out / (source.stem + '.bin')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(image)
        built.append(target)
    return built


def pciroms(bins: Path, out: Path) -> list:
    """Builds the PCI expansion ROMs from the ndrv in `bins` into `out`."""
    sys.path.insert(0, str(ROOT / 'tools'))
    import pcirom
    import yaml
    built = []
    for source in sorted(PCI.glob('*.yaml')):
        if is_fragment(source):
            continue
        with open(source, encoding='utf-8') as f:
            image = pcirom.build(yaml.safe_load(f), bins)
        target = out / (source.stem + '.bin')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(image)
        built.append(target)
    return built


def build() -> int:
    m68k = m68k_assembler()
    ppc = ppc_assembler()
    if not m68k.available():
        print('no m68k assembler: run `python tools/guest_build.py fetch`', file=sys.stderr)
        return 1
    if not ppc.available():
        print('no PowerPC assembler: run `python tools/guest_build.py fetch`', file=sys.stderr)
        return 1
    for s, cpu in m68k_sources_before_declrom():
        out = BIN / (s.stem + '.bin')
        m68k.build(s, out, [f'-m{cpu}'])
        print(f'{out.relative_to(ROOT).as_posix()}: {out.stat().st_size} bytes')
    for s in ppc_sources():
        out = BIN / (s.stem + '.bin')
        ppc.build(s, out, ['-mbig', '-mppc32', '-mregnames'])
        print(f'{out.relative_to(ROOT).as_posix()}: {out.stat().st_size} bytes')
        if s.stem == 'livadixcard_accel_ppc':
            for f in fragments(BIN, BIN):
                print(f'{f.relative_to(ROOT).as_posix()}: {f.stat().st_size} bytes (code fragment)')
    for s, cpu in m68k_declrom_sources():
        out = BIN / (s.stem + '.bin')
        m68k.build(s, out, [f'-m{cpu}'])
        print(f'{out.relative_to(ROOT).as_posix()}: {out.stat().st_size} bytes')
    for out in declroms(BIN, BIN):
        print(f'{out.relative_to(ROOT).as_posix()}: {out.stat().st_size} bytes (declaration ROM)')
    for out in pciroms(BIN, BIN):
        print(f'{out.relative_to(ROOT).as_posix()}: {out.stat().st_size} bytes (PCI expansion ROM)')
    return 0


def check() -> int:
    m68k = m68k_assembler()
    ppc = ppc_assembler()
    if not m68k.available() or not ppc.available():
        print('no guest assembler (python tools/guest_build.py fetch)')
        return SKIP
    stale = []
    for s, cpu in m68k_sources_before_declrom():
        fresh = WORK / (s.stem + '.bin')
        m68k.build(s, fresh, [f'-m{cpu}'])
        committed = BIN / (s.stem + '.bin')
        if not committed.exists() or committed.read_bytes() != fresh.read_bytes():
            stale.append(committed.relative_to(ROOT).as_posix())
    for s in ppc_sources():
        fresh = WORK / (s.stem + '.bin')
        ppc.build(s, fresh, ['-mbig', '-mppc32', '-mregnames'])
        committed = BIN / (s.stem + '.bin')
        if not committed.exists() or committed.read_bytes() != fresh.read_bytes():
            stale.append(committed.relative_to(ROOT).as_posix())
    for fresh in fragments(WORK, WORK / 'fragment'):
        committed = BIN / fresh.name
        if not committed.exists() or committed.read_bytes() != fresh.read_bytes():
            stale.append(committed.relative_to(ROOT).as_posix())
    for s, cpu in m68k_declrom_sources():
        fresh = WORK / (s.stem + '.bin')
        m68k.build(s, fresh, [f'-m{cpu}'])
        committed = BIN / (s.stem + '.bin')
        if not committed.exists() or committed.read_bytes() != fresh.read_bytes():
            stale.append(committed.relative_to(ROOT).as_posix())
    for fresh in declroms(WORK, WORK / 'declrom'):
        committed = BIN / fresh.name
        if not committed.exists() or committed.read_bytes() != fresh.read_bytes():
            stale.append(committed.relative_to(ROOT).as_posix())
    for fresh in pciroms(WORK, WORK / 'pcirom'):
        committed = BIN / fresh.name
        if not committed.exists() or committed.read_bytes() != fresh.read_bytes():
            stale.append(committed.relative_to(ROOT).as_posix())
    n = len(m68k_sources()) + len(list(DECLROM.glob('*.yaml'))) + len(ppc_sources()) + len(list(PCI.glob('*.yaml')))
    if stale:
        print('out of date (python tools/guest_build.py build): ' + ', '.join(stale))
        return 1
    print(f'{n} guest image(s) match their sources ({m68k.describe()}; {ppc.describe()})')
    return 0


def main(argv) -> int:
    commands = {'fetch': fetch, 'build': build, 'check': check}
    if len(argv) != 1 or argv[0] not in commands:
        print(__doc__)
        return 2
    return commands[argv[0]]()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
