"""Builds a NuBus declaration ROM from its description.

    python tools/declrom.py SOURCE.yaml OUT.bin [--bin FOLDER]

SOURCE.yaml describes the card (src/hw/guest/declrom/livadixcard.yaml); the
code it names (the PrimaryInit record, the driver) is read from FOLDER
(default src/hw/guest/bin), where tools/guest_build.py assembles it. The
image is the declaration ROM as the card's top bytes of slot space hold
it, the format block last: Designing Cards and Drivers for the Macintosh
Family, 3rd ed., chapter 8 ("The format block": ByteLanes, TestPattern
$5A932BC7, Format 1, RevisionLevel, the CRC's rotate-left-and-add over
Length bytes with its own field as zero, the self-relative DirectoryOffset;
"The sResource directory"; "sResource structure": entries of an ID byte and
a 24-bit self-relative offset, word or byte data, ending in $FF000000;
sRsrcType, sRsrcName, sRsrcDrvrDir with sMacOS68020, sRsrcFlags,
sRsrcHWDevId, MinorBaseOS, MinorLength; the board sResource's BoardId,
PRAMInitData, PrimaryInit, VendorInfo; sBlock and SExecBlock; sGammaDir
and sRsrcVidNames) and chapter 9 (the depth mode lists mVidParams, mPageCnt,
mDevType and the video parameter record). Only images using all four byte
lanes are built.

tools/guest_build.py runs this after assembling; the independent parser in
tests/declrom_check.py checks the result.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
TEST_PATTERN = 0x5A932BC7

# Apple-defined IDs (Designing Cards, Tables 8-3, 8-4, 8-6, 8-7, 8-9, 9-1).
sRsrcType, sRsrcName, sRsrcDrvrDir, sRsrcFlags, sRsrcHWDevId = 1, 2, 4, 7, 8
MinorBaseOS, MinorLength = 10, 11
BoardId, PRAMInitData, PrimaryInit, VendorInfo, SecondaryInit = 32, 33, 34, 36, 38
sGammaDir, sRsrcVidNames = 64, 65
VendorId, RevLevel, PartNum = 1, 3, 4
sMacOS68020 = 2
mVidParams, mPageCnt, mDevType = 1, 3, 4
END = 0xFF


class Item:
    """A block of the image: bytes with 4-byte offset entries to fill in."""

    def __init__(self, label: str, parts: list):
        self.label = label
        self.parts = parts          # bytes, or ('entry', id, target label)
        self.address = 0

    def size(self) -> int:
        n = sum(4 if isinstance(p, tuple) else len(p) for p in self.parts)
        return n + (n & 1)          # every block starts on an even offset


def entry_offset(spid: int, target: str) -> tuple:
    return ('entry', spid, target)


def entry_data(spid: int, value: int) -> bytes:
    return struct.pack('>I', spid << 24 | (value & 0xFFFFFF))


def end_of_list() -> bytes:
    return struct.pack('>I', END << 24)


def cstring(text: str) -> bytes:
    return text.encode('mac_roman') + b'\0'


def sblock(data: bytes) -> bytes:
    return struct.pack('>I', 4 + len(data)) + data


def sexec(code: bytes, cpu: int = 2) -> bytes:
    # Revision 2, the CPU, a reserved word, the code's offset from the offset field.
    return struct.pack('>IBBHI', 12 + len(code), 2, cpu, 0, 4) + code


def gamma_table(exponent: float, gtype: int) -> bytes:
    data = bytes(round(255 * (i / 255) ** exponent) for i in range(256))
    return struct.pack('>6H', 0, gtype, 0, 1, 256, 8) + data


def vp_block(width: int, height: int, depth: int, row_bytes: int, dpi: int) -> bytes:
    direct = depth >= 16
    return struct.pack('>IH4HHHIIIHHHHI', 0, row_bytes, 0, 0, height, width, 0, 0, 0, dpi << 16, dpi << 16,
                       16 if direct else 0, depth, 3 if direct else 1, (5 if depth == 16 else 8) if direct else depth, 0)


def row_bytes(width: int, depth: int) -> int:
    """Whole longwords, as the driver and the card compute them."""
    return (width * depth + 31) // 32 * 4


def build(desc: dict, bins: Path) -> bytes:
    if desc.get('byte_lanes', 0x0F) != 0x0F:
        raise SystemExit('only all four byte lanes ($0F) are built')
    board, video = desc['board'], desc['video']
    items: list[Item] = []

    def add(label, parts):
        items.append(Item(label, parts))

    first = video['first_id']
    resolutions = [tuple(int(v) for v in r.split('x')) for r in video['resolutions']]
    # The sResource directory.
    add('directory', [entry_offset(board['id'], 'board')] +
        [entry_offset(first + n, f'video{n}') for n in range(len(resolutions))] + [end_of_list()])
    # The board sResource.
    add('board', [entry_offset(sRsrcType, 'board_type'), entry_offset(sRsrcName, 'board_name'),
                  entry_data(BoardId, board['board_id']), entry_offset(PRAMInitData, 'board_pram'),
                  entry_offset(PrimaryInit, 'board_init'), entry_offset(VendorInfo, 'board_vendor')] +
        ([entry_offset(SecondaryInit, 'board_init2')] if board.get('secondary_init') else []) +
        [entry_offset(sRsrcVidNames, 'vid_names'), end_of_list()])
    add('board_type', [struct.pack('>4H', 1, 0, 0, 0)])          # CatBoard, TypBoard, DrSWBoard, DrHWBoard
    add('board_name', [cstring(board['name'])])
    pram = bytes(board['pram'])
    if len(pram) != 6:
        raise SystemExit('board.pram: six bytes')
    add('board_pram', [sblock(b'\0\0' + pram)])
    add('board_init', [sexec((bins / f"{board['primary_init']}.bin").read_bytes())])
    if board.get('secondary_init'):
        add('board_init2', [sexec((bins / f"{board['secondary_init']}.bin").read_bytes())])
    vendor = board['vendor']
    add('board_vendor', [entry_offset(VendorId, 'vendor_id'), entry_offset(RevLevel, 'vendor_rev'),
                         entry_offset(PartNum, 'vendor_part'), end_of_list()])
    add('vendor_id', [cstring(vendor['id'])])
    add('vendor_rev', [cstring(str(vendor['revision']))])
    add('vendor_part', [cstring(vendor['part'])])
    add('vid_names', [entry_offset(first + n, f'vid_name{n}') for n in range(len(resolutions))] + [end_of_list()])
    for n, (w, h) in enumerate(resolutions):
        add(f'vid_name{n}', [sblock(b'\0\0' + cstring(f'{w} x {h}'))])
    # What every video sResource shares.
    add('video_type', [struct.pack('>4H', video['category'], video['ctype'], video['drsw'], video['drhw'])])
    add('video_name', [cstring(video['name'])])
    add('driver_dir', [entry_offset(sMacOS68020, 'driver'), end_of_list()])
    add('driver', [sblock((bins / f"{video['driver']}.bin").read_bytes())])
    add('minor_base', [struct.pack('>I', video['minor_base'])])
    add('minor_length', [struct.pack('>I', video['vram'])])
    gammas = video['gamma']
    add('gamma_dir', [entry_offset(128 + g, f'gamma{g}') for g in range(len(gammas))] + [end_of_list()])
    for g, table in enumerate(gammas):
        name = cstring(table['name'])
        if len(name) & 1:
            name += b'\0'
        add(f'gamma{g}', [sblock(b'\0\0' + name + gamma_table(float(table['exponent']), video['drhw']))])
    # One video sResource per resolution, a depth mode per depth that fits.
    depths = video['depths']
    base = video['minor_base']
    for n, (w, h) in enumerate(resolutions):
        modes = [d for d in depths if base + row_bytes(w, d) * h <= video['vram']]
        add(f'video{n}', [entry_offset(sRsrcType, 'video_type'), entry_offset(sRsrcName, 'video_name'),
                          entry_offset(sRsrcDrvrDir, 'driver_dir'), entry_data(sRsrcFlags, video['flags']),
                          entry_data(sRsrcHWDevId, video['hw_dev_id']), entry_offset(MinorBaseOS, 'minor_base'),
                          entry_offset(MinorLength, 'minor_length'), entry_offset(sGammaDir, 'gamma_dir')] +
            [entry_offset(128 + depths.index(d), f'mode{n}_{d}') for d in modes] + [end_of_list()])
        for d in modes:
            add(f'mode{n}_{d}', [entry_offset(mVidParams, f'vp{n}_{d}'), entry_data(mPageCnt, 1),
                                 entry_data(mDevType, 2 if d >= 16 else 0), end_of_list()])
            add(f'vp{n}_{d}', [sblock(vp_block(w, h, d, row_bytes(w, d), video['dpi']))])
    # Layout, then the offsets.
    address = 0
    labels = {}
    for item in items:
        item.address = address
        labels[item.label] = address
        address += item.size()
    body = bytearray()
    for item in items:
        at = item.address
        for part in item.parts:
            if isinstance(part, tuple):
                _, spid, target = part
                offset = labels[target] - at
                if not -(1 << 23) <= offset < (1 << 23):
                    raise SystemExit(f'{item.label}: offset to {target} out of range')
                part = struct.pack('>I', spid << 24 | (offset & 0xFFFFFF))
            body += part
            at += len(part)
        if len(body) & 1:
            body += b'\0'
    # The format block: DirectoryOffset, Length, CRC, RevisionLevel, Format,
    # TestPattern, Reserved, ByteLanes.
    length = len(body) + 20
    directory = labels['directory'] - len(body)
    image = body + struct.pack('>IIIBBIBB', directory & 0xFFFFFF, length, 0, desc.get('revision', 1), 1,
                               TEST_PATTERN, 0, desc.get('byte_lanes', 0x0F))
    crc_at = len(body) + 8
    crc = 0
    for i, b in enumerate(image):
        crc = ((crc << 1) | (crc >> 31)) & 0xFFFFFFFF
        if not crc_at <= i < crc_at + 4:
            crc = (crc + b) & 0xFFFFFFFF
    image[crc_at:crc_at + 4] = struct.pack('>I', crc)
    return bytes(image)


def main(argv) -> int:
    if len(argv) not in (2, 4) or (len(argv) == 4 and argv[2] != '--bin'):
        print(__doc__)
        return 2
    bins = Path(argv[3]) if len(argv) == 4 else ROOT / 'src' / 'hw' / 'guest' / 'bin'
    with open(argv[0], encoding='utf-8') as f:
        desc = yaml.safe_load(f)
    Path(argv[1]).write_bytes(build(desc, bins))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
