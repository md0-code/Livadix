"""Rename the Macintosh ROMs of a folder to their standard names.

    python tools/rom_names.py FOLDER            show what would be renamed (dry run)
    python tools/rom_names.py FOLDER --apply    rename

Livadix itself finds ROMs by their content whatever their names; this tool only
tidies a folder. A Macintosh ROM is recognized by its header checksum and
size, controller firmware, declaration ROMs and New World "Mac OS ROM"
files by size and CRC32, as Livadix does (src/core/romdb.c, which holds each
image's standard name: a ROM's is its primary model's key + .rom). A
second file with the same image is reported and left alone, never
overwritten; unknown files are listed.
"""
from __future__ import annotations

import argparse
import re
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROMDB = ROOT / 'src/core/romdb.c'


def database() -> dict[tuple, tuple[str, str]]:
    """('rom', checksum, size) or ('firmware', crc32, size) -> (standard file
    name, models), from romdb.c."""
    text = ROMDB.read_text(encoding='utf-8')
    known = {}
    for m in re.finditer(r'ROM\(0x([0-9a-fA-F]{8}),\s*(\d+),\s*"[^"]*",\s*"([^"]*)",\s*"[^"]*",\s*"([^"]+)"\)', text):
        known[('rom', int(m[1], 16), int(m[2]) * 1024)] = (m[4] + '.rom', m[3])
    for m in re.finditer(r'\{"([^"]+\.bin)",\s*"([^"]*)",\s*(\d+),\s*0x([0-9a-fA-F]{8}),\s*"([^"]*)"\}', text):
        known[('firmware', int(m[4], 16), int(m[3]))] = (m[1], f'{m[2]}; {m[5]}')
    # The New World "Mac OS ROM" files: NW(version, bytes, crc, machines).
    for m in re.finditer(r'NW\("([^"]+)",\s*(\d+),\s*0x([0-9a-fA-F]{8}),\s*"([^"]*)"\)', text):
        known[('firmware', int(m[3], 16), int(m[2]))] = (f'mac-os-rom-{m[1]}.rom', f'Mac OS ROM file {m[1]} (New World); {m[4]}')
    return known


def identify(path: Path, known) -> tuple[str, str] | None:
    size = path.stat().st_size
    with path.open('rb') as f:
        head = f.read(4)
    rom = known.get(('rom', int.from_bytes(head, 'big'), size)) if len(head) == 4 else None
    if rom or not any(k[0] == 'firmware' and k[2] == size for k in known):
        return rom
    return known.get(('firmware', zlib.crc32(path.read_bytes()), size))


def plan(folder: Path, known):
    """(source, target or None, note) for every file of the folder."""
    files = sorted(p for p in folder.iterdir() if p.is_file())
    taken = {p.name.lower(): p for p in files}
    claimed: dict[str, Path] = {}
    steps = []
    # Files already under their standard name keep it.
    found = {p: identify(p, known) for p in files}
    for p, rom in found.items():
        if rom and p.name.lower() == rom[0].lower():
            claimed[rom[0].lower()] = p
    for p, rom in found.items():
        if not rom:
            steps.append((p, None, 'unknown'))
        elif claimed.get(rom[0].lower()) == p:
            steps.append((p, None, f'standard ({rom[1]})'))
        elif rom[0].lower() in claimed or (rom[0].lower() in taken and taken[rom[0].lower()] != p):
            other = claimed.get(rom[0].lower()) or taken[rom[0].lower()]
            steps.append((p, None, f'duplicate of {other.name}, skipped'))
        else:
            claimed[rom[0].lower()] = p
            steps.append((p, folder / rom[0], rom[1]))
    return steps


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('folder', type=Path)
    parser.add_argument('--apply', action='store_true', help='rename (default: dry run)')
    args = parser.parse_args()
    if not args.folder.is_dir():
        print(f'{args.folder}: not a folder', file=sys.stderr)
        return 2
    known = database()
    renamed = 0
    for source, target, note in plan(args.folder, known):
        if target is None:
            print(f'{source.name}: {note}')
            continue
        if args.apply:
            if target.exists():  # appeared meanwhile: never overwrite
                print(f'{source.name}: {target.name} exists, skipped')
                continue
            source.rename(target)
            renamed += 1
        print(f'{source.name} -> {target.name} ({note}){"" if args.apply else " [dry run]"}')
    if args.apply:
        print(f'{renamed} file(s) renamed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
