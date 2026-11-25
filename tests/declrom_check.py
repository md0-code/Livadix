"""Walks a NuBus declaration ROM image and checks it (tests/verify.py, check
declrom).

    python tests/declrom_check.py ROM.bin [--modes NOTES.md]

Written on its own from Designing Cards and Drivers for the Macintosh
Family, 3rd ed., chapter 8 and chapter 9 (it shares no code with the
generator, tools/declrom.py). From the top of the image: the format block
(ByteLanes from Table 8-2 with its complement nibble, the reserved byte,
TestPattern, Format, RevisionLevel 1-9, Length, the CRC recomputed as the
Slot Manager does, DirectoryOffset); the sResource directory (ascending IDs,
the end-of-list entry, one board sResource below 128); the board sResource
(sRsrcType CatBoard, sRsrcName, BoardId, PRAMInitData, PrimaryInit as an
SExecBlock, VendorInfo); each video sResource (sRsrcType CatDisplay /
TypVideo / DrSWApple, its name, the sDriver directory and the driver's
header and name, sRsrcFlags, MinorBaseOS / MinorLength, the gamma directory
and its tables, the depth modes from 128 in order with their parameter
lists and video parameter records). With --modes, every resolution of the
table "Resolutions" of docs/NOTES.md must be a video sResource
offering each depth up to the table's deepest.

Prints what it found; exits 1 on the first problem.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

LANE_VALUES = {0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F}


class Bad(Exception):
    pass


class Rom:
    def __init__(self, data: bytes):
        self.data = data
        self.top = len(data)        # one past the ByteLanes byte

    def u8(self, at):
        if not 0 <= at < self.top:
            raise Bad(f'read outside the ROM at {at:#x}')
        return self.data[at]

    def u16(self, at):
        return self.u8(at) << 8 | self.u8(at + 1)

    def u32(self, at):
        return self.u16(at) << 16 | self.u16(at + 2)

    def s16(self, at):
        v = self.u16(at)
        return v - 0x10000 if v & 0x8000 else v

    def bytes(self, at, n):
        return bytes(self.u8(at + i) for i in range(n))

    def cstr(self, at, limit=255):
        out = bytearray()
        while (c := self.u8(at + len(out))) != 0:
            out.append(c)
            if len(out) > limit:
                raise Bad(f'cString at {at:#x} longer than {limit}')
        return out.decode('mac_roman')

    def entries(self, at, what):
        """The (id, entry address, 24-bit field) of an sResource-style list."""
        out, last = [], -1
        for n in range(256):
            v = self.u32(at + 4 * n)
            spid, field = v >> 24, v & 0xFFFFFF
            if spid == 0xFF:
                if field:
                    raise Bad(f'{what}: end-of-list entry {v:#010x}')
                return out
            if spid <= last:
                raise Bad(f'{what}: IDs not ascending ({last} then {spid})')
            last = spid
            out.append((spid, at + 4 * n, field))
        raise Bad(f'{what}: no end of list')

    def target(self, entry_at, field, what):
        off = field - (1 << 24) if field & 0x800000 else field
        t = entry_at + off
        if not 0 <= t < self.top:
            raise Bad(f'{what}: offset {off} leaves the ROM')
        return t

    def sblock(self, at, what):
        n = self.u32(at)
        if n < 4 or at + n > self.top:
            raise Bad(f'{what}: sBlock size {n}')
        return self.bytes(at + 4, n - 4)


def need(cond, message):
    if not cond:
        raise Bad(message)


def check(data: bytes, modes_doc: Path | None) -> list:
    rom = Rom(data)
    report = []
    top = len(data)
    lanes = rom.u8(top - 1)
    need(lanes in LANE_VALUES and (lanes >> 4) == (~lanes & 0xF), f'ByteLanes {lanes:#04x}')
    need(lanes == 0x0F, f'ByteLanes {lanes:#04x}: this checker reads all-lane images only')
    need(rom.u8(top - 2) == 0, 'reserved byte')
    need(rom.u32(top - 6) == 0x5A932BC7, f'TestPattern {rom.u32(top - 6):#010x}')
    need(rom.u8(top - 7) == 1, f'Format {rom.u8(top - 7)}')
    revision = rom.u8(top - 8)
    need(1 <= revision <= 9, f'RevisionLevel {revision}')
    crc = rom.u32(top - 12)
    length = rom.u32(top - 16)
    need(20 <= length <= top, f'Length {length}')
    dir_field = rom.u32(top - 20)
    need(dir_field >> 24 == 0, 'DirectoryOffset high byte')
    start = top - length
    sum_ = 0
    for a in range(start, top):
        sum_ = ((sum_ << 1) | (sum_ >> 31)) & 0xFFFFFFFF
        if not top - 12 <= a < top - 8:
            sum_ = (sum_ + data[a]) & 0xFFFFFFFF
    need(sum_ == crc, f'CRC {crc:#010x}, computed {sum_:#010x}')
    directory = rom.target(top - 20, dir_field, 'DirectoryOffset')
    need(directory >= start, 'the directory lies below Length')
    report.append(f'format block: ByteLanes $0F, revision {revision}, {length} bytes, CRC {crc:08X} valid')
    dir_entries = rom.entries(directory, 'sResource directory')
    boards = [e for e in dir_entries if e[0] < 128]
    need(len(boards) == 1, f'{len(boards)} board sResources')
    found = {}
    for spid, at, field in dir_entries:
        rsrc = rom.target(at, field, f'sResource {spid}')
        items = {i: (a, f) for i, a, f in rom.entries(rsrc, f'sResource {spid}')}
        need(1 in items and 2 in items, f'sResource {spid}: no sRsrcType or sRsrcName')
        tp = rom.target(*items[1], f'sResource {spid} type')
        category, ctype, drsw, drhw = rom.u16(tp), rom.u16(tp + 2), rom.u16(tp + 4), rom.u16(tp + 6)
        name = rom.cstr(rom.target(*items[2], f'sResource {spid} name'))
        if spid < 128:
            need((category, ctype, drsw, drhw) == (1, 0, 0, 0), f'board sRsrcType {category},{ctype},{drsw},{drhw}')
            need(32 in items, 'board: no BoardId')
            board_id = items[32][1] & 0xFFFF
            need(items[32][1] >> 16 == 0, 'BoardId: not word data')
            if 33 in items:
                pram = rom.sblock(rom.target(*items[33], 'PRAMInitData'), 'PRAMInitData')
                need(len(pram) == 8 and pram[0:2] == b'\0\0', 'PRAMInitData: not 0, 0 and six bytes')
            if 34 in items:
                at34 = rom.target(*items[34], 'PrimaryInit')
                size = rom.u32(at34)
                rev, cpu, reserved = rom.u8(at34 + 4), rom.u8(at34 + 5), rom.u16(at34 + 6)
                need(rev == 2 and 1 <= cpu <= 4 and reserved == 0, f'PrimaryInit: revision {rev}, CPU {cpu}')
                code = at34 + 8 + rom.u32(at34 + 8)
                need(at34 + 12 <= code < at34 + size, 'PrimaryInit: code offset')
                code_bytes = rom.bytes(code, at34 + size - code)
                need(b'\x4e\x75' in code_bytes[::1], 'PrimaryInit: no RTS')
                need(size - (code - at34) < 16384, 'PrimaryInit: 16 KB or more')
            if 36 in items:
                vendor = {i: rom.cstr(rom.target(a, f, f'VendorInfo {i}'))
                          for i, a, f in rom.entries(rom.target(*items[36], 'VendorInfo'), 'VendorInfo')}
            report.append(f'board sResource {spid}: "{name}", BoardId ${board_id:04X}'
                          + (f', vendor {vendor.get(1, "?")} {vendor.get(3, "")}' if 36 in items else ''))
            continue
        need((category, ctype, drsw) == (3, 1, 1), f'sResource {spid}: not CatDisplay/TypVideo/DrSWApple')
        need(4 in items, f'sResource {spid}: no sRsrcDrvrDir')
        drivers = rom.entries(rom.target(*items[4], 'sRsrcDrvrDir'), 'sDriver directory')
        need(any(d[0] in (2, 3, 4) for d in drivers), f'sResource {spid}: no driver for the 68020 or later')
        for d_id, a, f in drivers:
            drv = rom.sblock(rom.target(a, f, 'sDriver'), 'sDriver')
            need(len(drv) > 18, 'driver: no header')
            dname = drv[19:19 + drv[18]].decode('mac_roman')
            need(dname == '.' + name, f'driver name "{dname}" for sRsrcName "{name}"')
            # drvrOpen, drvrPrime, drvrCtl, drvrStatus, drvrClose (Prime may be 0).
            for at_, routine in ((8, 'Open'), (10, 'Prime'), (12, 'Control'), (14, 'Status'), (16, 'Close')):
                off = drv[at_] << 8 | drv[at_ + 1]
                need(off < len(drv) and (off or routine == 'Prime'), f'driver {routine} offset {off}')
        flags = items[7][1] & 0xFFFF if 7 in items else 2
        need(flags & 2, f'sResource {spid}: fOpenAtStart clear')
        base = rom.u32(rom.target(*items[10], 'MinorBaseOS')) if 10 in items else 0
        length_vram = rom.u32(rom.target(*items[11], 'MinorLength')) if 11 in items else 0
        need(length_vram > 0, f'sResource {spid}: no MinorLength')
        gammas = []
        if 64 in items:
            for g_id, a, f in rom.entries(rom.target(*items[64], 'sGammaDir'), 'gamma directory'):
                block = rom.sblock(rom.target(a, f, 'gamma table'), 'gamma table')
                gname_end = block.index(b'\0', 2)
                gname = block[2:gname_end].decode('mac_roman')
                need(len(gname) <= 35, f'gamma name "{gname}" longer than 35')
                t = gname_end + 1 + ((gname_end + 1) & 1)
                version, gtype, fsize, chans, count, width = (int.from_bytes(block[t + 2 * k:t + 2 * k + 2], 'big') for k in range(6))
                need(version == 0 and chans in (1, 3) and count == 256 and width == 8, f'gamma table "{gname}"')
                need(len(block) >= t + 12 + fsize + chans * count, f'gamma table "{gname}": short')
                gammas.append(gname)
        depths = []
        mode_ids = sorted(i for i in items if i >= 128)
        need(mode_ids and mode_ids == list(range(128, 128 + len(mode_ids))), f'sResource {spid}: depth modes {mode_ids}')
        size = None
        for m in mode_ids:
            params = {i: (a, f) for i, a, f in rom.entries(rom.target(*items[m], f'mode {m}'), f'mode {m}')}
            need(1 in params and 4 in params, f'sResource {spid} mode {m}: no mVidParams or mDevType')
            vp = rom.sblock(rom.target(*params[1], 'mVidParams'), 'mVidParams')
            need(len(vp) == 42, f'sResource {spid} mode {m}: video parameters of {len(vp)} bytes')
            offset, rowbytes = int.from_bytes(vp[0:4], 'big'), int.from_bytes(vp[4:6], 'big')
            top_, left, bottom, right = (int.from_bytes(vp[6 + 2 * k:8 + 2 * k], 'big') for k in range(4))
            ptype, psize, ccount, csize = (int.from_bytes(vp[30 + 2 * k:32 + 2 * k], 'big') for k in range(4))
            w, h = right - left, bottom - top_
            need(size in (None, (w, h)), f'sResource {spid}: sizes differ between modes')
            size = (w, h)
            need(psize == 1 << (m - 128), f'sResource {spid} mode {m}: pixel size {psize}')
            dev = params[4][1] & 0xFFFF
            if psize >= 16:
                need((ptype, ccount, csize, dev) == (16, 3, 5 if psize == 16 else 8, 2), f'mode {m}: not a direct mode')
            else:
                need((ptype, ccount, csize) == (0, 1, psize) and dev in (0, 1), f'mode {m}: not an indexed mode')
            need(rowbytes % 2 == 0 and rowbytes < 0x4000 and rowbytes * 8 >= w * psize, f'mode {m}: row bytes {rowbytes}')
            need(base + offset + rowbytes * h <= length_vram, f'sResource {spid} mode {m}: {w}x{h}x{psize} beyond MinorLength')
            pages = params[3][1] & 0xFFFF if 3 in params else 1
            need(pages >= 1, f'mode {m}: {pages} pages')
            depths.append(psize)
        found[size] = depths
        report.append(f'video sResource {spid}: {size[0]}x{size[1]}, depths {depths}, base ${base:X}, '
                      f'{length_vram // 1024} KiB, flags {flags}, gamma {gammas}')
    if modes_doc:
        text = modes_doc.read_text(encoding='utf-8')
        table = re.findall(r'^\| `(\d+)x(\d+)` \| [^|]+\| (\d+) \|$', text, re.M)
        need(table, f'{modes_doc}: no resolution table')
        for w, h, deepest in table:
            size = (int(w), int(h))
            need(size in found, f'{w}x{h} of {modes_doc.name}: no video sResource')
            want = [d for d in (1, 2, 4, 8, 16, 32) if d <= int(deepest)]
            need(found[size] == want, f'{w}x{h}: depths {found[size]}, {modes_doc.name} says {want}')
        report.append(f'every resolution and depth of {modes_doc.name} ({len(table)} resolutions) is offered')
    return report


def main(argv) -> int:
    if not argv or len(argv) not in (1, 3) or (len(argv) == 3 and argv[1] != '--modes'):
        print(__doc__)
        return 2
    try:
        for line in check(Path(argv[0]).read_bytes(), Path(argv[2]) if len(argv) == 3 else None):
            print(line)
    except Bad as e:
        print(f'{argv[0]}: {e}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
