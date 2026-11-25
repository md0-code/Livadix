"""Livadix's gates, beside ctest (tools/build.ps1).

    python tests/verify.py            the quick tier: everyday work and commits
    python tests/verify.py --full     every check in full (releases; tools/build_dist.ps1)
    python tests/verify.py --list     list the checks
    python tests/verify.py NAME...    run only these checks, in full
    python tests/verify.py --family quadra800,ppc_pippin    only these boot-matrix files
    python tests/verify.py --cell "Pippin retail"     only the cells whose name has this
    --jobs N                          processes in parallel (default: physical cores - 1)
    --timings FILE                    per-check, per-unit and per-cell times as JSON

Checks split into units (a boot-matrix chain of cells, a chime, a slice of a
suite) that run in a pool of worker processes, each in its own work folder;
the report keeps the order of the checks and units. Checks that time the host
(cpu-speed, the paced chime) run alone, after the rest. The quick tier
leaves out what ctest runs (rom-trace, pmu-diff, fpu-suite); it samples
cpu-suite, jit-diff and chime,
and runs the boot-matrix cells marked `quick`. No check uses an existing
disk image: the checks make their own media (tests/hfs_blank.py) under
work/.

Checks:
  version      src/VERSION and tools/version.py
  root         only the allowed files and folders in the repository's root
  rom-names    tools/rom_names.py on made-up ROM and firmware files
  roms         every ROM in roms/ identified by livadix.exe --inspect as its
               machine, with a valid checksum; every file of roms/ listed by
               --list-roms as what it holds (skipped without roms/)
  config       first start uses embedded defaults without writing files; bad files refused
               with their line; profiles: --list-profiles, the default: one,
               --profile NAME (any letter case, any character) and the shared
               keys a profile overrides
  config-window  livadix.exe hidden, the configuration window driven by a script
               over the stub machine: each tab drawn and saved as a BMP
               (work/verify/config-window), a look applied as it runs, memory
               by a new start, another profile started, Save read back by
               --inspect with the comments kept; a profile that cannot start
               shown in the window, and a hidden run of it failing as before
  screen       hidden-window runs of the stub machine (its hidden model
               `stub`) at every depth: the test pattern's colours
  looks        every look and monitor through the presenter (mc_look_shot)
  cpu-suite    the SingleStepTests 68000 suite on the 68040 core and, in
               full (bus cycles and clocks too), on the 68000 model (fetched
               into work/cpu/sst by tests/cpu_suite.py; skipped offline),
               interpreted and through the translator (one-instruction
               blocks): the same results
  jit-diff     the translator against the interpreter on random programs
               (mc_cpu_tests jit): every model, MMU on and off with invalid
               and write-protected pages, bus errors, self-modifying code,
               interrupts, slices of every length; the 68020 with an
               MC68851, and on a 24-bit decode switching modes
  fpu-suite    the FPU against exact rational arithmetic and mpmath
               (tests/fpu_suite.py; skipped without mpmath)
  rom-trace    roms/quadra-700.rom from reset in a bare map: reaches its first
               I/O access, and runs 1,000,000 instructions with I/O answered 0,
               without an opcode of a later step
  boot         the Quadra 700 ROM headless (mc_hw_trace) to its question-mark
               disk at 640x480 and 1152x870: the gray desktop and the disk
               icon (no Sad Mac), 1 bit on a fresh PRAM and 8 bits once the
               PRAM persisted, the clock (low memory Time) starting at the
               host's time on a fresh PRAM and keeping a saved adjustment, the ADB
               keyboard and mouse in the ADB Manager's table, the mouse moving
               the cursor
  os-boot      the boot matrix (tests/boot_matrix, one file per family):
               each cell a headless start of a model without a disk or with
               blank media (tests/hfs_blank.py): POST, chime, screen, the
               ROM's question-mark disk, Gestalt, PRAM. Prints the boot times
  chime        every model's startup chime (livadix.exe hidden, unthrottled, no
               disk, recorded to a WAV file; the models whose ROM is in roms/)
               against its clean recording: the output rate, its length, no
               samples at full scale (the AVs' own samples reach it), the
               energy of the sample steps against the signal's (distortion,
               dropped or repeated bytes and a wrong pitch raise it) and the
               largest step; the Quadra 700's also paced in real time
  cpu-speed    the interpreter on simple loops, with the MMU off and on
               (identity-mapped tables): at least 100 MIPS; the translator
               on the same loops
  ppc-jit-diff  the PowerPC translator against the interpreter on the
               PowerPC machine back end (mc_ppc_jit_tests): random programs
               on the 601, 603, 603e, 604, 604e and 750 (integer, branch,
               every load and store, loops, calls, self-modifying code by
               stores and DMA, device reads of guest time, interrupts, the
               time base and decrementer, user mode, floating point; real
               mode, BATs rewritten, the hashed page table rewritten, the
               603's software reload) in random slices, compared after each
               (CPU state, RAM, device, clocks, guest time), also in
               one-instruction blocks; and oracle.py's generated programs
  ppc-grackle  MPC106 manual fixtures and both G3 ROM prefixes to the
               first unanswered Heathrow write (not a Macintosh boot)
  ppc-jit-rom  roms/power-macintosh-6100.rom on the PDM board to its first hardware
               probe (11,010,265 instructions) by the interpreter and the
               translator: identical every million clocks; MIPS of each
               (skipped without the ROM)
  guest        the committed guest images (src/hw/guest/bin) match a rebuild of
               their sources with the m68k assembler (tools/guest_build.py;
               skipped without the assembler)
  declrom      the Livadix Display Card's declaration ROM walked by the
               independent parser (tests/declrom_check.py): format block,
               CRC, sResources, driver, video modes; every resolution and
               depth of docs/NOTES.md offered
  pmu-diff     the PowerBook PMU's protocol level against Apple's firmware
               (pmu-pb140.bin, pmu-pb160.bin) on the 740 core, on the same
               command sequences: identical byte streams, timing within
               tolerance (mc_test_pmu --diff; skipped without the firmware)

Everything is written under work/verify (git-ignored). roms/ and data/ are
only read, and the run fails if any file in them changed. Windows are hidden:
nothing opens on the desktop.
"""
from __future__ import annotations

import os
import random
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('LIVADIX_BUILD') or ROOT / 'build' / 'x64')   # tools/build_dist.ps1 names its release build
EXE = '.exe' if os.name == 'nt' else ''   # the programs of a Linux build (tools/build_linux.sh, LIVADIX_BUILD) have no suffix
LIVADIX = BUILD / ('livadix' + EXE)
LOOK_SHOT = BUILD / ('mc_look_shot' + EXE)
CPU_TESTS = BUILD / ('mc_cpu_tests' + EXE)
ROM_TRACE = BUILD / ('mc_rom_trace' + EXE)
HW_TRACE = BUILD / ('mc_hw_trace' + EXE)
MIN_MIPS = 100
WORK = ROOT / 'work' / 'verify'
# The user's ROMs and disk images: roms/ and data/ in the root, or the
# folders LIVADIX_ROMS and LIVADIX_DATA name (copies under work/ in a worktree).
ROMS = Path(os.environ.get('LIVADIX_ROMS') or ROOT / 'roms')
DATA = Path(os.environ.get('LIVADIX_DATA') or ROOT / 'data')
BOOT_MATRIX = ROOT / 'tests' / 'boot_matrix'
ROOT_FILES = {'README.md', 'AGENTS.md', 'PLAN.md', 'TODO.md', 'ISSUES.md', '.gitignore', '.gitattributes', '.git'}  # .git is a file in a git worktree
ROOT_DIRS = {'.git', '.claude', 'src', 'tests', 'tools', 'assets', 'docs', 'roms', 'data', 'work', 'build', 'dist', 'nvram'}


class Skip(Exception):
    pass


def run(args, cwd=None, check=True, timeout=120, input_text=None):
    p = subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True, text=True,
                       timeout=timeout, input=input_text)
    if check and p.returncode != 0:
        raise AssertionError(f'{" ".join(map(str, args))} exited {p.returncode}:\n{p.stdout}{p.stderr}')
    return p


def check_version():
    run([sys.executable, ROOT / 'tools' / 'version.py', 'check'])


def check_root():
    problems = []
    for entry in ROOT.iterdir():
        if entry.is_dir() and entry.name not in ROOT_DIRS:
            problems.append(f'unexpected folder in the root: {entry.name}')
        # livadix.yaml (and its PRAM in nvram/) appear when Livadix runs here; git-ignored.
        if entry.is_file() and entry.name not in ROOT_FILES and entry.name != 'livadix.yaml' and not entry.name.endswith('.nvram'):
            problems.append(f'unexpected file in the root: {entry.name}')
    if problems:
        raise AssertionError('\n'.join(problems))


def fake_rom(path: Path, checksum: int, size: int):
    path.write_bytes(struct.pack('>I', checksum) + bytes(size - 4))


def forced_crc(data: bytes, target: int) -> bytes:
    """data with its last four bytes set so that its CRC-32 is target (the
    register after them depends only on the table indices they select)."""
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = 0xEDB88320 ^ (c >> 1) if c & 1 else c >> 1
        table.append(c)
    t, index = target ^ 0xFFFFFFFF, [0] * 4
    for k in range(3, -1, -1):
        j = next(j for j in range(256) if table[j] >> 24 == t >> 24)
        index[k], t = j, ((t ^ table[j]) << 8) & 0xFFFFFFFF
    r = zlib.crc32(data[:-4]) ^ 0xFFFFFFFF
    tail = bytearray()
    for k in range(4):
        tail.append((r ^ index[k]) & 0xFF)
        r = table[index[k]] ^ (r >> 8)
    return data[:-4] + bytes(tail)


def check_rom_names():
    folder = WORK / 'rom-names'
    folder.mkdir(parents=True, exist_ok=True)
    for f in folder.iterdir():
        f.unlink()
    fake_rom(folder / 'Q700.BIN', 0x420DBFF3, 1 << 20)
    fake_rom(folder / 'copy of q700.bin', 0x420DBFF3, 1 << 20)
    fake_rom(folder / 'iici.rom', 0x368CADFE, 512 << 10)
    fake_rom(folder / 'mystery.bin', 0x12345678, 1 << 20)
    # A PowerPC ROM (4 MiB) under its old name.
    fake_rom(folder / 'pm-6100.rom', 0x9FEB69B3, 4 << 20)
    # Firmware by size and CRC32: the Egret 341S0851's.
    (folder / 'EGRET.BIN').write_bytes(forced_crc(bytes(range(256)) * 17, 0xEA9EA6E4))
    out = run([sys.executable, ROOT / 'tools' / 'rom_names.py', folder, '--apply']).stdout
    names = sorted(f.name for f in folder.iterdir())
    if len(names) != 6 or 'quadra-700.rom' not in names or 'iici.rom' not in names or 'mystery.bin' not in names \
            or 'egret-341s0851.bin' not in names or 'power-macintosh-6100.rom' not in names:
        raise AssertionError(f'after renaming: {names}\n{out}')
    # Whichever copy is renamed first, the other is reported and left alone.
    if ', skipped' not in out or 'duplicate of' not in out:
        raise AssertionError(out)
    if 'mystery.bin: unknown' not in out or '3 file(s) renamed' not in out:
        raise AssertionError(out)


def write_config(name: str, text: str) -> Path:
    WORK.mkdir(parents=True, exist_ok=True)
    path = WORK / name
    path.write_text(text, encoding='utf-8')
    return path


def posix(path: Path) -> str:
    return path.as_posix()


def check_roms():
    roms = sorted((ROMS).glob('*.rom')) if (ROMS).is_dir() else []
    if not roms:
        raise Skip('no ROMs in roms/')
    out = run([sys.executable, ROOT / 'tools' / 'rom_names.py', ROMS]).stdout
    for line in out.splitlines():
        name, _, note = line.partition(': ')
        # Files that are no Macintosh ROM (controller firmware, card
        # declaration ROMs) are not this check's.
        if note == 'unknown' and not name.lower().endswith('.rom'):
            continue
        if ': standard (' not in line:
            raise AssertionError(f'roms/ holds a file without its standard name: {line}')
    # The Macintosh ROMs: the New World "Mac OS ROM" files, also .rom, are no
    # machine's ROM (only listed).
    sys.path.insert(0, str(ROOT / 'tools'))
    import rom_names
    machine_roms = {name.lower() for key, (name, _) in rom_names.database().items() if key[0] == 'rom'}
    roms = [rom for rom in roms if rom.name.lower() in machine_roms]
    for rom in roms:
        config = write_config('rom.yaml', f"model: {rom.stem}\nemulation:\n  rom: '{posix(rom)}'\nstorage:\n  disks: []\n")
        out = run([LIVADIX, '--inspect', '--config', config]).stdout
        if '; valid;' not in out or f'Model: {rom.stem} (' not in out:
            raise AssertionError(f'{rom.name}:\n{out}')
    # --list-roms: every file of the configuration's roms folder, as what it holds.
    out = run([LIVADIX, '--list-roms', '--config', ROMS.parent / 'none.yaml']).stdout
    files = [f for f in ROMS.iterdir() if f.is_file()]
    listed = {line.strip().partition(': ')[0]: line.strip().partition(': ')[2] for line in out.splitlines() if line.startswith('  ')}
    for f in files:
        if f.name not in listed:
            raise AssertionError(f'--list-roms does not list {f.name}:\n{out}')
        if f.suffix.lower() in ('.rom', '.bin') and (listed[f.name] == 'unknown' or 'standard name' in listed[f.name]):
            raise AssertionError(f'--list-roms: {f.name}: {listed[f.name]}')
    print(f'    {len(roms)} ROMs identified; {len(files)} files listed by --list-roms')


def check_config():
    first = WORK / 'first-start'
    first.mkdir(parents=True, exist_ok=True)
    (first / 'livadix.yaml').unlink(missing_ok=True)
    run([LIVADIX, '--inspect'], cwd=first)
    if (first / 'livadix.yaml').exists():
        raise AssertionError('first start must use embedded defaults without writing livadix.yaml')
    for text, message in [('model: quadra-700\nemulation:\n  ram_mb: 128\n', 'maximum of 68 MiB'),
                          ('model: fast\n', 'Unknown model'),
                          ('video:\n  depth: 3\n', 'video.depth'),
                          ('storage:\n  floppies: [{}, {}, {}]\n', 'At most two floppy drives')]:
        p = run([LIVADIX, '--inspect', '--config', write_config('bad.yaml', text)], check=False)
        if p.returncode != 2 or message not in p.stderr:
            raise AssertionError(f'{text!r}: exit {p.returncode}, {p.stderr}')
    # Profiles: shared keys, each profile's own, the default and --profile.
    if (ROMS / 'quadra-700.rom').exists():
        name = 'Quadra 700 \u2013 System 7.1'
        config = write_config('profiles.yaml', f"default: Second\nemulation:\n  rom: '{posix(ROMS / 'quadra-700.rom')}'\n"
                              "  ram_mb: 8\nstorage:\n  disks: []\nprofiles:\n"
                              f"  \"{name}\":\n    model: quadra-700\n    emulation: {{ram_mb: 20}}\n"
                              "  Second:\n    model: quadra-900\n    video: {resolution: 640x480}\n")
        out = subprocess.run([str(LIVADIX), '--list-profiles', '--config', str(config)], capture_output=True,
                             timeout=120).stdout.decode('utf-8', 'replace')
        if '* Second' not in out or f'  {name} ' not in out:
            raise AssertionError(f'--list-profiles:\n{out}')
        out = run([LIVADIX, '--inspect', '--config', config]).stdout
        if 'Profile: Second' not in out or 'Emulation: 8 MiB' not in out or 'Video: 640x480' not in out:
            raise AssertionError(f'the default profile:\n{out}')
        p = subprocess.run([str(LIVADIX), '--inspect', '--config', str(config), '--profile', name.upper()],
                           capture_output=True, timeout=120)
        out = p.stdout.decode('utf-8', 'replace')
        if p.returncode != 0 or f'Profile: {name}' not in out or 'Emulation: 20 MiB' not in out or 'Video: 1152x870' not in out:
            raise AssertionError(f'--profile {name}: exit {p.returncode}\n{out}{p.stderr.decode("utf-8", "replace")}')
        p = run([LIVADIX, '--inspect', '--config', config, '--profile', 'Third'], check=False)
        if p.returncode != 2 or 'There is no profile "Third"' not in p.stderr:
            raise AssertionError(f'--profile Third: exit {p.returncode}, {p.stderr}')
        bad = write_config('bad.yaml', 'profiles:\n  A:\n    model: iici\n  B:\n    video:\n      depth: 3\n')
        p = run([LIVADIX, '--inspect', '--config', bad], check=False)
        if p.returncode != 2 or 'line 6 (profile "B"): video.depth' not in p.stderr:
            raise AssertionError(f'a bad profile: exit {p.returncode}, {p.stderr}')
    # The Built-in machine, the default: Livadix's embedded ROM, its desktop,
    # and the README opened by a double-click.
    folder = WORK / 'builtin'
    folder.mkdir(parents=True, exist_ok=True)
    config = folder / 'livadix.yaml'
    config.write_text('video: {look: plain}\n', encoding='utf-8')
    out = run([LIVADIX, '--inspect', '--config', config]).stdout
    if 'Model: builtin (Built-in)' not in out or 'ROM checksum: 4C564458 stored, 4C564458 computed; valid' not in out:
        raise AssertionError(f'the Built-in machine:\n{out}')
    script = folder / 'open.txt'
    script.write_text('frames 90\nmouse-to 470 50\nframes 5\n' + 'button 1\nframes 3\nbutton 0\nframes 3\n' * 2 + 'frames 30\n',
                      encoding='utf-8')
    shot = folder / 'readme.bmp'
    run([LIVADIX, '--hidden', '--config', config, '--frames', '200', '--script', script, '--screenshot', shot], cwd=folder)
    w, h, px = read_bmp(shot)
    ink = sum(px(x, y) == 0 for y in range(49, 330, 2) for x in range(16, 400, 2))
    if (w, h) != (512, 342) or px(100, 29) != 0 or px(100, 30) == 0 or ink < 1000:
        raise AssertionError(f'{shot}: the README window is not open ({w}x{h}, {ink} dark pixels in its text)')


def stub_rom(path: Path):
    """A ROM file the stub machine (the hidden model `stub`) takes: the
    PowerBook 520's checksum and size, its words summing to that checksum."""
    size, target = 2 * 1024 * 1024, 0xB6909089
    data = bytearray(size)
    n, rest = divmod(target, 0xFFFF)
    data[4:4 + 2 * n] = b'\xff' * (2 * n)
    data[4 + 2 * n:6 + 2 * n] = struct.pack('>H', rest)
    data[0:4] = struct.pack('>I', target)
    path.write_bytes(bytes(data))


def check_config_window():
    folder = WORK / 'config-window'
    folder.mkdir(parents=True, exist_ok=True)
    for f in folder.glob('*'):
        if f.is_file():
            f.unlink()
    (folder / 'roms').mkdir(exist_ok=True)
    stub_rom(folder / 'roms' / 'stub.rom')
    config = folder / 'livadix.yaml'
    config.write_text('# my own comment\nvideo:\n  filter: sharp\nprofiles:\n'
                      '  Stub:\n    model: stub\n    video: {look: plain}   # kept\n'
                      '  Other:\n    model: stub\n    emulation:\n      ram_mb: 8\n'
                      '  "No ROM":\n    model: quadra-700\n', encoding='utf-8')
    # The window over the running stub machine: every tab drawn, a look that
    # applies as it runs, memory saved with its warning and waiting for
    # Restart, another profile started.
    tabs = ('Machine', 'Video', 'Storage', 'Sound', 'Input', 'Ports', 'Network', 'Sharing')
    lines = ['frames 3', 'open', 'expect-open yes']
    for tab in tabs:
        lines += [f'press {tab}', f'screenshot "{posix(folder / (tab.lower() + ".bmp"))}"']
    lines += ['key win', 'frames 1', 'open',
              # Close warns about unsaved changes (the same ones once) and keeps them; what applies as it runs does
              'press Video', 'choose Look CRT', 'press Close', 'expect-open yes', 'press "Don\'t Save"', 'frames 2', 'expect-open no',
              'expect-config look crt',
              'open', 'key win', 'frames 2', 'expect-open no',
              # Save warns when the running Macintosh needs a new start for a change; Close leaves that to Restart
              'open', 'press Video', 'choose Look Plain', 'press Machine', 'choose Memory "5 MB"', 'press Save', 'press Later', 'expect Saved',
              'press Close', 'frames 2', 'expect-open no', 'expect-config look plain', 'expect-config ram 4',
              'open', 'press Restart', 'frames 2', 'expect-open no', 'expect-config ram 5',
              'open', 'select Other', 'press Start', 'frames 2', 'expect-running Other', 'expect-config ram 8',
              'open',
              # Stop: the Macintosh off, the logo shown; a click there opens the window again
              'press Stop', 'frames 2', 'expect-open no', f'screenshot "{posix(folder / "logo.bmp")}"', 'frames 2', 'open',
              'expect-open yes']
    script = folder / 'script.txt'
    script.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    p = run([LIVADIX, '--hidden', '--config', config, '--profile', 'Stub', '--frames', '400', '--script', script], cwd=folder,
            check=False, timeout=120)
    if p.returncode != 0:
        raise AssertionError(f'the configuration window, exit {p.returncode}:\n{p.stdout}{p.stderr}')
    for want in ('Configuration: closed; nothing changed', 'Configuration: closed; the changes apply as the Macintosh runs', 'Configuration: closed; changes wait for a new start',
                 'Configuration: started the profile "Other"', 'Stopped: the Macintosh is off'):
        if want not in p.stdout:
            raise AssertionError(f'expected "{want}" in:\n{p.stdout}')
    for tab in tabs:
        w, h, px = read_bmp(folder / (tab.lower() + '.bmp'))
        if (w, h) != (820, 600) or px(0, 0) != 0 or px(100, 300) == px(0, 0):
            raise AssertionError(f'{tab.lower()}.bmp: {w}x{h}')
    w, h, px = read_bmp(folder / 'logo.bmp')
    blue = sum(1 for y in range(0, h, 2) for x in range(0, w, 2) if (px(x, y) & 0xFF) > (px(x, y) >> 16) + 60)
    if px(0, 0) != 0xDDDDDD or blue < 500:
        raise AssertionError(f'logo.bmp: {w}x{h}, corner {px(0, 0):06X}, {blue} blue pixels: not the logo')
    # Livadix reads what the window wrote: the comments and the other profiles kept.
    text = config.read_text(encoding='utf-8')
    if not text.startswith('# my own comment\n') or '      ram_mb: 8\n' not in text or '"No ROM":\n    model: quadra-700\n' not in text:
        raise AssertionError(f'the saved livadix.yaml:\n{text}')
    inspected = run([LIVADIX, '--inspect', '--config', config, '--profile', 'Stub'], cwd=folder).stdout
    if 'Emulation: 5 MiB' not in inspected or 'plain look' not in inspected:
        raise AssertionError(f'livadix.exe --inspect of the saved file:\n{inspected}')
    # A profile that cannot start: the window says why, and another starts
    # from it; without a script, a hidden run reports it and fails as before.
    script.write_text('expect-open yes\nexpect "could not start"\nselect Stub\npress Start\nframes 2\nexpect-open no\n'
                      'expect-running Stub\n', encoding='utf-8')
    p = run([LIVADIX, '--hidden', '--config', config, '--profile', 'No ROM', '--frames', '30', '--script', script], cwd=folder,
            check=False, timeout=120)
    if p.returncode != 0 or 'Configuration: the Macintosh could not start: Cannot find ROM' not in p.stdout:
        raise AssertionError(f'a profile that cannot start, exit {p.returncode}:\n{p.stdout}{p.stderr}')
    p = run([LIVADIX, '--hidden', '--config', config, '--profile', 'No ROM', '--frames', '30'], cwd=folder, check=False)
    if p.returncode != 1 or 'Cannot find ROM' not in p.stderr:
        raise AssertionError(f'a hidden run of a profile that cannot start: exit {p.returncode}\n{p.stdout}{p.stderr}')
    print(f'    8 tabs drawn over the stub machine ({posix(folder)}/*.bmp); closed unchanged, a look applied as it ran, memory by a new start, '
          'another profile started, saved and read back, stopped to the logo; a failed start shown in the window')


def read_bmp(path: Path):
    """(width, height, pixel(x, y) -> 0xRRGGBB) of an uncompressed or bitfield BMP."""
    data = path.read_bytes()
    offset, = struct.unpack_from('<I', data, 10)
    width, height, _, bpp, compression = struct.unpack_from('<iiHHI', data, 18)
    top_down = height < 0
    height = abs(height)
    if bpp not in (24, 32):
        raise AssertionError(f'{path.name}: {bpp}-bit BMP')
    masks = (0xff0000, 0xff00, 0xff)
    if compression == 3:
        masks = struct.unpack_from('<III', data, 54)
    stride = (width * bpp // 8 + 3) & ~3

    def channel(v, mask):
        shift = (mask & -mask).bit_length() - 1
        return (v & mask) >> shift

    def pixel(x, y):
        row = y if top_down else height - 1 - y
        at = offset + row * stride + x * bpp // 8
        v = int.from_bytes(data[at:at + bpp // 8], 'little')
        return channel(v, masks[0]) << 16 | channel(v, masks[1]) << 8 | channel(v, masks[2])
    return width, height, pixel


def screen_config(depth: int, rom: Path) -> Path:
    # The stub machine's own hidden model (models.h, MC_MODEL_STUB; any file
    # with the PowerBook 520 ROM's checksum serves it), in the plain look.
    return write_config(f'screen-{depth}.yaml',
                        f"model: stub\nemulation:\n  rom: '{posix(rom)}'\n"
                        f"video:\n  resolution: 640x480\n  depth: {depth}\n  look: plain\nstorage:\n  disks: []\nsound:\n  enabled: false\n")


def check_screen():
    rom = ROMS / 'powerbook-520.rom'
    if not rom.exists():
        raise Skip('no roms/powerbook-520.rom')
    # The second bar is yellow, as near as each depth's palette allows.
    yellow = {1: 0xffffff, 2: 0xaaaaaa, 4: 0xfcf305, 8: 0xffff00, 16: 0xffff00, 32: 0xffff00}
    for depth, want in yellow.items():
        shot = WORK / f'screen-{depth}.bmp'
        shot.unlink(missing_ok=True)
        run([LIVADIX, '--config', screen_config(depth, rom), '--hidden', '--frames', '30', '--screenshot', shot])
        w, h, px = read_bmp(shot)
        if (w, h) != (640, 480):
            raise AssertionError(f'{shot.name}: {w}x{h}')
        got = (px(0, 0), px(120, 40), px(20, 20))
        if got != (0, want, 0xffffff):
            raise AssertionError(f'depth {depth}: border, yellow, white = {[hex(v) for v in got]}')


def check_looks():
    source = WORK / 'screen-8.bmp'
    if not source.exists():
        raise Skip('needs the depth-8 screenshot of the screen check')
    plain = None
    for look in range(3):
        for monitor in range(3):
            out = WORK / f'look-{look}-{monitor}.bmp'
            out.unlink(missing_ok=True)
            p = run([LOOK_SHOT, source, out, look, monitor, 2], check=False)
            if p.returncode == 2:
                raise Skip('OpenGL 3.3 is unavailable in this session')
            if p.returncode != 0:
                raise AssertionError(f'look {look}, monitor {monitor}: exit {p.returncode} {p.stderr}')
            w, h, px = read_bmp(out)
            if (w, h) != (1280, 960):
                raise AssertionError(f'{out.name}: {w}x{h}')
            if look == 0 and monitor == 0:
                plain = px
                if px(240, 80) != 0xffff00:
                    raise AssertionError(f'plain look: {px(240, 80):06x} where the yellow bar is')
    if plain is None:
        raise AssertionError('no plain picture')
    print('    9 looks rendered')


def indented(text: str) -> str:
    return '    ' + text.strip().replace('\n', '\n    ')


SST_SLICES = 8              # the suite's 124 groups in this many processes (the quick tier runs the first)


def cpu_suite_units() -> list:
    return ['0'] if QUICK else [str(i) for i in range(SST_SLICES)]


def check_cpu_suite():
    suite = ROOT / 'tests' / 'cpu_suite.py'
    status = run([sys.executable, suite, 'status']).stdout
    if not status.startswith('124/124'):
        p = run([sys.executable, suite, 'fetch'], check=False, timeout=1800)
        if p.returncode != 0:
            raise Skip(f'the suite could not be fetched: {(p.stderr or p.stdout).strip()[-200:]}')
    for i in ([int(UNIT)] if UNIT is not None else range(SST_SLICES)):
        only = f'%{i}/{SST_SLICES}'
        p = run([CPU_TESTS, 'sst', ROOT / 'work' / 'cpu' / 'sst', only], check=False, timeout=600)
        if p.returncode != 0:
            raise AssertionError(p.stdout[-3000:] + p.stderr[-3000:])
        summary = p.stdout[p.stdout.index('SingleStepTests'):]
        j = run([CPU_TESTS, 'sst', ROOT / 'work' / 'cpu' / 'sst', only, '--jit'], check=False, timeout=900)
        if j.returncode == 77:
            print(indented(f'slice {i}/{SST_SLICES}: ' + summary.splitlines()[0]))
            continue
        if j.returncode != 0:
            raise AssertionError('through the translator:\n' + j.stdout[-3000:] + j.stderr[-3000:])
        if j.stdout != p.stdout:
            a, b = p.stdout.splitlines(), j.stdout.splitlines()
            first = next((n for n in range(min(len(a), len(b))) if a[n] != b[n]), min(len(a), len(b)))
            raise AssertionError(f'the translator differs from the interpreter:\n  {a[first] if first < len(a) else ""}\n  '
                                 f'{b[first] if first < len(b) else ""}')
        failed = re.search(r'failed\s+(\d+)', summary)
        print(indented(f'slice {i}/{SST_SLICES}: {summary.splitlines()[0].split(": ", 1)[1]}; '
                       f'failed {failed.group(1) if failed else "?"}; the translator gives the same results'))
        # The same slice on the 68000 model, nothing skipped (sst.c, --68000).
        k = run([CPU_TESTS, 'sst', ROOT / 'work' / 'cpu' / 'sst', only, '--68000'], check=False, timeout=600)
        if k.returncode != 0:
            raise AssertionError('on the 68000 model:\n' + k.stdout[-3000:] + k.stderr[-3000:])
        s000 = k.stdout[k.stdout.index('SingleStepTests'):]
        passed = re.search(r'passed \(registers[^)]*\)\s+(\d+)', s000)
        print(indented(f'slice {i}/{SST_SLICES} on the 68000 model: {s000.splitlines()[0].split(": ", 1)[1]}; '
                       f'passed in full {passed.group(1) if passed else "?"}, failed 0'))


def check_fpu_suite():
    p = run([sys.executable, ROOT / 'tests' / 'fpu_suite.py', '--cases', '4000'], check=False, timeout=1800)
    if p.returncode == 77:
        raise Skip(p.stdout.strip())
    if p.returncode != 0:
        raise AssertionError(p.stdout[-4000:] + p.stderr[-2000:])
    print(indented(p.stdout))


def check_rom_trace():
    rom = ROMS / 'quadra-700.rom'
    if not rom.exists():
        raise Skip('no roms/quadra-700.rom')
    first = run([ROM_TRACE, rom, '--check'], check=False)
    if first.returncode != 0:
        raise AssertionError(first.stdout + first.stderr)
    longer = run([ROM_TRACE, rom, '--check', '--continue', '4096', '--max', '1000000'], check=False, timeout=300)
    if longer.returncode != 0:
        raise AssertionError(longer.stdout + longer.stderr)
    print(indented(first.stdout))


BOOT_INSTRUCTIONS = 700_000_000      # well past the question-mark disk of a 25 MHz Quadra 700


def boot_run(name: str, args: list) -> str:
    rom = ROMS / 'quadra-700.rom'
    shot = WORK / f'{name}.bmp'
    shot.unlink(missing_ok=True)
    p = run([HW_TRACE, rom, '--max', BOOT_INSTRUCTIONS, '--shot', shot] + args, check=False, timeout=600)
    first = p.stdout.splitlines()[0] if p.stdout else ''
    if p.returncode != 0 or not first.endswith('; ok'):
        raise AssertionError(f'{name}: exit {p.returncode}\n{p.stdout[-2000:]}{p.stderr[-1000:]}')
    return p.stdout


def dumped(out: str, address: int) -> list:
    for line in out.splitlines():
        if line.startswith(f'{address:08X}:'):
            return [int(b, 16) for b in line.split(':')[1].split()]
    raise AssertionError(f'no dump of {address:08X} in:\n{out[-1500:]}')


def desktop_with_disk(shot: Path, size: tuple):
    """The gray desktop (a black-and-white checkerboard everywhere away from
    the centre: not a Sad Mac's black screen) with the disk icon in the
    middle (runs of one colour the checkerboard never has)."""
    w, h, raw = read_bmp(shot)
    if (w, h) != size:
        raise AssertionError(f'{shot.name}: {w}x{h}, expected {size[0]}x{size[1]}')

    def px(x, y):
        """Light (1), dark (0) or neither (the ROM's white goes through the
        monitor's gamma: $FFF7D6 on the 21-inch)."""
        v = raw(x, y)
        channels = (v >> 16 & 255, v >> 8 & 255, v & 255)
        return 1 if min(channels) >= 0xC0 else 0 if max(channels) <= 0x40 else v + 2
    for x, y in [(100, 100), (w - 100, h - 100), (50, h - 50), (w - 60, 60)]:
        a, b, c = px(x, y), px(x + 1, y), px(x + 1, y + 1)
        if {a, b} != {0, 1} or a != c:
            raise AssertionError(f'{shot.name}: no gray desktop at ({x}, {y}): {raw(x, y):06x} {raw(x + 1, y):06x}')
    cx, cy = w // 2, h // 2
    longest = 0
    for y in range(cy - 25, cy + 25):
        run_length = 0
        for x in range(cx - 25, cx + 25):
            run_length = run_length + 1 if px(x, y) == px(x - 1, y) else 0
            longest = max(longest, run_length)
    if longest < 8:
        raise AssertionError(f'{shot.name}: no disk icon at the centre')


def screen_line(out: str) -> str:
    return next((l for l in out.splitlines() if l.startswith('screen:')), 'screen: none')


def check_boot():
    if not (ROMS / 'quadra-700.rom').exists():
        raise Skip('no roms/quadra-700.rom')
    WORK.mkdir(parents=True, exist_ok=True)
    report = []
    for res, size in (('640x480', (640, 480)), ('1152x870', (1152, 870))):
        nvram = WORK / f'boot-{res}.nvram'
        nvram.unlink(missing_ok=True)
        args = ['--resolution', res, '--depth', '8', '--nvram', nvram, '--dump', '0x20C', '4', '--dump', '@0xCF8', '24']
        # A fresh PRAM: the ROM initialises it and starts in its default depth.
        # The host's clock reads 3,000,000,000 (1999), then 1000 s later.
        host = 3_000_000_000
        first = boot_run(f'boot-{res}-first', args + ['--clock', host])
        if f'screen: {res}, 1 bits' not in first:
            raise AssertionError(f'{res}, fresh PRAM: {screen_line(first)}')
        desktop_with_disk(WORK / f'boot-{res}-first.bmp', size)
        if not nvram.exists():
            raise AssertionError(f'{res}: no PRAM file written')
        # ADB: the keyboard (handler 2) at 2 and the mouse (handler 1) at 3 in
        # the ADB Manager's device table.
        table = dumped(first, int(next(l for l in first.splitlines() if l.startswith('0000') and not l.startswith('0000020C')).split(':')[0], 16))
        if table[0:3] != [2, 2, 2] or table[12:15] != [1, 3, 3]:
            raise AssertionError(f'{res}: ADB device table {table}')
        time_first = int.from_bytes(bytes(dumped(first, 0x20C)), 'big')
        # The PRAM persisted: the configured depth applies, the clock went on.
        second = boot_run(f'boot-{res}', args + ['--clock', host + 1000])
        if f'screen: {res}, 8 bits' not in second:
            raise AssertionError(f'{res}, saved PRAM: {screen_line(second)}')
        desktop_with_disk(WORK / f'boot-{res}.bmp', size)
        time_second = int.from_bytes(bytes(dumped(second, 0x20C)), 'big')
        # A clock set in the Macintosh (an adjustment of 500 s against the
        # host's time, as the PRAM file keeps it) is kept at the next start.
        adjusted_nvram = WORK / f'boot-{res}-adjusted.nvram'
        data = bytearray(nvram.read_bytes())
        data[9:17] = (500).to_bytes(8, 'little', signed=True)
        adjusted_nvram.write_bytes(bytes(data))
        adjusted = boot_run(f'boot-{res}-adjusted', [a if a != nvram else adjusted_nvram for a in args] + ['--clock', host + 2000])
        time_adjusted = int.from_bytes(bytes(dumped(adjusted, 0x20C)), 'big')
        # Time (ReadDateTime from the RTC at startup, then the one-second
        # interrupt) is the host's time at start (plus the adjustment) plus
        # guest time: the clock the ROM read through both forms of the
        # seconds commands.
        for t, out, start in ((time_first, first, host), (time_second, second, host + 1000),
                              (time_adjusted, adjusted, host + 2500)):
            guest = float(re.search(r'guest time ([0-9.]+) s', out).group(1))
            if not (start + guest - 4 <= t <= start + guest + 1):
                raise AssertionError(f'{res}: Time {t} after {guest:.1f} s of guest time from {start}')
        report.append(f'{res}: 1 bit on a fresh PRAM, then 8 bits; Time the host clock + guest time, the adjustment kept')
    # The mouse moves the cursor: the 640 x 480 run again with motion.
    nvram = WORK / 'boot-640x480.nvram'
    moved = boot_run('boot-mouse', ['--resolution', '640x480', '--depth', '8', '--nvram', nvram,
                                    '--clock', 3_000_001_000, '--mouse', '0,40@300000000'])
    w, h, still = read_bmp(WORK / 'boot-640x480.bmp')
    _, _, after = read_bmp(WORK / 'boot-mouse.bmp')
    near = [(x, y) for y in range(0, 120) for x in range(0, 60) if still(x, y) != after(x, y)]
    if not near or max(y for _, y in near) < 40:
        raise AssertionError('the cursor did not move with the mouse')
    report.append('the mouse moves the cursor')
    # livadix.exe runs the real machine: five seconds in a hidden window (the
    # startup chime takes 1.5 of them) reach the gray desktop.
    app = write_config('boot-app.yaml', f"model: quadra-700\nemulation:\n  rom: '{posix(ROMS / 'quadra-700.rom')}'\n"
                                        "  ram_mb: 8\nvideo:\n  resolution: 640x480\n  depth: 8\nstorage:\n  disks: []\n"
                                        "sound:\n  enabled: false\n")
    shot = WORK / 'boot-app.bmp'
    shot.unlink(missing_ok=True)
    out = run([LIVADIX, '--config', app, '--hidden', '--frames', '300', '--screenshot', shot]).stdout
    if '; emulated' not in out:
        raise AssertionError(out)
    w, h, px = read_bmp(shot)
    if (w, h) != (640, 480) or {px(100, 100), px(101, 100)} != {0, 0xFFFFFF}:
        raise AssertionError(f'livadix.exe: {w}x{h}, no gray desktop')
    report.append('livadix.exe shows the gray desktop')
    print(indented('\n'.join(report)))


# --- os-boot: the boot matrix ---------------------------------------------------------
#
# The cells live in data files, one per family: tests/boot_matrix/<family>.toml.
# Every file of the folder runs, so a family adds its rows without editing
# this file. A file names `model`, `rom` (its file in roms/),
# `ram_mb`, `gestalt` (what every System reports there: selector = value),
# `media` (images made fresh for the run: name = {kib, name} an empty HFS
# volume from tests/hfs_blank.py, without `name` zeros) and its `cell`s.
# A cell is one headless run of mc_hw_trace (or with `runner = "livadix"` of
# livadix.exe, `frames` frames) and what it must show:
#   name, quick (true: the quick tier runs it, one cell or two per family),
#   model and rom (the file's unless given: one file holds a family
#   whose models share a board), resolution, look (runner "livadix": video.look),
#   depth, video_card, ram_mb, max, finder (instructions to run on after the
#   Finder came up), cdrom (a media name), run_code (a src/hw/guest/bin
#   image), args (more mc_hw_trace arguments), input (see cell_inputs),
#   continue (the disk and PRAM of an earlier cell, for a restart), bus ("ata": the disk on
#   the machine's ATA bus, storage.disks bus: ata), floppy (true: the
#   disk in the first floppy drive instead),
#   fresh_pram (no PRAM file: the one Livadix makes for a first start);
#   screen (true: the cell's size and depth, or the text after `screen: `),
#   menu_bar, gestalt (the file's merged with these), gestalt_bits,
#   chimes (how many times the machine chimed: starts, so resets show),
#   question_mark (the ROM's gray screen with the disk icon: no startup disk),
#   boot_volume, volumes (mounted), post_clean, post_log (PRAM bytes the
#   POST logged: "F3" = value), unmounted (the disk marked unmounted
#   cleanly at the end, drAtrb bit 8), mouse_at ("x,y": where the System has
#   the cursor at the end, low memory Mouse), lowmem_byte ({"208" = value}:
#   one low-memory byte at the end), chime, alert (a sound after
#   the Finder came up), silent (the loudest sample allowed with the chime
#   off), written (files the run must change; "disk" for the cell's disk),
#   hfs (media holding an HFS volume afterwards), fpsp_save, fpsp_compare
#   (the FPSP probe's results kept, and compared), probe (offset = longword
#   in a probe's results), moved (an earlier cell whose screen must differ
#   inside moved_box), pixels ("x,y" = 0xRRGGBB in the screenshot),
#   alerts (the ALRT IDs the run may show: any other alert or system error
#   box at any time of the run fails it, even one input dismissed), note
#   (for the report), powered_off (the run ends with the Macintosh switching
#   itself off: Shut Down through its power controller), asleep (the
#   processor stopped asleep for 30 s and the next input given then: the
#   run slept, and went on), left_mounted (the
#   disk the cell continues from was left mounted by the earlier run, which
#   ended without a shutdown: drAtrb bit 8 clear). menu_bar may name
#   the screen's size when the run changes it ("800x600"); shot_size (runner
#   "livadix": the screenshot's size alone, "800x600").

OS_DIR = WORK / 'os'
# The FPSP probe's results (src/hw/guest/probes/fpsp_probe.s): the offsets
# of transcendental results, which the ROM's floating-point package computes
# to its own accuracy (within an ulp, and absolute error near 2^-65 for
# cos near pi/2) where emulation.fpu_full rounds the exact value; and the
# address registers stepped by (An)+/-(An), as offsets from the operands.
PROBE_TRANSCENDENTAL = [0x00, 0x10, 0x20, 0x2C, 0x38, 0x48, 0x58, 0x68, 0x74]
PROBE_STEPS = {0x0C: 12, 0x1C: 12, 0x44: 36, 0x54: 40, 0x64: 48, 0xA8: 60}
PROBE_DONE = 0x134


def os_trace(name: str, rom: Path, args: list, timeout=900, end='; ok') -> str:
    p = run([HW_TRACE, rom] + args, check=False, timeout=timeout)
    if p.returncode != 0 or not any(l.endswith(end) for l in p.stdout.splitlines()):
        raise AssertionError(f'{name}: exit {p.returncode}\n{p.stdout[-2000:]}{p.stderr[-1000:]}')
    return p.stdout


def copy_of(src: Path, name: str) -> Path:
    dst = OS_DIR / name
    dst.write_bytes(src.read_bytes())
    return dst


def finder_line(name: str, out: str) -> tuple:
    m = re.search(r'finder: instruction (\d+), guest time ([0-9.]+) s, host time ([0-9.]+) s', out)
    if not m:
        raise AssertionError(f'{name}: the Finder never came up\n{out[-1500:]}')
    return int(m.group(1)), float(m.group(2)), float(m.group(3))


def gestalts(out: str) -> dict:
    return {m.group(1): (int(m.group(2)), int(m.group(3)))
            for m in re.finditer(r"gestalt '(.{4})' = (-?\d+) \(\$[0-9A-F]+\), error (-?\d+)", out)}


def volumes(out: str) -> list:
    return [l[len('volume: '):] for l in out.splitlines() if l.startswith('volume: ')]


def finder_menu_bar(shot: Path, size: tuple, *, titles=False):
    """A menu bar across the top (light, its titles dark, the black line under
    it at row 19), as the Finder draws it once it runs."""
    w, h, px = read_bmp(shot)
    if (w, h) != size:
        raise AssertionError(f'{shot.name}: {w}x{h}, expected {size[0]}x{size[1]}')

    def share(y, test):
        return sum(1 for x in range(w) if test(px(x, y))) / w
    dark = lambda v: max(v >> 16, v >> 8 & 255, v & 255) <= 0x40
    light = lambda v: min(v >> 16, v >> 8 & 255, v & 255) >= 0xC0
    if share(19, dark) < 0.98 or share(1, light) < 0.9 or not (0.02 <= share(10, dark) <= 0.4):
        raise AssertionError(f'{shot.name}: no menu bar')
    if titles and sum(dark(px(x, 10)) for x in range(24, min(w // 2, 320))) < 18:
        raise AssertionError(f'{shot.name}: no Finder menu titles (a clock alone is startup)')


def probe_results(name: str, out: str) -> bytes:
    if 'run-code: done' not in out:
        raise AssertionError(f'{name}: the probe did not finish\n{out[-1500:]}')
    data = bytearray()
    for l in out.splitlines():
        if l.startswith('res '):
            data += bytes(int(b, 16) for b in l.split(':')[1].split())
    return bytes(data)


def fpsp_results(name: str, out: str) -> bytes:
    data = probe_results(name, out)
    if data[PROBE_DONE:PROBE_DONE + 4] != b'LVDX':
        raise AssertionError(f'{name}: the FPSP probe stopped early')
    return data


def extended(b: bytes):
    from fractions import Fraction
    se, mant = int.from_bytes(b[0:2], 'big'), int.from_bytes(b[4:12], 'big')
    v = Fraction(mant) * Fraction(2) ** ((se & 0x7FFF) - 16383 - 63)
    return -v if se & 0x8000 else v


def compare_probe(trapping: bytes, full: bytes) -> list:
    from fractions import Fraction
    for results in (trapping, full):
        ops = int.from_bytes(results[0x0C:0x10], 'big') - 12
        for at, step in PROBE_STEPS.items():
            got = int.from_bytes(results[at:at + 4], 'big')
            if got != ops + step:
                raise AssertionError(f'FPSP probe: address register at result {at:#x} is operands+{got - ops}, expected +{step}')
    differ = []
    for at in range(0, PROBE_DONE + 4, 2):
        if trapping[at:at + 2] == full[at:at + 2] or any(s <= at < s + 4 for s in PROBE_STEPS):
            continue
        slot = next((s for s in PROBE_TRANSCENDENTAL if s <= at < s + 12), None)
        if slot is None:
            raise AssertionError(f'FPSP probe: result at {at:#x} differs: trapping {trapping[at:at + 4].hex()}, fpu_full {full[at:at + 4].hex()}')
        if slot not in differ:
            differ.append(slot)
    for slot in differ:
        a, b = extended(trapping[slot:slot + 12]), extended(full[slot:slot + 12])
        if abs(a - b) > max(abs(a), abs(b)) * Fraction(1, 2 ** 62) + Fraction(1, 2 ** 64):
            raise AssertionError(f'FPSP probe: transcendental at {slot:#x}: trapping {float(a)!r}, fpu_full {float(b)!r}')
    return differ


def wav_peak(path: Path, after: float = 0.0) -> int:
    """The loudest sample of a WAV file, from `after` seconds on."""
    import array
    import wave
    with wave.open(str(path)) as w:
        w.setpos(min(int(after * w.getframerate()), w.getnframes()))
        samples = array.array('h', w.readframes(w.getnframes()))
    return max((abs(s) for s in samples), default=0)


def wav_bipolar(path: Path, after: float, level: int) -> bool:
    """Detect an audible excursion in both directions within 20 ms."""
    import array
    import wave
    with wave.open(str(path)) as w:
        w.setpos(min(int(after * w.getframerate()), w.getnframes()))
        while samples := array.array('h', w.readframes(w.getframerate() // 50)):
            if min(samples) <= -level and max(samples) >= level:
                return True
    return False


def wav_sounds(path: Path) -> list:
    """When each sound of a WAV file starts (seconds): a tenth of a second
    louder than 4000 after silence (every sample below 2000 for half a
    second), so a chime counts once however it fades."""
    import array
    import wave
    with wave.open(str(path)) as w:
        rate = w.getframerate()
        samples = array.array('h', w.readframes(w.getnframes()))
    step = rate // 10 * 2                   # a tenth of a second, both channels
    starts, quiet = [], 5
    for i in range(0, len(samples), step):
        peak = max((abs(v) for v in samples[i:i + step]), default=0)
        if peak > 4000 and quiet >= 5:
            starts.append(round(i / 2 / rate, 1))
        quiet = 0 if peak >= 2000 else quiet + 1
    return starts


def digest(path) -> str:
    import hashlib
    return hashlib.sha1(Path(path).read_bytes()).hexdigest() if path and Path(path).exists() else ''


def slug(name: str) -> str:
    return re.sub(r'[^a-z0-9.]+', '-', name.lower()).strip('-')


def matrix_files() -> list:
    import tomllib
    files = []
    for path in sorted(BOOT_MATRIX.glob('*.toml')):
        with path.open('rb') as f:
            files.append((path, tomllib.load(f)))
    return files


def cell_inputs(events: list) -> list:
    """The hw_trace arguments of a cell's `input` list: `click X,Y @T`
    (point, then the button down and up a million instructions apart),
    `point X,Y @T`, `down @T`, `up @T`, `key HEX @T` (| 80 for a release),
    `insert-floppy FILE @T`, `insert-cdrom FILE @T` (FILE in the matrix's
    work folder), `mouse DX,DY @T` (motion), `control C,V @T` (mc_machine_control: C 0 the keyswitch,
    1 brightness, 2 contrast, 3 volume, 4 the interrupt switch, 5 the reset switch, V 1 pressed and 0 released). T counts instructions; @+T counts from the
    Finder's start."""
    args = []
    for event in events:
        words = event.split()
        verb, at = words[0], words[-1]
        if not at.startswith('@'):
            raise AssertionError(f'input "{event}": no @time')
        rel = at[1:].startswith('+')
        t = int(at[1:].lstrip('+').replace('_', ''))

        def when(n):
            return f'@{"+" if rel else ""}{n}'
        if verb == 'click':
            args += ['--point', f'{words[1]}{when(t)}', f'--down{when(t + 1_000_000)}', f'--up{when(t + 2_000_000)}']
        elif verb == 'point':
            args += ['--point', f'{words[1]}{when(t)}']
        elif verb == 'mouse':
            args += ['--mouse', f'{words[1]}{when(t)}']
        elif verb in ('down', 'up'):
            args += [f'--{verb}{when(t)}']
        elif verb == 'key':
            args += ['--key', f'{words[1]}{when(t)}']
        elif verb in ('insert-floppy', 'insert-cdrom'):
            args += [f'--{verb}', f'{OS_DIR / words[1]}{when(t)}']
        elif verb == 'control':
            args += ['--control', f'{words[1]}{when(t)}']
        else:
            raise AssertionError(f'input "{event}": unknown event')
    return args


def shot_size(cell: dict, width: int, height: int) -> tuple:
    """The screenshot's size: the cell's, or the one menu_bar names."""
    if isinstance(cell.get('menu_bar'), str):
        return tuple(int(v) for v in cell['menu_bar'].split('x'))
    return width, height


def remove(path: Path):
    """Deletes a file (or a folder, such as a run's nvram folder, with what
    it holds), retrying for up to two seconds while Windows still holds it
    (a scanner or indexer opening a just-written image)."""
    import time
    if path.is_dir():
        for entry in path.iterdir():
            remove(entry)
        path.rmdir()
        return
    for _ in range(20):
        try:
            path.unlink(missing_ok=True)
            return
        except PermissionError:
            time.sleep(0.1)
    path.unlink(missing_ok=True)


UNIT = None             # the unit of a check this process runs (a worker); None: the whole check
QUICK = False           # the quick tier (a worker's copy of the parent's choice)
FILTER: dict = {'family': None, 'cell': None}


def want(unit: str) -> bool:
    return UNIT is None or UNIT == unit


TIMINGS: dict = {'checks': {}, 'cells': {}, 'chime': {}}


def run_cell(family: dict, cell: dict, state: dict) -> str:
    import time
    t0 = time.perf_counter()
    try:
        return run_cell_inner(family, cell, state)
    finally:
        TIMINGS['cells'][cell['name']] = round(time.perf_counter() - t0, 2)


PDM_BOOT = BUILD / ('mc_ppc_pdm_boot' + EXE)


def pci1_console_nvram(path: Path):
    """Select the original OF console in a disposable Grand Central NVRAM.
    IEEE 1275 auto-boot?; the ROM's 1275 configuration header/checksum at
    physical $1800 (observed original ROM 9630C68B, OF 1.0.5)."""
    raw = bytearray(path.read_bytes())
    base = 273
    if len(raw) != base + 8192 or raw[:8] != b'LVDXPRAM' or raw[base + 0x1800:base + 0x1802] != b'\x12\x75':
        raise AssertionError(f'{path.name}: no initialized PCI Open Firmware NVRAM')
    raw[base + 0x180C] &= ~0x20
    raw[base + 0x1804:base + 0x1806] = bytes(2)
    total = sum(struct.unpack('>1024H', raw[base + 0x1800:base + 0x2000]))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    raw[base + 0x1804:base + 0x1806] = struct.pack('>H', 0xFFFF - total)
    path.write_bytes(raw)


def ppc_lcd_thousands(width: int) -> str:
    """Use the stock Mac OS 8.0 fixture's Monitors & Sound panel to set
    Thousands. The final screen, depth, and Finder checks are independent
    of the input commands and fail if the guest rejects the selection."""
    commands = ['run 2']

    def key(code):
        commands.extend((f'key {code:x} 1', 'run .065', f'key {code:x} 0'))

    def shortcut(code):
        commands.append('key 37 1')
        key(code)
        commands.extend(('key 37 0', 'run 1.3'))

    # Type the prefix of the boot volume, System Folder, Control Panels
    # and Monitors & Sound in successive Finder windows; Command-O opens.
    for prefix in ((4,), (1, 16, 1), (8, 31, 45), (46, 31, 45)):
        for code in prefix:
            key(code)
        shortcut(0x1f)
    # Select 256 colors first: the fixture may already start in Thousands.
    # Check the guest's actual 8-bit framebuffer before selecting 16 bits.
    for x, y in (((198, 190), (198, 202)) if width == 640 else ((282, 301), (282, 313))):
        commands.extend((f'mouse-to {x} {y}', 'mouse 0 0 1', 'run .15', 'mouse 0 0 0', 'run 2'))
        key(0x24)  # accept a mode confirmation if shown
        commands.extend(('run 2', 'video'))
    shortcut(0x0d)  # close Monitors & Sound and return to Finder
    commands.extend(('run 2', 'quit'))
    return '\n'.join(commands) + '\n'


def ppc_media_path(family: dict, name: str) -> Path:
    """A PowerPC cell's blank image under the cells' folder."""
    return OS_DIR / name


def run_ppc_cell(family: dict, cell: dict, state: dict) -> str:
    """A cell of a PowerPC family (`runner = "ppc"` in its file): one headless
    run of mc_ppc_pdm_boot (tests/ppc/pdm_boot.c) for `seconds` of guest
    time; `model` is a catalogue model key (including G3) or a catalogue model's
    number (7500: machines/pci1.c), `jit` the translator,
    `cuda = "firmware"` Apple's Cuda firmware from roms/. It takes
    floppy, cdrom, continue, ram_mb, resolution, depth, disk_bus ("ata" or
    "scsi"), video_card ("livadix"), m68k_accelerator (the 68k accelerator), keys
    (["7F@120"]: ADB code @ guest seconds), insert_floppy (["90
    floppy-dd.img"]), insert_cd (the same time/image form), and net (the Ethernet on the NAT, the network probe as
    a startup application: a DHCP lease and a TCP connection to the host,
    net_results), and checks screen, menu_bar, question_mark, chime,
    main_framebuffer (the guest QuickDraw ScrnBase),
    gestalt mach (box flag + 6 for legacy cells; guest_api or guest_floppy_api
    uses an actual Gestalt application for every selector), powered_off, volumes (mounted at the
    end) and written. media.g3_probe adds the original test application to a
    blank test floppy's root. control (["mouse-to 220 215", "mouse 0 0
    1", "run .3", "shot a.bmp"]) drives the run after `seconds` through
    pdm_boot's control channel; its shots are compared by shots_same and
    shots_differ ([["a.bmp", "b.bmp"]]). ticks_min is the least low-memory
    Ticks at the end, colors_min the least distinct colors of the final
    screen, flash_written a Pippin flash memory holding a valid parameter
    RAM in the PRAM file."""
    import time
    name = cell['name']
    rom = ROMS / cell.get('rom', family['rom'])
    res = cell.get('resolution', '640x480')
    width, height = (int(v) for v in res.split('x'))
    # A cold NVRAM initializer can select the ROM's documented default;
    # configured-mode cells independently require their requested geometry.
    expected_size = tuple(int(v) for v in cell['initial_screen'].split('x')) if 'initial_screen' in cell else (width, height)
    if 'continue' in cell:
        disk, nvram = state['cells'][cell['continue']]
    else:
        disk = None
        nvram = OS_DIR / f'{slug(name)}.nvram'
        nvram.unlink(missing_ok=True)
    if cell.get('guest_api') and cell.get('net'):
        raise AssertionError(f'{name}: guest API and network probes need separate cells')
    if cell.get('of_tree'):
        pci1_console_nvram(nvram)
    serial_probe = b''
    if cell.get('serial_loopback'):
        if not cell.get('guest_api') or any(k in cell for k in ('pram', 'pram_setting', 'pram_write')):
            raise AssertionError(f'{name}: serial loopback requires its own guest API cell')
        pbx_release_serial_port(disk)
        serial_probe = (ROOT / 'src/hw/guest/bin/pbx_serial_probe.bin').read_bytes()
    sound_alert = cell.get('guest_alert', False)
    if sound_alert and not cell.get('guest_api'):
        raise AssertionError(f'{name}: guest alert requires the startup API probe')
    if cell.get('guest_api'):
        import ppc_g3_probe
        probe = ppc_g3_probe.program(cell.get('pram_write', False), cell.get('pram_setting'), serial_probe,
                                     sound_alert=sound_alert)
        startup_application(disk, 'Livadix G3 Probe', PROBE_PROLOGUE + probe, replace_existing='continue' in cell)
    record_input = None
    if cell.get('guest_record'):
        if not disk or cell.get('guest_api') or cell.get('guest_floppy_api'):
            raise AssertionError(f'{name}: recording needs a dedicated guest probe disk')
        import struct
        probe = (ROOT / 'src/hw/guest/bin/pbx_record_probe.bin').read_bytes()
        startup_application(disk, 'Livadix Record Probe', PROBE_PROLOGUE + probe)
        record_input = OS_DIR / 'singer-input.pcm'
        # Distinct left/right ramps, centered on silence and long enough
        # for the guest input driver to drain several half buffers.
        record_input.write_bytes(b''.join(struct.pack('<hh', 16 * (i % 128) - 1024,
                                                       512 - 8 * (i % 128)) for i in range(4096)))
    state['cells'][name] = (disk, nvram)
    shot, wav = OS_DIR / f'{slug(name)}.bmp', OS_DIR / f'{slug(name)}.wav'
    shot.unlink(missing_ok=True)
    written = {f: digest(disk if f == 'disk' else OS_DIR / f) for f in cell.get('written', [])}
    args = [PDM_BOOT, rom, str(cell.get('model', family['model'])), cell['seconds'],
            '--ram', cell.get('ram_mb', family.get('ram_mb', 16)), '--size', res, '--nvram', nvram,
            '--shot', shot, '--every', 100000]
    if cell.get('guest_api') or cell.get('guest_floppy_api'):
        args += ['--find', '47334750', '40']
        if not any(cell.get(k) for k in ('keys', 'insert_floppy', 'insert_cd', 'powered_off', 'net', 'written', 'volumes')):
            args += ['--until-found', '47334750', 2]
    if record_input:
        args += ['--audio-input', record_input, '--find', '50425852', '40', '--until-found', '50425852', 3]
    # A cell that only waits for the Finder ends two guest seconds after its
    # menu bar shows instead of at `seconds`.
    early = (cell.get('menu_bar') and not any(cell.get(k) for k in ('keys', 'insert_floppy', 'insert_cd', 'powered_off', 'net', 'written', 'volumes'))
             and name not in state['continued'] and not (cell.get('guest_api') or cell.get('guest_floppy_api') or record_input))
    if cell.get('menu_bar'):
        args += ['--until-menu-bar', 2 if early else cell['seconds'] + 1]
    if cell.get('jit', family.get('jit')):
        args.append('--jit')
    if cell.get('m68k_accelerator'):
        args.append('--m68k-accel')
    if cell.get('cuda', family.get('cuda')) == 'firmware':
        args += ['--cuda-fw', ROMS]
    disk_bus = cell.get('disk_bus', family.get('disk_bus'))
    if serial_probe:
        args.append('--serial-loopback')
    if disk_bus:
        if disk_bus not in ('ata', 'scsi'):
            raise AssertionError(f'{name}: invalid disk_bus {disk_bus}')
        args.append('--' + disk_bus)
    if cell.get('cd_first'):
        args.append('--cd-first')
    if cell.get('cd_bus'):
        if cell['cd_bus'] not in ('ata', 'scsi'):
            raise AssertionError(f'{name}: invalid CD bus')
        args.append('--cd-' + cell['cd_bus'])
    if 'depth' in cell:
        args += ['--depth', cell['depth']]
    if disk:
        args += ['--floppy' if cell.get('floppy') else '--disk', disk]
    if 'cdrom' in cell:
        args += ['--cd', ppc_media_path(family, cell['cdrom'])]
    if cell.get('video_card'):
        args.append('--card')
    if 'main_framebuffer' in cell:
        args += ['--dump', '824', '4']
    if cell.get('chime') or sound_alert:
        args += ['--wav', wav]
    for key in cell.get('keys', []):
        args += ['--key', key]
    for key, flag in (('insert_floppy', '--insert-floppy'), ('insert_cd', '--insert-cd')):
        for item in cell.get(key, []):
            at, image = item.split()
            args += [flag, at, ppc_media_path(family, image)]
    if 'ticks_min' in cell:
        args += ['--dump', '16A', '4']
    if 'volumes' in cell:
        args.append('--volumes')
    if cell.get('of_tree'):
        args += ['--serial', '--type', r'dev /\rls\r@5',
                 '--type', r'dev /bandit/gc/mace\r@12', '--type', r'.properties\r@16']
    net = None
    if cell.get('net'):
        net = net_server()
        patched = enable_tcpip(disk) + net_probe_app(disk, net[0])
        args += ['--net', '--find', '4E455450', '200', '--find-a5']
    started = time.perf_counter()
    try:
        if cell.get('lcd_thousands'):
            args.append('--control')
            p = run(args, check=False, timeout=1200, input_text=ppc_lcd_thousands(width))
        elif 'control' in cell:
            args.append('--control')
            commands = [f'shot {OS_DIR / (slug(name) + "-" + c[5:].strip())}' if c.startswith('shot ') else c
                        for c in cell['control']]
            p = run(args, check=False, timeout=1200, input_text='\n'.join(commands + ['quit', '']))
        else:
            p = run(args, check=False, timeout=1200)
    finally:
        if net:
            net[3].close()
            net[2].join(5)
    host = time.perf_counter() - started
    out = p.stdout
    (OS_DIR / f'{slug(name)}.log').write_text(out + p.stderr, encoding='utf-8')
    if p.returncode != 0:
        raise AssertionError(f'{name}: exit {p.returncode}\n{out[-1500:]}{p.stderr[-500:]}')
    if 'system_version' in cell:
        version = re.search(r'^system-version: ([0-9A-F]{4})$', out, re.M)
        if not version or int(version[1], 16) != cell['system_version']:
            raise AssertionError(f'{name}: booted the wrong System, expected {cell["system_version"]:04X}')
    if 'main_framebuffer' in cell:
        base = re.search(r'^dump at 00000824: ([0-9A-F]{8})$', out, re.M)
        expected = cell['main_framebuffer']
        if not base or int(base[1], 16) != expected:
            raise AssertionError(f'{name}: main framebuffer {base[1] if base else None}, expected {expected:08X}')
    if cell.get('powered_off') and 'powered off' not in out:
        raise AssertionError(f'{name}: the machine did not switch itself off\n{out[-800:]}')
    if cell.get('unmounted') and not volume_attributes(disk) & 0x100:
        raise AssertionError(f'{name}: shutdown did not mark the volume unmounted cleanly')
    if cell.get('of_tree'):
        console = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', out).replace('\0', '')
        required = ['Open Firmware, 1.0.5', 'bandit', 'gc', 'mace', 'swim3', 'via-cuda', 'local-mac-address', '0800074D', '4143']
        if int(cell.get('model', family['model'])) != 7200:
            required.append('mesh')
        missing = [token for token in required if token not in console]
        if missing or not re.search(r'AAPL,interrupts\s+0000000E\s+00000002\s+00000003', console):
            raise AssertionError(f'{name}: original OF device tree mismatch: {missing}\n{console[-4000:]}')
    if cell.get('screen') and 'screen saved' not in out:
        raise AssertionError(f'{name}: no screen\n{out[-800:]}')
    if isinstance(cell.get('screen'), str) and f"video: {cell['screen']}" not in out:
        raise AssertionError(f"{name}: framebuffer mismatch, expected {cell['screen']}\n{out[-800:]}")
    if cell.get('lcd_thousands'):
        depths = re.findall(r'^control video [0-9]+x[0-9]+ depth ([0-9]+) stride ', out, re.MULTILINE)
        if out.count('control mouse-to 1') != 2 or depths != ['8', '16']:
            raise AssertionError(f'{name}: guest LCD depth selections were {depths!r}, expected 8 then 16')
    if cell.get('menu_bar'):
        finder_menu_bar(shot, expected_size, titles=True)
    if 'ticks_min' in cell:
        ticks = re.search(r'^dump at 0000016A: ([0-9A-F]{8})$', out, re.M)
        if not ticks or int(ticks[1], 16) < cell['ticks_min']:
            raise AssertionError(f'{name}: Ticks {int(ticks[1], 16) if ticks else None}, expected at least {cell["ticks_min"]}')
    if 'colors_min' in cell:
        w, h, px = read_bmp(shot)
        colors = len({px(x, y) for y in range(0, h, 4) for x in range(0, w, 4)})
        if colors < cell['colors_min']:
            raise AssertionError(f'{name}: {colors} colors on the screen, expected at least {cell["colors_min"]}')
    if 'control' in cell and out.count('control mouse-to 1') != sum(1 for c in cell['control'] if c.startswith('mouse-to ')):
        raise AssertionError(f'{name}: the pointer did not reach a mouse-to position\n{out[-800:]}')
    for key, same in (('shots_same', True), ('shots_differ', False)):
        for a, b in cell.get(key, []):
            first, second = ((OS_DIR / f'{slug(name)}-{f}').read_bytes() for f in (a, b))
            if (first == second) != same:
                raise AssertionError(f'{name}: {a} and {b} {"differ" if same else "are the same"}')
    if cell.get('flash_written'):
        raw = nvram.read_bytes()
        if len(raw) != 273 + 0x20000 or raw[273 + 0x130C:273 + 0x1310] != b'NuMc':
            raise AssertionError(f'{name}: the flash memory holds no parameter RAM ({len(raw)} bytes in the PRAM file)')
    if cell.get('post_clean', family.get('post_clean')):
        raw = nvram.read_bytes()
        if len(raw) not in (273, 273 + 8192) or raw[:8] != b'LVDXPRAM':
            raise AssertionError(f'{name}: invalid RTC/PRAM record ({len(raw)} bytes)')
        pram_base = 273 + 0x1300 if len(raw) == 273 + 8192 else 17
        post = raw[pram_base + 0xF0:pram_base + 0x100]
        if any(post):
            raise AssertionError(f'{name}: POST failures logged in PRAM $F0-$FF: {post.hex()}')
    if cell.get('question_mark'):
        desktop_with_disk(shot, expected_size)
    if cell.get('chime'):
        chime_ok = any(t < 5 for t in wav_sounds(wav)) if sound_alert else wav_peak(wav) >= 4000
        if not chime_ok:
            raise AssertionError(f'{name}: the chime is silent in {wav.name}')
    if sound_alert and not wav_bipolar(wav, after=10, level=2000):
        raise AssertionError(f'{name}: no Sound Manager alert waveform in {wav.name}')
    if record_input:
        result = probe_found(out, '50425852')
        if len(result) < 36:
            raise AssertionError(f'{name}: no Sound Input Manager result')
        stage, _, opened, bits, channels, recorded, error = struct.unpack_from('>6h2xh', result, 4)
        count, first, closed, low, high = struct.unpack_from('>IIh2xhh', result, 20)
        if (stage != 5 or any((opened, bits, channels, recorded, error, closed)) or
                count != 8192 or first == 0x7bad5eed or not low < 0 < high or high - low < 512):
            raise AssertionError(f'{name}: recording failed: stage {stage}, API statuses '
                                 f'{(opened, bits, channels, recorded, error, closed)}, '
                                 f'{count} bytes, range {low}..{high}')
    want = {**family.get('gestalt', {}), **cell.get('gestalt', {})}
    if cell.get('guest_api') or cell.get('guest_floppy_api'):
        import ppc_g3_probe
        probe_data = probe_found(out, '47334750')
        result = ppc_g3_probe.decode(probe_data)
        if serial_probe:
            serial = ppc_g3_probe.serial_result(probe_data)
            if serial != (b'RXOK', 0, 4, 0, 5, 0, 0) or 'serial loopback: port 0 sent 5, received 4' not in out:
                raise AssertionError(f'{name}: guest serial exchange failed: {serial!r}')
        for selector, value in want.items():
            got = result.get(selector)
            if not got or got['error'] or got['value'] != value:
                raise AssertionError(f"{name}: guest Gestalt {selector}: {got}, expected {value}")
        if 'pram_setting' in cell and (result['pram_setting'] != cell['pram_setting'] or result['pram_write_error']):
            raise AssertionError(f'{name}: documented PRAM setting mismatch: {result}')
        if 'pram' in cell and (result['pram'] != cell['pram'] or result['pram_read_error'] or result['pram_write_error']):
            raise AssertionError(f'{name}: guest PRAM mismatch: {result}')
    elif 'mach' in want:
        m = re.search(r'^boxflag: (\d+)', out, re.M)
        if not m or int(m.group(1)) + 6 != want['mach']:
            raise AssertionError(f"{name}: Gestalt 'mach' {int(m.group(1)) + 6 if m else None}, expected {want['mach']}")
    if cell.get('manual_shutdown'):
        if 'powered off' in out or volumes(out):
            raise AssertionError(f'{name}: expected powered-on final shutdown screen with no mounted volumes')
        if not disk or not volume_attributes(disk) & 0x100:
            raise AssertionError(f'{name}: shutdown did not mark the volume unmounted cleanly')
        _, _, px = read_bmp(shot)
        if px(300, 300) != 0 or px(110, 94) != 0xFFFFFF:
            raise AssertionError(f'{name}: no final manual-power alert on black')
    missing = [v for v in cell.get('volumes', []) if v not in volumes(out)]
    if missing:
        raise AssertionError(f'{name}: not mounted: {", ".join(missing)} (mounted: {", ".join(volumes(out))})')
    for f, before in written.items():
        if digest(disk if f == 'disk' else OS_DIR / f) == before:
            raise AssertionError(f'{name}: nothing written to {"the disk copy" if f == "disk" else f}')
    if net:
        return net_results(name, net_probe_found(out), net[1], out, patched) + f'; {cell["seconds"]} s guest, {host:.1f} s host'
    ended = re.search(r'^end: .* at t=([0-9.]+)s', out, re.M)
    guest = f'{float(ended.group(1)):.0f}' if ended else cell['seconds']
    finder = re.search(r'^menu bar at t=([0-9.]+)s host=([0-9.]+)s', out, re.M)
    reached = f'; Finder {float(finder[1]):.0f} s guest, {float(finder[2]):.1f} s host' if finder else ''
    return f'{name}: {guest} s guest, {host:.1f} s host' + reached + (f'; {cell["note"]}' if 'note' in cell else '')


def run_cell_inner(family: dict, cell: dict, state: dict) -> str:
    """One cell of a boot-matrix file: runs it and checks what it expects.
    Returns its report line. The cell's disk copy is deleted afterwards
    unless a later cell continues from it."""
    try:
        return run_cell_checks(family, cell, state)
    finally:
        disk = state['cells'].get(cell['name'], (None, None))[0]
        if disk and cell['name'] not in state['continued']:
            remove(disk)


def run_cell_checks(family: dict, cell: dict, state: dict) -> str:
    if family.get('runner') == 'ppc':
        return run_ppc_cell(family, cell, state)
    name = cell['name']
    rom = ROMS / cell.get('rom', family['rom'])
    model = cell.get('model', family['model'])
    res = cell.get('resolution', '640x480')
    depth = cell.get('depth', 8)
    width, height = (int(v) for v in res.split('x'))
    ram = cell.get('ram_mb', family.get('ram_mb', 8))
    card = cell.get('video_card')
    machine_args = ['--model', model, '--ram', ram]
    video_args = ['--resolution', res, '--depth', depth] + (['--video-card', card] if card else [])
    # The disk and PRAM: copies made for this cell, or an earlier cell's.
    if 'continue' in cell:
        disk, nvram = state['cells'][cell['continue']]
    else:
        disk = None
        # A PRAM the ROM initialised once at this screen, so the configured
        # depth applies (the first start of a fresh PRAM is in 1 bit), or
        # with fresh_pram none: Livadix makes a new PRAM file.
        key = (model, rom.name, res, card or '')
        if not cell.get('fresh_pram') and key not in state['pram']:
            path = OS_DIR / f'pram-{slug("-".join(key))}.nvram'
            path.unlink(missing_ok=True)
            os_trace(f'{name} PRAM', rom, machine_args + ['--max', 60_000_000] + video_args + ['--nvram', path])
            state['pram'][key] = path
        if cell.get('fresh_pram'):
            nvram = OS_DIR / f'{slug(name)}.nvram'
            nvram.unlink(missing_ok=True)
        else:
            nvram = copy_of(state['pram'][key], f'{slug(name)}.nvram')
    state['cells'][name] = (disk, nvram)
    if cell.get('left_mounted') and volume_attributes(disk) & 0x100:
        raise AssertionError(f'{name}: the disk it starts from is marked unmounted cleanly, not left mounted')
    shot, wav = OS_DIR / f'{slug(name)}.bmp', OS_DIR / f'{slug(name)}.wav'
    shot.unlink(missing_ok=True)
    written = {f: digest(disk if f == 'disk' else OS_DIR / f) for f in cell.get('written', [])}
    want_gestalt = {**family.get('gestalt', {}), **cell['gestalt']} if 'gestalt' in cell else {}
    want_bits = cell.get('gestalt_bits', {})
    sound = cell.get('chime') or cell.get('alert') or 'silent' in cell or 'chimes' in cell
    if cell.get('runner') == 'livadix':
        # livadix.exe itself, hidden and unthrottled.
        config = OS_DIR / f'{slug(name)}.yaml'
        # A profile file: shared keys, and the cell's profile with its model and disk.
        config.write_text(f"emulation:\n  rom: '{posix(rom)}'\nvideo:\n  depth: {depth}\n"
                          f"profiles:\n  other:\n    model: iici\n  \"{name}\":\n    model: {model}\n"
                          f"    emulation:\n      ram_mb: {ram}\n      nvram: '{posix(nvram)}'\n"
                          f"    video:\n      resolution: {res}\n"
                          + (f'      card: {card}\n' if card else '') + (f'      look: {cell["look"]}\n' if 'look' in cell else '') +
                          f"    storage:\n      disks:\n        - image: '{posix(disk)}'\n"
                          + ('          bus: ata\n' if cell.get('bus') == 'ata' else ''), encoding='utf-8')
        import time
        started = time.perf_counter()
        run([LIVADIX, '--config', config, '--profile', name, '--hidden', '--unthrottled', '--frames', cell['frames'],
             '--screenshot', shot] + (['--wav', wav] if sound else []), timeout=600)
        seconds = time.perf_counter() - started
        if cell.get('menu_bar'):
            finder_menu_bar(shot, shot_size(cell, width, height))
        if 'shot_size' in cell:
            got = read_bmp(shot)[:2]
            if got != tuple(int(v) for v in cell['shot_size'].split('x')):
                raise AssertionError(f'{name}: the screen is {got[0]}x{got[1]}, expected {cell["shot_size"]}')
        if cell.get('chime') and wav_peak(wav) < 4000:
            raise AssertionError(f'{name}: the chime is silent in {wav.name}')
        return f'{name}: livadix.exe --hidden --unthrottled, {cell["frames"]} frames, {seconds:.1f} s host' + \
            (f'; {cell["note"]}' if 'note' in cell else '')
    args = machine_args + ['--max', cell['max']] + video_args + ['--nvram', nvram]
    if disk:
        args += ['--floppy' if cell.get('floppy') else '--disk-ata' if cell.get('bus') == 'ata' else '--disk', disk]
    if 'cdrom' in cell:
        args += ['--cdrom', OS_DIR / cell['cdrom']]
    if 'finder' in cell:
        args += ['--finder', cell['finder']]
    args += ['--shot', shot] + (['--wav', wav] if sound else []) + (['--no-chime'] if 'silent' in cell else [])
    if 'boot_volume' in cell or 'volumes' in cell:
        args.append('--volumes')
    if 'run_code' in cell:
        args += ['--run-code', ROOT / 'src' / 'hw' / 'guest' / 'bin' / f'{cell["run_code"]}.bin']
    for sel in list(want_gestalt) + [s for s in want_bits if s not in want_gestalt]:
        args += ['--gestalt', sel]
    if 'alerts' in cell:
        args.append('--alerts')
    if 'mouse_at' in cell:
        args += ['--peek', '0x830', '1']
    if 'lowmem_byte' in cell:
        args += ['--peek', f'0x{next(iter(cell["lowmem_byte"]))}', '1']
    args += list(cell.get('args', [])) + cell_inputs(cell.get('input', []))
    out = os_trace(name, rom, args, end='; powered off' if cell.get('powered_off') else '; ok')
    if 'alerts' in cell:
        # Every alert the System put up, whenever it came and whatever
        # closed it: only the cell's expected ones.
        shown = re.findall(r'^alert: trap [0-9A-F]{4}, (ALRT|SysError) (-?\d+)', out, re.M)
        unexpected = [f'{kind} {n}' for kind, n in shown if kind != 'ALRT' or int(n) not in cell['alerts']]
        if unexpected:
            raise AssertionError(f'{name}: alerts shown: {", ".join(unexpected)}')
    if cell.get('asleep') and 'asleep: waking with the next input' not in out:
        raise AssertionError(f'{name}: the Macintosh never slept')
    report = name
    if 'finder' in cell:
        at, guest, host = finder_line(name, out)
        report += f': Finder after {at:,} instructions, {guest:.1f} s guest, {host:.1f} s host'
    if cell.get('screen'):
        want = cell['screen'] if isinstance(cell['screen'], str) else f'{res}, {depth} bits'
        if f'screen: {want}' not in out:
            raise AssertionError(f'{name}: {screen_line(out)}, expected screen: {want}')
    if cell.get('menu_bar'):
        finder_menu_bar(shot, shot_size(cell, width, height))
    if cell.get('question_mark'):
        desktop_with_disk(shot, (width, height))
    got = gestalts(out)
    for sel, want in want_gestalt.items():
        if got.get(sel) != (want, 0):
            raise AssertionError(f"{name}: Gestalt '{sel}' {got.get(sel)}, expected {want}")
    for sel, bits in want_bits.items():
        if not got.get(sel, (0, 1))[0] & bits:
            raise AssertionError(f"{name}: Gestalt '{sel}' {got.get(sel)}, expected bits {bits:#x}")
    vols = volumes(out)
    if 'boot_volume' in cell and vols[:1] != [cell['boot_volume']]:
        raise AssertionError(f'{name}: volumes {vols}, expected {cell["boot_volume"]} first')
    for v in cell.get('volumes', []):
        if v not in vols:
            raise AssertionError(f'{name}: volume {v} not mounted: {vols}')
    if cell.get('unmounted') and not volume_attributes(disk) & 0x100:
        raise AssertionError(f'{name}: the disk is not marked unmounted cleanly (drAtrb bit 8)')
    for at, want in cell.get('lowmem_byte', {}).items():
        m = re.search(r'^peek ([0-9A-F]{2})', out, re.M)
        if not m or int(m.group(1), 16) != want:
            raise AssertionError(f'{name}: low memory ${at} is {m.group(1) if m else "?"}, expected {want:02X}')
    if 'mouse_at' in cell:
        x, y = (int(v) for v in cell['mouse_at'].split(','))
        m = re.search(r'^peek ([0-9A-F]{8})', out, re.M)
        if not m or int(m.group(1), 16) != (y << 16 | x):
            raise AssertionError(f'{name}: the cursor (low memory Mouse) is at {m.group(1) if m else "?"}, expected {x},{y}')
    if cell.get('post_clean'):
        pram = nvram.read_bytes()[17:]
        if any(pram[0xF0:0x100]):
            raise AssertionError(f'{name}: POST failures logged in PRAM $F0-$FF: {pram[0xF0:0x100].hex()}')
    if 'post_log' in cell:
        pram = nvram.read_bytes()[17:]
        for at, want in cell['post_log'].items():
            if pram[int(at, 16)] != want:
                raise AssertionError(f'{name}: PRAM ${at} is {pram[int(at, 16)]:#04x}, expected {want:#04x} ($F0-$FF: {pram[0xF0:0x100].hex()})')
    if cell.get('chime'):
        m = re.search(r'sound: \d+ frames at (\d+) Hz, peak (\d+), first sound at (-?[0-9.]+) s', out)
        if not m or int(m.group(2)) < 4000 or not 0 <= float(m.group(3)) < 0.5 or wav_peak(wav) != int(m.group(2)):
            raise AssertionError(f'{name}: startup chime {m.group(0) if m else "not recorded"}')
        report += f'; chime peak {m.group(2)} from {float(m.group(3)):.2f} s'
    if 'chimes' in cell:
        # The machine started as many times as it chimed: one start, no
        # reset into a second boot on the way to the Finder.
        starts = wav_sounds(wav)
        if len(starts) != cell['chimes']:
            raise AssertionError(f'{name}: {len(starts)} chimes (at {starts} s), expected {cell["chimes"]}')
        report += f'; {len(starts)} chime{"s" if len(starts) != 1 else ""}'
    if cell.get('alert'):
        # A sound a second after the Finder came up: an alert the System played.
        _, guest, _ = finder_line(name, out)
        if wav_peak(wav, guest + 1) < 4000:
            raise AssertionError(f'{name}: no sound after the Finder came up')
    if 'silent' in cell:
        m = re.search(r'peak (\d+), first sound at -1', out)
        if not m or int(m.group(1)) > cell['silent']:
            raise AssertionError(f'{name}: ' + next((l for l in out.splitlines() if l.startswith('sound:')), 'no sound line'))
    for f, before in written.items():
        if digest(disk if f == 'disk' else OS_DIR / f) == before:
            raise AssertionError(f'{name}: nothing written to {"the disk copy" if f == "disk" else f}')
    for f in cell.get('hfs', []):
        if (OS_DIR / f).read_bytes()[1024:1026] != b'BD':
            raise AssertionError(f'{name}: {f} holds no HFS volume')
    if 'fpsp_save' in cell:
        state['probes'][cell['fpsp_save']] = fpsp_results(name, out)
    if 'fpsp_compare' in cell:
        differ = compare_probe(state['probes'][cell['fpsp_compare']], fpsp_results(name, out))
        report += (f'; the FPSP probe agrees with {cell["fpsp_compare"]} (transcendentals within the FPSP accuracy, '
                   f'{len(differ)} of {len(PROBE_TRANSCENDENTAL)} not bit-identical)')
    if 'probe' in cell:
        # A probe's results: big-endian longwords at the offsets given.
        data = probe_results(name, out)
        for at, want in cell['probe'].items():
            off = int(at, 16)
            v = int.from_bytes(data[off:off + 4], 'big')
            if v != want:
                raise AssertionError(f'{name}: probe result at {at} is {v:#x}, expected {want:#x}')
    if 'moved' in cell:
        # The screen differs from an earlier cell's inside the box: the
        # cursor moved (the main screen's VBL task draws it).
        _, _, a = read_bmp(OS_DIR / f'{slug(cell["moved"])}.bmp')
        _, _, b = read_bmp(shot)
        x0, y0, x1, y1 = cell.get('moved_box', [0, 0, 200, 200])
        if not any(a(x, y) != b(x, y) for y in range(y0, y1) for x in range(x0, x1)):
            raise AssertionError(f'{name}: the cursor did not move')
    if 'pixels' in cell:
        _, _, px = read_bmp(shot)
        for at, want in cell['pixels'].items():
            x, y = (int(v) for v in at.split(','))
            if px(x, y) != want:
                raise AssertionError(f'{name}: pixel ({x}, {y}) is {px(x, y):06x}, expected {want:06x}')
    if 'note' in cell:
        report += f'; {cell["note"]}'
    return report


# Every model's clean startup chime: (output rate, seconds, step energy,
# largest step, samples at full scale) and the models that play it. The
# EASC's compressed chime, the PowerBook 190's 16-bit one, the AVs' (the
# PSC's samples as the ROM holds them, some at full scale) and the ASC's
# beeps. The 190's lasts 2.1 s and its last sample (a few hundred on the
# right) is held until the ROM sets the sound up again at 3.5 s and writes
# a zero sample to each FIFO (+$1000, +$1800): its length runs to there.
CHIMES = [
    ((44100, 1.39, 0.0200, 0.42, 0), 'centris-610 centris-650 lc-475 lc-575 lc-580 lc-630 performa-475 performa-476 '
     'performa-575 performa-577 performa-578 performa-580 performa-588 performa-630 powerbook-520 powerbook-520c '
     'powerbook-540 powerbook-540c quadra-605 quadra-610 quadra-630 quadra-650 quadra-700 quadra-800 quadra-900 quadra-950'),
    ((44100, 3.13, 0.0047, 0.23, 0), 'powerbook-190 powerbook-190cs'),
    ((44100, 1.22, 0.0076, 0.25, 207), 'quadra-660av quadra-840av'),
    ((22255, 1.38, 0.0389, 0.47, 0), 'iivi iivx performa-600'),
    ((22255, 0.78, 0.0006, 0.17, 0), 'mac-ii-fdhd mac-ii-v1 mac-ii-v2'),
    ((22255, 0.77, 0.0005, 0.11, 0), 'mac-iicx mac-iix se-30'),
    ((22255, 0.76, 0.0005, 0.11, 0), 'lc'),
    ((22255, 0.72, 0.0004, 0.14, 0), 'classic-ii color-classic lc-ii powerbook-140'),
    ((22255, 0.64, 0.0004, 0.11, 0), 'iisi'),
    ((22255, 0.57, 0.0004, 0.11, 0), 'iici lc-iii-v1 lc-iii lc-520 powerbook-145 powerbook-145b powerbook-160 powerbook-170 powerbook-duo-210'),
    ((22255, 0.51, 0.0003, 0.11, 0), 'color-classic-ii lc-iii-plus lc-550 performa-275 performa-550 performa-560 powerbook-150 '
     'powerbook-165 powerbook-165c powerbook-180 powerbook-180c powerbook-duo-230 powerbook-duo-250 powerbook-duo-270c'),
    ((22255, 0.51, 0.0003, 0.12, 0), 'macintosh-tv'),
    ((22255, 0.47, 0.0004, 1.00, 0), 'iifx'),
    ((22255, 0.38, 0.0004, 1.00, 0), 'powerbook-duo-280 powerbook-duo-280c'),
    # The compact Macs' boot beep from the sound buffer (video/compactvideo.h).
    ((22255, 0.70, 0.0448, 0.87, 0), 'mac-512ke mac-plus mac-plus-v1 mac-plus-v2'),
    ((22255, 0.68, 0.0353, 0.64, 0), 'mac-128k mac-512k'),
    # The Portable's and PowerBook 100's ASC, once the PMU has powered the machine.
    ((22255, 0.98, 0.0007, 0.18, 0), 'mac-portable powerbook-100'),
    ((22255, 0.68, 0.0424, 0.88, 0), 'mac-classic mac-se mac-se-fdhd')]


def chime_metrics(path: Path):
    """A chime's measures: the WAV's rate, when the sound starts and how
    long it lasts (seconds), the loudest sample, the samples at full scale,
    and over 10-500 ms after the start the energy of the sample steps
    against the signal's (distortion and a higher pitch raise it) and the
    largest step against the loudest sample."""
    import array
    import wave
    with wave.open(str(path)) as w:
        rate = w.getframerate()
        s = array.array('h', w.readframes(w.getnframes()))
    if sys.byteorder == 'big':
        s.byteswap()
    start = next((i for i in range(0, len(s) - 1, 2) if abs(s[i]) > 200 or abs(s[i + 1]) > 200), None)
    if start is None:
        return None
    # The end: the last sample 200 away from the level the output rests at.
    end = next(i for i in range(len(s) - 2, start - 2, -2) if abs(s[i] - s[-2]) > 200 or abs(s[i + 1] - s[-1]) > 200)
    a, b = start + 2 * (rate // 100), min(len(s), start + 2 * (rate // 2))
    peak = max((abs(v) for v in s[a:b]), default=0)
    steps = [s[i + 2] - s[i] for i in range(a, b - 2)]
    return {'rate': rate, 'start': start / 2 / rate, 'seconds': (end - start) / 2 / rate,
            'clipped': sum(1 for v in s if v in (32767, -32768)),
            'steps': sum(d * d for d in steps) / max(sum(v * v for v in s[a:b]), 1),
            'jump': max((abs(d) for d in steps), default=0) / max(peak, 1)}


def chime_fault(m, want) -> str:
    """What is wrong with a chime against its clean measures, or ''."""
    rate, seconds, steps, jump, clipped = want
    if m is None:
        return 'silent'
    faults = []
    if m['rate'] != rate:
        faults.append(f'{m["rate"]} Hz, expected {rate}')
    if abs(m['seconds'] - seconds) > 0.03:
        faults.append(f'{m["seconds"]:.3f} s long, expected {seconds}')
    if m['clipped'] > clipped:
        faults.append(f'{m["clipped"]} samples at full scale, expected {clipped}')
    if not steps * 0.8 <= m['steps'] <= steps * 1.25:
        faults.append(f'step energy {m["steps"]:.4f}, expected {steps}')
    if m['jump'] > jump * 1.1 + 0.02:
        faults.append(f'largest step {m["jump"]:.2f} of the peak, expected {jump}')
    return '; '.join(faults)


def chime_models() -> tuple:
    """(model -> its ROM file or None, model -> its clean chime's measures)."""
    roms = {}
    for rom in sorted(ROMS.glob('*.rom')) if ROMS.is_dir() else []:
        roms.setdefault(rom.read_bytes()[:4].hex().upper(), rom)
    listed = run([LIVADIX, '--list-models']).stdout.splitlines()
    model_rom = {}
    for line in listed:
        checksum = re.search(r'\s([0-9A-F]{8})\s', line)   # the ROM column, after the name
        # Models not emulated yet have no chime; the PowerPC ones' chimes are
        # their boot matrix's.
        # The Built-in machine's ROM keeps the sound off (no chime).
        if checksum and not line.rstrip().endswith('not yet') and ' PowerPC ' not in line and not line.startswith('builtin '):
            model_rom[line.split()[0]] = roms.get(checksum.group(1))
    return model_rom, {model: ref for ref, models in CHIMES for model in models.split()}


def chime_prepare():
    """The controller firmware and declaration ROMs some models need, found
    in roms/ beside the configuration (copies: roms/ is only read)."""
    folder = WORK / 'chime'
    (folder / 'roms').mkdir(parents=True, exist_ok=True)
    for f in ROMS.iterdir() if ROMS.is_dir() else []:
        if f.is_file() and f.suffix.lower() != '.rom':
            copy = folder / 'roms' / f.name
            if not copy.exists() or copy.stat().st_size != f.stat().st_size:
                copy.write_bytes(f.read_bytes())


# The models of the quick tier: one of each clean chime's group that differs in the sound's source
# (the ASC's, the EASC's, the AV's PSC, the 16-bit one).
QUICK_CHIMES = ['quadra-700', 'iici', 'lc', 'powerbook-190', 'quadra-840av', 'iifx', 'iivx', 'mac-ii-v2', 'iisi']


def chime_units() -> list:
    model_rom, _ = chime_models()
    models = [m for m, rom in model_rom.items() if rom and (not QUICK or m in QUICK_CHIMES)]
    return ['table'] + models + (['quadra-700-paced'] if model_rom.get('quadra-700') else [])


def check_chime():
    model_rom, wanted = chime_models()
    missing = sorted(set(model_rom) - set(wanted))
    if missing:
        raise AssertionError(f'models without a clean chime in CHIMES: {missing}')
    folder = WORK / 'chime'
    # The controller firmware and declaration ROMs some models need, found
    # in roms/ beside the configuration (copies: roms/ is only read).
    chime_prepare()
    faults, done = [], 0

    def record(model, name, paced=False):
        import time
        t0 = time.perf_counter()
        try:
            record_inner(model, name, paced)
        finally:
            TIMINGS['chime'][name] = round(time.perf_counter() - t0, 2)

    def record_inner(model, name, paced=False):
        wav = folder / f'{name}.wav'
        wav.unlink(missing_ok=True)
        config = write_config(f'chime/{name}.yaml', f"model: {model}\nemulation:\n  rom: '{posix(model_rom[model].resolve())}'\n")
        run([LIVADIX, '--config', config, '--hidden', '--frames', 240 if paced else 300, '--wav', wav]
            + ([] if paced else ['--unthrottled']), timeout=300)
        fault = chime_fault(chime_metrics(wav), wanted[model])
        if fault:
            faults.append(f'{name}: {fault}')

    for model, rom in model_rom.items():
        if rom and want(model) and (UNIT is not None or not QUICK or model in QUICK_CHIMES):
            record(model, model)
            done += 1
    if not done and UNIT is None:
        raise Skip('no model ROM in roms/')
    paced = bool(model_rom.get('quadra-700')) and want('quadra-700-paced')
    if paced:
        record('quadra-700', 'quadra-700-paced', paced=True)
    if faults:
        raise AssertionError('\n'.join(faults))
    if UNIT is None:
        print(f'    {done} models: clean chimes' + (', the Quadra 700 paced too' if paced else ''))
    elif UNIT != 'table':
        print(f'    {UNIT}: clean chime')


def matrix_chains(family: dict) -> list:
    """The cells of a boot-matrix file in groups that must run together and
    in file order: a cell that continues from another's disk, compares with
    another's screen (`moved`) or with another's FPSP results."""
    cells = family.get('cell', [])
    names = [c['name'] for c in cells]
    saved = {c['fpsp_save']: c['name'] for c in cells if 'fpsp_save' in c}
    parent = list(range(len(cells)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for i, c in enumerate(cells):
        for key in ('continue', 'moved'):
            if key in c:
                parent[find(i)] = find(names.index(c[key]))
        if 'fpsp_compare' in c:
            parent[find(i)] = find(names.index(saved[c['fpsp_compare']]))
    chains = {}
    for i in range(len(cells)):
        chains.setdefault(find(i), []).append(cells[i])
    return list(chains.values())


def chain_selected(family: dict, stem: str, chain: list):
    """The chain's cells to run under the filters (--family, --cell, the
    quick tier's `quick` cells) with the cells they depend on, or [] when
    none is selected."""
    if FILTER['family'] and stem not in FILTER['family']:
        return []
    if FILTER['cell']:
        picked = [c for c in chain if any(f in c['name'] or f == slug(c['name']) for f in FILTER['cell'])]
    elif QUICK:
        picked = [c for c in chain if c.get('quick')]
    else:
        return chain
    if not picked:
        return []
    needed = {c['name'] for c in picked}
    saved = {c['fpsp_save']: c['name'] for c in chain if 'fpsp_save' in c}
    for c in reversed(chain):
        if c['name'] in needed:
            for key in ('continue', 'moved'):
                if key in c:
                    needed.add(c[key])
            if 'fpsp_compare' in c:
                needed.add(saved[c['fpsp_compare']])
    return [c for c in chain if c['name'] in needed]


def os_boot_units() -> list:
    units = []
    for path, family in matrix_files():
        for chain in matrix_chains(family):
            if chain_selected(family, path.stem, chain):
                units.append(f'{path.stem}/{chain[0]["name"]}')
    return units


def check_os_boot():
    files = matrix_files()
    if not files:
        raise AssertionError(f'no boot-matrix files in {BOOT_MATRIX}')
    OS_DIR.mkdir(parents=True, exist_ok=True)
    for old in OS_DIR.iterdir():
        old.unlink()
    report, skipped = [], []
    state = {'pram': {}, 'cells': {}, 'probes': {},
             'continued': {c['continue'] for _, f in files for c in f.get('cell', []) if 'continue' in c}}
    for path, family in files:
        wanted = [c for chain in matrix_chains(family) for c in chain_selected(family, path.stem, chain)
                  if want(f'{path.stem}/{chain[0]["name"]}')]
        if not wanted:
            continue
        cells = [c for c in family.get('cell', []) if c in wanted]
        # Cells start from blank media only: an existing disk image is refused.
        for c in cells:
            for k in ('system', 'install', 'install_work', 'disk_image'):
                if k in c:
                    raise AssertionError(f'{path.stem}/{c["name"]}: `{k}` names an existing image; cells use blank media only')
        for name, media in family.get('media', {}).items():
            if 'source_work' in media or 'source_data' in media:
                raise AssertionError(f'{path.stem}: media {name} is an existing image; cells use blank media only')
        rom = next((ROMS / r for r in [family['rom']] + [c['rom'] for c in cells if 'rom' in c] if not (ROMS / r).exists()),
                   ROMS / family['rom'])
        if not rom.exists():
            skipped.append(f'{path.name} (no {rom.name})')
            continue
        blank = [sys.executable, ROOT / 'tests' / 'hfs_blank.py']
        for name, media in family.get('media', {}).items():
            if 'name' in media:
                run(blank + [OS_DIR / name, media['kib'], media['name']])
            else:
                (OS_DIR / name).write_bytes(bytes(media['kib'] * 1024))
            if media.get('g3_probe'):
                import ppc_g3_probe
                image = OS_DIR / name
                with image.open('r+b') as f:
                    f.seek(1024 + 92)
                    f.write(struct.pack('>I', 2))
                startup_application(image, 'Livadix G3 Probe', PROBE_PROLOGUE + ppc_g3_probe.program(), subfolder='')
        for cell in cells:
            report.append(run_cell(family, cell, state))
    if not report:
        raise Skip('skipped ' + ', '.join(skipped))
    print(indented('\n'.join(report + [f'skipped {s}' for s in skipped])))


def volume_attributes(image: Path) -> int:
    """The HFS master directory block's drAtrb (a bare volume)."""
    with open(image, 'rb') as f:
        f.seek(1024)
        mdb = f.read(12)
    if mdb[:2] != b'BD':
        raise AssertionError(f'{image.name}: no HFS volume')
    return int.from_bytes(mdb[10:12], 'big')


def check_cpu_speed():
    out = run([CPU_TESTS, 'bench'], timeout=300).stdout
    m = re.search(r'interpreter: ([0-9.]+) MIPS', out)
    if not m:
        raise AssertionError(out)
    print(indented(out))
    if float(m.group(1)) < MIN_MIPS:
        raise AssertionError(f'{m.group(1)} MIPS, below {MIN_MIPS}')


JIT_DIFF_CASES = 20000
JIT_DIFF_SLICES = 20        # the programs split over processes: slice i takes the seeds from 1 + i * size
JIT_DIFF_QUICK = (4, 250)   # the quick tier: the first 250 programs of slices 0-3


def jit_diff_units() -> list:
    return [str(i) for i in range(JIT_DIFF_QUICK[0] if QUICK else JIT_DIFF_SLICES)]


def owned_nvram(base: Path) -> bytes:
    """The board payload Livadix saved for a configuration's emulation.nvram
    base: the owned record beside it, <stem>-<model>-<ROM>-<hash>.nvram,
    after its header (the record format)."""
    records = [p for p in base.parent.glob(f'{base.stem}-*.nvram') if p.is_file()]
    if len(records) != 1:
        raise AssertionError(f'{base.name}: {len(records)} owned NVRAM records beside it')
    raw = records[0].read_bytes()
    if raw[:8] != b'LVDXNV2\0':
        raise AssertionError(f'{records[0].name}: not an owned NVRAM record')
    return raw[int.from_bytes(raw[8:12], 'big'):]


def check_jit_diff():
    size = JIT_DIFF_CASES // JIT_DIFF_SLICES
    first = 0 if UNIT is None else int(UNIT)
    last = JIT_DIFF_SLICES if UNIT is None else first + 1
    for i in range(first, last):
        p = run([CPU_TESTS, 'jit', JIT_DIFF_QUICK[1] if QUICK else size, 1 + i * size, 1000000], check=False, timeout=1800)
        if p.returncode != 0:
            raise AssertionError(p.stdout[-4000:] + p.stderr[-2000:])
        print(indented(f'slice {i}: ' + p.stdout.strip().splitlines()[-1]))


# --- the PowerPC translator (phase 6.6) ------------------------------------------------------

PPC_JIT_TESTS = BUILD / ('mc_ppc_jit_tests' + EXE)
PPC_MODELS = ['601', '603', '603e', '604', '604e', '750']
PPC_JIT_PROGRAMS = (40, 600)        # random programs per model: quick tier, full
PPC_JIT_SHORT = (10, 100)           # the same in one-instruction blocks
PPC_JIT_ORACLE = (8, 64)            # oracle.py's programs per model


def ppc_jit_diff_units() -> list:
    return PPC_MODELS + ['oracle']


def ppc_oracle_programs(path: Path, per_model: int):
    """oracle.py's generator (tests/ppc/oracle.py, program()), the
    programs mc_ppc_jit_tests oracle reads: a count, then each program's
    model and length (little-endian) and its words (big-endian)."""
    import random
    import struct
    sys.path.insert(0, str(ROOT / 'tests' / 'ppc'))
    import oracle as ppc_oracle
    out = []
    for m, model in enumerate(PPC_MODELS):
        rng = random.Random(6600 + m)
        for index in range(per_model):
            code = ppc_oracle.program(rng, 128, index & 3, model)
            out.append(struct.pack('<II', m, len(code)) + struct.pack(f'>{len(code)}I', *code))
    path.write_bytes(struct.pack('<I', len(out)) + b''.join(out))


def check_ppc_jit_diff():
    tier = 0 if QUICK else 1
    for unit in ([UNIT] if UNIT else ppc_jit_diff_units()):
        if unit == 'oracle':
            work = WORK / 'ppc-jit'
            work.mkdir(parents=True, exist_ok=True)
            programs = work / 'oracle-programs.bin'
            ppc_oracle_programs(programs, PPC_JIT_ORACLE[tier])
            runs = [[PPC_JIT_TESTS, 'oracle', programs]]
        else:
            seed = 66 + PPC_MODELS.index(unit) * 100000
            runs = [[PPC_JIT_TESTS, 'diff', PPC_JIT_PROGRAMS[tier], seed, unit],
                    [PPC_JIT_TESTS, 'diff', PPC_JIT_SHORT[tier], seed + 50000, unit, 1]]
        for args in runs:
            p = run(args, check=False, timeout=1800)
            if p.returncode == 77:
                raise Skip(p.stdout.strip())
            if p.returncode != 0:
                raise AssertionError(p.stdout[-4000:] + p.stderr[-2000:])
            print(indented(f'{unit}: ' + p.stdout.strip().splitlines()[-1]))


def check_ppc_jit_rom():
    rom = ROMS / 'power-macintosh-6100.rom'
    if not rom.exists():
        raise Skip('no roms/power-macintosh-6100.rom')
    p = run([PPC_JIT_TESTS, 'rom', rom], check=False, timeout=300)
    if p.returncode == 77:
        raise Skip(p.stdout.strip())
    if p.returncode != 0:
        raise AssertionError(p.stdout[-4000:] + p.stderr[-2000:])
    print(indented(p.stdout.strip()))


def check_ppc_finder():
    p = run([sys.executable, ROOT / 'tests' / 'test_ppc_finder.py'], check=False)
    if p.returncode:
        raise AssertionError(p.stdout + p.stderr)
    print(indented(p.stdout.strip()))


def check_ppc_grackle():
    exe = BUILD / ('mc_ppc_grackle_tests' + EXE)
    p = run([exe], check=False)
    if p.returncode:
        raise AssertionError(p.stdout + p.stderr)
    print(indented(p.stdout.strip()))
    for name in ('power-macintosh-g3.rom', 'power-macintosh-g3-v1.rom'):
        if not (ROMS / name).exists():
            print(indented(f'{name}: probe skipped (ROM absent)'))
            continue
        p = run([exe, 'probe', ROMS / name], check=False, timeout=60)
        if p.returncode:
            raise AssertionError(p.stdout + p.stderr)
        print(indented(name + ': ' + p.stdout.strip()))


# --- the guest probes of the cells ------------------------------------------------------

# A 68k application's start: its QuickDraw globals below A5 (InitGraf with
# thePort at -4(A5), InitFonts; Inside Macintosh I, "QuickDraw" and "The
# Font Manager"), as the probes find them in the Finder.
PROBE_PROLOGUE = struct.pack('>HHHH', 0x486D, 0xFFFC, 0xA86E, 0xA8FE)


def probe_found(out: str, word: str) -> bytes:
    """The results the long `word` (hex) starts, from mc_ppc_pdm_boot --find:
    the copy that is not the immediate of a move.l #,(An) ($20BC + An)."""
    for l in out.splitlines():
        m = re.match(r'found [0-9A-F]+ after ([0-9A-F]{4}):((?: [0-9A-F]{2})+)', l)
        if m and int(m.group(1), 16) & 0xF1FF != 0x20BC and m.group(2).replace(' ', '').startswith(word):
            return bytes(int(b, 16) for b in m.group(2).split())
    return b''


def parallel(tasks: list) -> list:
    """The results of the calls (function, arguments...) run side by side."""
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, physical_cores() - 1)) as pool:
        return [f.result() for f in [pool.submit(*task) for task in tasks]]


# --- the cells' Ethernet probe through the NAT (src/net) -----------------------------

NET_PROBE = ROOT / 'src' / 'hw' / 'guest' / 'bin' / 'net_probe.bin'


def resource_forks(img: bytes, types: list) -> list:
    """Offsets of the resource forks, on 512-byte boundaries, whose resource
    map lists every one of `types` (Inside Macintosh I, "The Resource
    Manager": the header, its copy at the map, the type list)."""
    found, pos = [], 0
    while True:
        pos = img.find(b'\x00\x00\x01\x00', pos)
        if pos < 0:
            return found
        if pos % 512 == 0:
            doff, moff, dlen, mlen = struct.unpack('>IIII', img[pos:pos + 16])
            if doff == 256 and 0 < mlen < 65536 and moff >= 256 + dlen and img[pos + moff:pos + moff + 16] == img[pos:pos + 16]:
                m = img[pos + moff:pos + moff + mlen]
                tl = m[struct.unpack('>H', m[24:26])[0]:]
                names = {tl[2 + 8 * i:6 + 8 * i] for i in range(struct.unpack('>H', tl[:2])[0] + 1)}
                if all(t in names for t in types):
                    found.append(pos)
        pos += 4


def resource_data(img: bytes, fork: int, rtype: bytes, rid: int):
    """(offset, length) of a resource's data in the image, or None."""
    doff, moff, dlen, mlen = struct.unpack('>IIII', img[fork:fork + 16])
    m = img[fork + moff:fork + moff + mlen]
    tl = m[struct.unpack('>H', m[24:26])[0]:]
    for i in range(struct.unpack('>H', tl[:2])[0] + 1):
        t, cnt, ref = struct.unpack('>4sHH', tl[2 + 8 * i:10 + 8 * i])
        for k in range(cnt + 1) if t == rtype else ():
            r = tl[ref + 12 * k:ref + 12 * k + 12]
            if struct.unpack('>h', r[:2])[0] == rid:
                off = fork + doff + (struct.unpack('>I', r[4:8])[0] & 0xFFFFFF)
                return off + 4, struct.unpack('>I', img[off:off + 4])[0]
    return None


def enable_tcpip(path: Path, built_in=True) -> int:
    """Open Transport's TCP/IP made active on the work copy, as its control
    panel saves it (observed: the TCP/IP Preferences file's 'unld' 128
    loses bit 1, "inactive", and the port name in 'iitf' 128, "enet"
    stored with length 5, becomes "enet0" for the built-in Ethernet). With
    a card (built_in False) the name stays "enet", the default port, which
    Open Transport gives the card (observed on the LC 475). Returns the
    files changed."""
    img = bytearray(path.read_bytes())
    done = 0
    for fork in resource_forks(bytes(img), [b'iitf', b'unld', b'port', b'prot']):
        doff, moff, dlen, mlen = struct.unpack('>IIII', img[fork:fork + 16])
        u, i = resource_data(img, fork, b'unld', 128), resource_data(img, fork, b'iitf', 128)
        if moff + mlen > 4096 or not u or not i:
            continue            # the TCP/IP control panel's own defaults
        img[u[0] + u[1] - 1] &= ~2
        if built_in and img[i[0] + 13] == 5 and img[i[0] + 18] == 0:
            img[i[0] + 18] = ord('0')
        done += 1
    path.write_bytes(bytes(img))
    return done


def hfs_fork(img: bytes, name: str, rsrc=True) -> list:
    """The image offsets of a file's fork as (offset, length) runs, from
    the catalog's leaf records (Inside Macintosh: Files, "Data Organization
    on Volumes": the master directory block at 1024 with drAlBlkSiz at +20,
    drAlBlSt at +28, the extents overflow file's size and first extents at
    +130 and +134, the catalog's at +146 and +150; leaf nodes of 512 bytes;
    a file record's number at +20, logical lengths at +26 and +36, its first
    data and resource extents at +74 and +86; an extents key's fork type,
    file number and first allocation block after its length byte)."""
    mdb = 1024
    blk, first = struct.unpack('>I', img[mdb + 20:mdb + 24])[0], struct.unpack('>H', img[mdb + 28:mdb + 30])[0] * 512

    def extents(ext: bytes) -> list:
        return [struct.unpack('>HH', ext[4 * k:4 * k + 4]) for k in range(3)]

    def runs(exts: list, length: int) -> list:
        out = []
        for start, count in exts:
            take = min(count * blk, length)
            if take:
                out.append((first + start * blk, take))
            length -= take
        return out if length == 0 else []

    def leaves(exts: list, size: int):
        tree = b''.join(img[o:o + n] for o, n in runs(exts, size))
        for node in range(len(tree) // 512):
            nd = tree[node * 512:node * 512 + 512]
            if nd[8] != 0xFF:
                continue
            for r in range(struct.unpack('>H', nd[10:12])[0]):
                off = struct.unpack('>H', nd[510 - 2 * r:512 - 2 * r])[0]
                rec = off + 1 + nd[off]
                yield nd, off, rec + (rec & 1)

    for nd, off, rec in leaves(extents(img[mdb + 150:mdb + 162]), struct.unpack('>I', img[mdb + 146:mdb + 150])[0]):
        if nd[rec] == 2 and nd[off + 7:off + 7 + nd[off + 6]] == name.encode('mac_roman'):
            number = nd[rec + 20:rec + 24]
            length = struct.unpack('>I', nd[rec + (36 if rsrc else 26):][:4])[0]
            exts = extents(nd[rec + (86 if rsrc else 74):][:12])
            more = sorted((struct.unpack('>H', x[off + 6:off + 8])[0], x[r:r + 12])
                          for x, off, r in leaves(extents(img[mdb + 134:mdb + 146]), struct.unpack('>I', img[mdb + 130:mdb + 134])[0])
                          if x[off + 1] == (0xFF if rsrc else 0) and x[off + 2:off + 6] == number)
            for _, ext in more:
                exts += extents(ext)
            return runs(exts, length)
    return []


def hfs_add_file(path: Path, name: str, finfo: bytes, rsrc: bytes, data=b'', dates=(0, 0), subfolder='') -> int:
    """Adds a file to the System Folder of an HFS image (the folder the
    master directory block blesses, drFndrInfo at +92; with `subfolder`,
    the folder of that name in it: a directory record, type 1, its ID at
    +6 of its body), as the Finder would
    copy it: its forks in free allocation blocks (the volume bit map at
    drVBMSt, +14), a file record in the catalog's leaves (Inside Macintosh:
    Files, "Data Organization on Volumes": B*-tree nodes of 512 bytes with
    their descriptor (fLink, bLink, type, height, record count), records
    from offset 14 and their offsets from the node's end; the header node's
    header record (depth +14, root +16, leaf records +20, first and last
    leaf +24 and +28, node count +36, free nodes +40) and its map record of
    one bit per node from +248; catalog keys of key length, reserved byte,
    parent ID and name, padded to even length in leaves and to 38 bytes in
    index records, which point at their node with a long word; the 102-byte
    file record of hfs_fork), a full node split in two with its right
    half's first key added to the parent; the folder's valence (+4 of its
    directory record), the volume's next catalog node ID (+30), free blocks
    (+34) and file count (+84) counted. Returns the new file's ID."""
    img = bytearray(path.read_bytes())
    mdb = 1024
    u16 = lambda o: struct.unpack('>H', img[o:o + 2])[0]
    u32 = lambda o: struct.unpack('>I', img[o:o + 4])[0]
    blk, first, nmab = u32(mdb + 20), u16(mdb + 28) * 512, u16(mdb + 18)
    folder = u32(mdb + 92)
    cat_ext = [struct.unpack('>HH', img[mdb + 150 + 4 * k:mdb + 154 + 4 * k]) for k in range(3)]
    cat_runs = [(first + s * blk, c * blk) for s, c in cat_ext if c]
    if sum(n for _, n in cat_runs) != u32(mdb + 146):
        raise AssertionError(f'{path.name}: the catalog has overflow extents')

    def node_at(n: int) -> int:
        o = n * 512
        for start, length in cat_runs:
            if o < length:
                return start + o
            o -= length
        raise AssertionError(f'{path.name}: catalog node {n} outside the catalog')

    def node(n: int) -> bytes:
        return bytes(img[node_at(n):node_at(n) + 512])

    def offsets(nd: bytes) -> list:
        cnt = struct.unpack('>H', nd[10:12])[0]
        return [struct.unpack('>H', nd[510 - 2 * r:512 - 2 * r])[0] for r in range(cnt + 1)]

    def records(nd: bytes) -> list:
        offs = offsets(nd)
        return [nd[offs[r]:offs[r + 1]] for r in range(len(offs) - 1)]

    def fits(recs: list) -> bool:
        return 14 + sum(len(r) for r in recs) + 2 * (len(recs) + 1) <= 512

    def write_node(n: int, head: bytes, recs: list):
        body, offs = bytearray(head[:14]), []
        body[10:12] = struct.pack('>H', len(recs))
        for r in recs:
            offs.append(len(body))
            body += r
        offs.append(len(body))
        body += bytes(512 - len(body) - 2 * len(offs))
        for o in reversed(offs):
            body += struct.pack('>H', o)
        img[node_at(n):node_at(n) + 512] = body

    def sort_key(rec: bytes) -> tuple:
        return struct.unpack('>I', rec[2:6])[0], rec[7:7 + rec[6]].decode('mac_roman').upper()

    def index_record(rec: bytes, child: int) -> bytes:
        return (bytes([37]) + rec[1:7 + rec[6]]).ljust(38, b'\0') + struct.pack('>I', child)

    hdr = node(0)
    depth, root, leaf_recs, first_leaf, last_leaf = struct.unpack('>HIIII', hdr[14:32])
    nodes, free = struct.unpack('>II', hdr[36:44])
    mp = bytearray(hdr[248:248 + 256])
    used = lambda n: mp[n >> 3] & (0x80 >> (n & 7))

    def new_node() -> int:
        nonlocal free
        for n in range(min(nodes, len(mp) * 8)):
            if not used(n):
                mp[n >> 3] |= 0x80 >> (n & 7)
                free -= 1
                return n
        raise AssertionError(f'{path.name}: the catalog is full')

    def parent_of(child: int) -> int:
        for n in range(1, min(nodes, len(mp) * 8)):
            nd = node(n) if used(n) else b''
            if nd and nd[8] == 0 and any(struct.unpack('>I', r[-4:])[0] == child for r in records(nd)):
                return n
        return -1

    def insert(n: int, rec: bytes, after_child=None):
        """rec into node n in key order (an index record after the one
        pointing at after_child); a full node splits."""
        nonlocal root, depth, last_leaf
        nd = node(n)
        recs = records(nd)
        if after_child is None:
            at = next((i for i, r in enumerate(recs) if sort_key(r) > sort_key(rec)), len(recs))
        else:
            at = next(i for i, r in enumerate(recs) if struct.unpack('>I', r[-4:])[0] == after_child) + 1
        recs.insert(at, rec)
        if fits(recs):
            write_node(n, nd, recs)
            return
        cut = len(recs) // 2
        right = new_node()
        flink = struct.unpack('>I', nd[0:4])[0]
        write_node(n, struct.pack('>I', right) + nd[4:14], recs[:cut])
        write_node(right, struct.pack('>II', flink, n) + nd[8:14], recs[cut:])
        if flink:
            f = node(flink)
            write_node(flink, f[0:4] + struct.pack('>I', right) + f[8:14], records(f))
        elif nd[8] == 0xFF:
            last_leaf = right
        up = parent_of(n)
        if up < 0:
            top = new_node()
            write_node(top, struct.pack('>IIBBH', 0, 0, 0, nd[9] + 1, 0) + bytes(2),
                       [index_record(recs[0], n), index_record(recs[cut], right)])
            root, depth = top, depth + 1
        else:
            insert(up, index_record(recs[cut], right), n)

    if subfolder:
        n, sb, found = first_leaf, subfolder.encode('mac_roman'), 0
        while n and not found:
            nd = node(n)
            for r in records(nd):
                body = 1 + r[0] + ((1 + r[0]) & 1)
                if r[body] == 1 and struct.unpack('>I', r[2:6])[0] == folder and r[7:7 + r[6]] == sb:
                    found = struct.unpack('>I', r[body + 6:body + 10])[0]
            n = struct.unpack('>I', nd[0:4])[0]
        if not found:
            raise AssertionError(f'{path.name}: no folder {subfolder} in the System Folder')
        folder = found

    # The new file's place: the leaf holding the last key below its own.
    nb = name.encode('mac_roman')
    key = bytes([6 + len(nb), 0]) + struct.pack('>I', folder) + bytes([len(nb)]) + nb
    key += bytes(len(key) & 1)
    n, target = first_leaf, first_leaf
    while n:
        nd = node(n)
        for r in records(nd):
            if sort_key(r) == sort_key(key):
                raise AssertionError(f'{path.name}: {name} is already there')
            if sort_key(r) < sort_key(key):
                target = n
        n = struct.unpack('>I', nd[0:4])[0]

    # Its forks in free blocks.
    vbm = u16(mdb + 14) * 512

    def allocate(nbytes: int) -> tuple:
        count = (nbytes + blk - 1) // blk
        run = 0
        for b in range(nmab if count else 0):
            run = 0 if img[vbm + (b >> 3)] & (0x80 >> (b & 7)) else run + 1
            if run == count:
                start = b - count + 1
                for k in range(start, b + 1):
                    img[vbm + (k >> 3)] |= 0x80 >> (k & 7)
                return start, count
        if count:
            raise AssertionError(f'{path.name}: no room for {name}')
        return 0, 0

    forks = []
    for fork in (data, rsrc):
        start, count = allocate(len(fork))
        img[first + start * blk:first + (start + count) * blk] = fork.ljust(count * blk, b'\0')
        forks.append((start, count))
    (dstart, dcount), (rstart, rcount) = forks
    cnid = u32(mdb + 30)
    rec = bytearray(102)
    rec[0] = 2
    rec[4:20] = finfo[:16]
    rec[20:24] = struct.pack('>I', cnid)
    rec[26:34] = struct.pack('>II', len(data), dcount * blk)
    rec[36:44] = struct.pack('>II', len(rsrc), rcount * blk)
    rec[44:52] = struct.pack('>II', dates[0], dates[1])
    rec[56:72] = finfo[16:32]
    rec[74:78] = struct.pack('>HH', dstart, dcount)
    rec[86:90] = struct.pack('>HH', rstart, rcount)
    insert(target, key + bytes(rec))

    # The counts: the header node, the folder's valence, the volume's.
    hdr = bytearray(node(0))
    hdr[14:32] = struct.pack('>HIIII', depth, root, leaf_recs + 1, first_leaf, last_leaf)
    hdr[40:44] = struct.pack('>I', free)
    hdr[248:248 + 256] = mp
    img[node_at(0):node_at(0) + 512] = hdr
    n = first_leaf
    while n:
        nd = node(n)
        for r, start in zip(records(nd), offsets(nd)):
            body = 1 + r[0] + ((1 + r[0]) & 1)
            if r[body] == 1 and struct.unpack('>I', r[body + 6:body + 10])[0] == folder:
                v = node_at(n) + start + body + 4
                img[v:v + 2] = struct.pack('>H', u16(v) + 1)
        n = struct.unpack('>I', nd[0:4])[0]
    img[mdb + 30:mdb + 34] = struct.pack('>I', cnid + 1)
    img[mdb + 34:mdb + 36] = struct.pack('>H', u16(mdb + 34) - dcount - rcount)
    img[mdb + 84:mdb + 88] = struct.pack('>I', u32(mdb + 84) + 1)
    if folder == 2:
        img[mdb + 12:mdb + 14] = struct.pack('>H', u16(mdb + 12) + 1)
    path.write_bytes(bytes(img))
    return cnid


def net_server():
    """A server on the host's loopback interface that reads a line and
    answers one: (port, what it got, the thread, the socket)."""
    import socket
    import threading
    srv = socket.socket()
    srv.bind(('127.0.0.1', 0))
    srv.listen(1)
    srv.settimeout(600)
    port = srv.getsockname()[1]
    got = {}

    def serve():
        try:
            c, _ = srv.accept()
            c.settimeout(60)
            data = b''
            while not data.endswith(b'\n'):
                b = c.recv(100)
                if not b:
                    break
                data += b
            got['line'] = data
            c.sendall(b'hello from the host\r\n')
            c.close()
        except OSError as e:
            got['error'] = str(e)

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    return port, got, t, srv


def resource_fork(resources: list) -> bytes:
    """A resource fork of (type, ID, data) (Inside Macintosh: More Macintosh
    Toolbox, "Resource File Format": the header's data and map offsets and
    lengths, each resource's data after its length, the map's copy of the
    header, its type list offset at +24 and name list offset at +26, the
    type list's count - 1, entries of type, count - 1 and reference list
    offset, references of ID, name offset (-1: none), attributes and data
    offset, a handle's long)."""
    data, refs = b'', {}
    for rtype, rid, body in resources:
        refs.setdefault(rtype, []).append((rid, len(data)))
        data += struct.pack('>I', len(body)) + body
    types = list(refs)
    tl = struct.pack('>H', len(types) - 1)
    reflist, at = b'', 2 + 8 * len(types)
    for t in types:
        tl += t + struct.pack('>HH', len(refs[t]) - 1, at + len(reflist))
        for rid, off in refs[t]:
            reflist += struct.pack('>hhII', rid, -1, off & 0xFFFFFF, 0)
    rmap = bytes(16) + bytes(8) + struct.pack('>HH', 28, 28 + len(tl) + len(reflist)) + tl + reflist
    head = struct.pack('>IIII', 256, 256 + len(data), len(data), len(rmap))
    return head + bytes(240) + data + head + rmap[16:]


def pbx_release_serial_port(path: Path) -> None:
    """Reproduce the Mac OS 8.0 AppleTalk panel's Modem/Printer Port and
    Inactive settings on the test disk. The control panel rewrites this
    resource fork without its old 'vers' resource. The stable bytes of
    'atpf' below reproduce that setting without copying guest pointers."""
    img = path.read_bytes()
    fork, = hfs_fork(img, 'AppleTalk Preferences')
    start, length = fork
    changes = {b'ulvl': b'\0\x02', b'port': b'\x12Modem/Printer Port', b'ptfm': b'\0\x03'}
    types = [(b'neta', -16400), (b'ulvl', 1), (b'ccfg', 1), (b'cnam', 128), (b'atpf', 128),
             (b'port', 128), (b'ptfm', 128), (b'pwrd', 128), (b'lcks', 128), (b'cvrs', 128),
             (b'prot', 128), (b'wpos', 1)]
    resources = []
    for kind, rid in types:
        data = resource_data(img, start, kind, rid)
        if not data:
            raise AssertionError(f'{path.name}: AppleTalk Preferences lacks {kind!r} {rid}')
        at, size = data
        body = changes.get(kind, img[at:at + size])
        if kind == b'atpf':
            if size != 274:
                raise AssertionError(f'{path.name}: unexpected AppleTalk Preferences format')
            body = body[:5] + b'\x03' + body[6:7] + b'\x0a' + body[8:92] + b'\0\x01\0\0' + body[96:]
        resources.append((kind, rid, body))
    raw = resource_fork(resources)
    if len(raw) > length:
        raise AssertionError(f'{path.name}: rewritten AppleTalk preferences do not fit')
    with path.open('r+b') as f:
        f.seek(start)
        f.write(raw)


def startup_application(path: Path, name: str, code: bytes, subfolder='Startup Items', replace_existing=False) -> int:
    """A 68k application of one code segment put in the System Folder's
    Startup Items, which the Finder opens once it is up (Inside Macintosh
    II, "The Segment Loader": 'CODE' 0 holds the sizes above and below A5,
    the jump table's length and offset (32) and its entries (the routine's
    offset, move.w #1,-(sp), _LoadSeg); 'CODE' 1 the first entry's offset
    and the entry count, then the code; 'SIZE' -1 the flags and the
    preferred and minimum sizes). The code runs from its first byte."""
    if replace_existing:
        # A warm restart retains the disk's OS updates as well as NVRAM.
        # Replace only our own startup probe, so the reader cannot rewrite
        # the setting whose persistence it is meant to check.
        img = bytearray(path.read_bytes())
        runs = hfs_fork(img, name)
        fork = bytearray(b''.join(img[o:o + n] for o, n in runs))
        location = resource_data(fork, 0, b'CODE', 1) if fork else None
        if not location or len(code) + 4 > location[1]:
            raise AssertionError(f'{name}: no existing code segment with enough room')
        offset, length = location
        segment = struct.pack('>HH', 0, 1) + code
        fork[offset:offset + length] = segment + bytes(length - len(segment))
        cursor = 0
        for offset, length in runs:
            img[offset:offset + length] = fork[cursor:cursor + length]
            cursor += length
        path.write_bytes(img)
        return 0
    code0 = struct.pack('>IIII', 40, 256, 8, 32) + struct.pack('>HHHH', 0, 0x3F3C, 1, 0xA9F0)
    code1 = struct.pack('>HH', 0, 1) + code
    size = struct.pack('>HII', 0x0080, 512 * 1024, 512 * 1024)
    rsrc = resource_fork([(b'CODE', 0, code0), (b'CODE', 1, code1), (b'SIZE', -1, size)])
    finfo = b'APPLMCnp' + bytes(24)
    return hfs_add_file(path, name, finfo, rsrc, subfolder=subfolder)


def net_results(name: str, res: bytes, got: dict, out: str, patched: int) -> str:
    """The network probe's results (net_probe.s) and the server's side."""
    if len(res) < 128:
        raise AssertionError(f'{name}: the network probe did not publish a complete result record\n{out[-1200:]}')
    words = [struct.unpack('>i', res[k:k + 4])[0] for k in range(0, 64, 4)]
    stats = re.search(r'network: (\d+) DHCP offers, (\d+) DHCP acks, (\d+) BOOTP replies.*?(\d+) TCP connections', out)
    lease = f'DHCP {stats.group(2)} ack(s), BOOTP {stats.group(3)}' if stats else 'no counters'
    problems = []
    if res[:4] != b'NETP':
        problems.append('no results')
    names = ['_Open .IPP', 'ipctlGetAddr', 'TCPCreate', 'TCPActiveOpen', 'TCPSend', 'TCPRcv', 'TCPClose', 'TCPRelease']
    for label, k in zip(names, [1, 3, 6, 8, 10, 11, 13, 14]):
        if words[k]:
            problems.append(f'{label}: error {words[k]}')
    if words[4] != 0x0A00020F or (words[5] & 0xFFFFFFFF) != 0xFFFFFF00:
        problems.append(f'address {words[4] & 0xFFFFFFFF:08X} mask {words[5] & 0xFFFFFFFF:08X}, expected 10.0.2.15/24')
    received = bytes(res[64:64 + max(0, min(words[12], 64))])
    if received != b'hello from the host\r\n':
        problems.append(f'received {received!r}')
    if got.get('line') != b'hello from the Macintosh\r\n':
        problems.append(f'the server read {got!r}')
    if problems:
        raise AssertionError(f'{name}: ' + '; '.join(problems) + f' ({lease}; prefs patched: {patched})\n{out[-1500:]}')
    return f'{name}: 10.0.2.15/24 ({lease}), a TCP connection to the host both ways'


def net_probe_app(path: Path, port: int) -> int:
    """The network probe as a startup application (startup_application) for
    a PowerPC cell: the server's address and port in its longs at 4 and 8
    (as --live-arg gives them); it ends in its `bra.s *', its results in
    RAM, found by mc_ppc_pdm_boot --find."""
    code = bytearray(NET_PROBE.read_bytes())
    code[4:12] = struct.pack('>II', 0x0A000202, port)
    return startup_application(path, 'Livadix Net Probe', bytes(code))


def net_probe_found(out: str) -> bytes:
    """The probe's results from mc_ppc_pdm_boot --find 4E455450 200: the
    copy of "NETP" that is not the immediate of its own move.l #,(a5)
    ($2ABC)."""
    for l in out.splitlines():
        m = re.match(r'found [0-9A-F]+ after ([0-9A-F]{4}):((?: [0-9A-F]{2})+)', l)
        if m and m.group(1) != '2ABC':
            return bytes(int(b, 16) for b in m.group(2).split())
    return b''


def check_declrom():
    p = run([sys.executable, ROOT / 'tests' / 'declrom_check.py', ROOT / 'src' / 'hw' / 'guest' / 'bin' / 'livadixcard.bin',
             '--modes', ROOT / 'docs' / 'NOTES.md'], check=False)
    if p.returncode != 0:
        raise AssertionError(p.stdout + p.stderr)
    lines = p.stdout.strip().splitlines()
    print(indented('\n'.join(lines[:2] + [f'{len(lines) - 3} video sResources'] + lines[-1:])))


def check_pmu_diff():
    p = run([BUILD / ('mc_test_pmu' + EXE), '--diff', ROMS], check=False, timeout=300)
    if p.returncode == 77:
        raise Skip('no PMU firmware in roms/')
    if p.returncode != 0:
        raise AssertionError(p.stdout + p.stderr)
    print(indented(p.stdout.strip()))


def check_guest():
    p = run([sys.executable, ROOT / 'tools' / 'guest_build.py', 'check'], check=False, timeout=300)
    if p.returncode == 77:
        raise Skip(p.stdout.strip())
    if p.returncode != 0:
        raise AssertionError(p.stdout + p.stderr)
    print(indented(p.stdout))


# --- the 68k accelerator's speed ---------------------------------


# --- the 68k accelerator against Apple's emulator: random programs ---


# --- the 68k accelerator's shadow check as a gate ------------------


CHECKS = [('version', check_version), ('root', check_root), ('rom-names', check_rom_names),
          ('roms', check_roms), ('config', check_config), ('config-window', check_config_window), ('screen', check_screen),
          ('looks', check_looks), ('cpu-suite', check_cpu_suite),
          ('fpu-suite', check_fpu_suite), ('rom-trace', check_rom_trace), ('boot', check_boot), ('os-boot', check_os_boot), ('chime', check_chime),
          ('cpu-speed', check_cpu_speed), ('jit-diff', check_jit_diff), ('guest', check_guest), ('declrom', check_declrom), ('pmu-diff', check_pmu_diff),
          ('ppc-jit-diff', check_ppc_jit_diff), ('ppc-jit-rom', check_ppc_jit_rom), ('ppc-grackle', check_ppc_grackle), ('ppc-finder', check_ppc_finder)]


def snapshot():
    files = {}
    for d in (ROMS, DATA):
        for p in d.rglob('*') if d.is_dir() else []:
            if p.is_file():
                st = p.stat()
                files[p] = (st.st_size, st.st_mtime_ns)
    return files


# --- tiers, units and the process pool ---------------------------------------------------

# The quick tier (the default): what everyday work and commits run. The unit
# tests are in ctest (tools/build.ps1), which verify does not repeat; the rest is
# sampled: a few CPU-suite groups and translator programs, one boot per
# machine family (the cells marked `quick` in tests/boot_matrix), the chimes
# of the sound sources. `--full` runs everything.
QUICK_CHECKS = ['version', 'root', 'rom-names', 'roms', 'config', 'config-window', 'screen', 'looks',
                'cpu-suite', 'boot', 'os-boot', 'chime', 'cpu-speed', 'jit-diff', 'guest', 'declrom', 'ppc-jit-diff', 'ppc-jit-rom', 'ppc-grackle', 'ppc-finder', 'pippin-app']
# Timing-sensitive checks (host speed against a limit, real-time pacing): run
# alone, after the rest, so that load from other checks cannot fail them.
EXCLUSIVE_CHECKS = {'cpu-speed', 'boot'}   # boot: its last run is paced by the window loop
EXCLUSIVE_UNITS = {('chime', 'quadra-700-paced')}
# A check that needs the files of another, in one process: looks reads the
# screen check's screenshot.
FOLLOWS = {'looks': 'screen'}
# Guessed cost (seconds) of a unit without a timing from an earlier run, to start the long ones first.
COST_GUESS = {'os-boot': 40, 'jit-diff': 40, 'cpu-suite': 30, 'chime': 2}


def check_units(name: str) -> list:
    """The units a check splits into (independent runs of one process each); [''] when it does not."""
    if name == 'os-boot':
        return os_boot_units()
    if name == 'chime':
        return chime_units()
    if name == 'jit-diff':
        return jit_diff_units()
    if name == 'ppc-jit-diff':
        return ppc_jit_diff_units()
    if name == 'cpu-suite':
        return cpu_suite_units()
    return ['']


def physical_cores() -> int:
    import subprocess as sp
    if os.name != 'nt':
        return max(1, (os.cpu_count() or 2) // 2)
    try:
        out = sp.run(['powershell', '-NoProfile', '-Command',
                      '(Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum'],
                     capture_output=True, text=True, timeout=30).stdout.strip()
        return max(1, int(out))
    except Exception:
        return max(1, (os.cpu_count() or 2) // 2)


def run_worker(checks: list, unit: str, result: Path) -> int:
    """A worker process: runs the checks (one unit of them) and writes their results."""
    import contextlib
    import io
    import json
    import time
    global UNIT, OS_DIR
    UNIT = unit if unit != '' else None
    key = slug(f'{checks[0]}-{unit}') if unit else checks[0]
    OS_DIR = WORK / 'os' / key
    table = dict(CHECKS)
    out = []
    for name in checks:
        buf = io.StringIO()
        t0 = time.perf_counter()
        status, message = 'PASS', ''
        try:
            with contextlib.redirect_stdout(buf):
                table[name]()
        except Skip as s:
            status, message = 'SKIP', str(s)
        except AssertionError as e:
            status, message = 'FAIL', str(e)
        except subprocess.TimeoutExpired as e:
            status, message = 'FAIL', f'timed out: {e}'
        out.append({'check': name, 'status': status, 'message': message, 'text': buf.getvalue(),
                    'seconds': round(time.perf_counter() - t0, 2)})
    result.write_text(json.dumps({'results': out, 'timings': TIMINGS}), encoding='utf-8')
    return 0


def main(argv) -> int:
    import argparse
    import concurrent.futures
    import json
    import time
    global QUICK, UNIT
    ap = argparse.ArgumentParser(prog='verify.py', description="Livadix's gates: the quick tier by default, --full for everything")
    ap.add_argument('names', nargs='*', help='run only these checks (all of each)')
    ap.add_argument('--list', action='store_true', help='list the checks')
    ap.add_argument('--full', action='store_true', help='every check in full, as before a release')
    ap.add_argument('--jobs', type=int, default=0, help='processes in parallel (default: physical cores - 1)')
    ap.add_argument('--family', help='boot-matrix files to run (comma separated: quadra800,iici); with no check named: only os-boot')
    ap.add_argument('--cell', help='boot-matrix cells to run (comma separated names or parts of names)')
    ap.add_argument('--timings', help='write per-check, per-unit and per-cell timings to this JSON file')
    ap.add_argument('--worker', nargs=3, metavar=('CHECKS', 'UNIT', 'RESULT'), help=argparse.SUPPRESS)
    ap.add_argument('--quick', action='store_true', help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    names_all = [n for n, _ in CHECKS]
    FILTER['family'] = set(args.family.split(',')) if args.family else None
    FILTER['cell'] = [c for c in args.cell.split(',')] if args.cell else None
    if args.worker:
        QUICK = args.quick
        return run_worker(args.worker[0].split(','), args.worker[1] if args.worker[1] != '-' else '', Path(args.worker[2]))
    if args.list:
        for n in names_all:
            print(n + ('' if n in QUICK_CHECKS else '   (full tier only)'))
        return 0
    unknown = [a for a in args.names if a not in names_all]
    if unknown:
        print('unknown check: ' + ', '.join(unknown), file=sys.stderr)
        return 2
    if not LIVADIX.exists():
        print('build first: ' + ('pwsh -File tools/build.ps1' if os.name == 'nt' else 'tools/build_linux.sh (LIVADIX_BUILD names its build folder)'), file=sys.stderr)
        return 2
    filtered = bool(args.family or args.cell)
    if args.names:
        selected, QUICK = [n for n in names_all if n in args.names], False
    elif filtered:
        selected, QUICK = ['os-boot'], False
    elif args.full:
        selected, QUICK = names_all, False
    else:
        selected, QUICK = [n for n in names_all if n in QUICK_CHECKS], True
    jobs = args.jobs or max(1, physical_cores() - 1)
    started = time.perf_counter()
    before = snapshot()
    if 'chime' in selected:
        chime_prepare()
    # The tasks: (checks, unit) in the order of the checks; a follower runs in its leader's process.
    tasks = []
    for name in selected:
        if name in FOLLOWS and FOLLOWS[name] in selected:
            continue
        group = [name] + [f for f, lead in FOLLOWS.items() if lead == name and f in selected]
        for unit in check_units(name):
            tasks.append((tuple(group), unit))
    if 'os-boot' in selected and (FILTER['family'] or FILTER['cell']) and not any(group[0] == 'os-boot' for group, unit in tasks):
        ap.error('no boot-matrix cells match the requested family/cell filters')
    previous = {}
    try:
        previous = json.loads((ROOT / 'work' / 'timings.json').read_text()).get('units', {})
    except Exception:
        pass
    work = ROOT / 'work' / 'verify' / 'tasks'
    work.mkdir(parents=True, exist_ok=True)
    for old in work.glob('*.json'):
        old.unlink()
    results = {}

    def execute(index: int) -> None:
        group, unit = tasks[index]
        result = work / f'{index:04d}.json'
        cmd = [sys.executable, str(Path(__file__).resolve()), '--worker', ','.join(group), unit or '-', str(result)]
        if QUICK:
            cmd.append('--quick')
        if args.family:
            cmd += ['--family', args.family]
        if args.cell:
            cmd += ['--cell', args.cell]
        t0 = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, text=True)
        wall = time.perf_counter() - t0
        try:
            data = json.loads(result.read_text(encoding='utf-8'))
        except Exception:
            data = {'results': [{'check': c, 'status': 'FAIL', 'text': '', 'seconds': round(wall, 2),
                                 'message': f'worker failed ({p.returncode}):\n{p.stdout[-1500:]}{p.stderr[-2500:]}'} for c in group],
                    'timings': {}}
        results[index] = (data, wall)
        bad = [r for r in data['results'] if r['status'] == 'FAIL']
        print(f'  {"FAIL" if bad else "done"} {",".join(group)}{(" " + unit) if unit else ""} ({wall:.1f} s)',
              file=sys.stderr, flush=True)

    def cost(i):
        group, unit = tasks[i]
        return previous.get(f'{group[0]}:{unit}', COST_GUESS.get(group[0], 10))
    parallel = [i for i, (g, u) in enumerate(tasks) if g[0] not in EXCLUSIVE_CHECKS and (g[0], u) not in EXCLUSIVE_UNITS]
    alone = [i for i in range(len(tasks)) if i not in parallel]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(execute, sorted(parallel, key=lambda i: -cost(i))))
    for i in alone:
        execute(i)
    # The report, in the order of the checks and their units.
    timings = {'jobs': jobs, 'tier': 'quick' if QUICK else 'full', 'checks': {}, 'units': {}, 'cells': {}, 'chime': {}}
    failed = 0
    for name in selected:
        mine = [(i, r) for i, (group, unit) in enumerate(tasks) if name in group
                for r in results[i][0]['results'] if r['check'] == name]
        text, fails, skips, passes = [], [], [], 0
        for i, r in mine:
            timings['units'][f'{name}:{tasks[i][1]}'] = r['seconds']
            if r['status'] == 'FAIL':
                fails.append(((tasks[i][1] + ': ') if tasks[i][1] else '') + r['message'])
            elif r['status'] == 'SKIP':
                skips.append(r['message'])
            else:
                passes += 1
            text += [l for l in r['text'].splitlines()]
        for i in {i for i, _ in mine}:
            for key in ('cells', 'chime'):
                timings[key].update(results[i][0]['timings'].get(key, {}))
        timings['checks'][name] = round(sum(r['seconds'] for _, r in mine), 2)
        # skip lines of a family repeated by each of its units once
        seen, lines = set(), []
        for l in text:
            if l.strip().startswith('skipped ') and l in seen:
                continue
            seen.add(l)
            lines.append(l)
        if lines:
            print('\n'.join(lines))
        if fails:
            failed += 1
            print(f'FAIL {name}\n    ' + '\n'.join(fails).replace('\n', '\n    '))
        elif not passes and skips:
            print(f'SKIP {name}: {skips[0]}')
        else:
            print(f'PASS {name}')
    if snapshot() != before:
        failed += 1
        print('FAIL roms/ or data/ changed during the checks')
    timings['wall'] = round(time.perf_counter() - started, 1)
    out = Path(args.timings) if args.timings else ROOT / 'work' / 'timings.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    if not (filtered or args.names) or args.timings:
        out.write_text(json.dumps(timings, indent=1), encoding='utf-8')
    print(f'verify ({timings["tier"]} tier, {jobs} jobs, {timings["wall"]:.0f} s): ' + ('OK' if not failed else f'{failed} failure(s)'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
