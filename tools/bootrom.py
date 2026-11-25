"""Builds Livadix's built-in boot ROM.

    python tools/bootrom.py --bin FILE       optional standalone ROM image
    python tools/bootrom.py --header FILE    C array embedded in livadix.exe

The ROM is the code in src/hw/guest/bin/bootrom.bin (assembled from
src/hw/guest/drivers/bootrom.s by tools/guest_build.py) followed by its
data: Livadix's two bitmap fonts (src/app/ui_font.c, Livadix's own design), the
document icon (Livadix's own) and the classic arrow cursor, the version
(src/VERSION) and the documents, README.md and docs/CHANGELOG.md, turned
from Markdown into lines wrapped to the window's width. The table at offset
$10 gets the data's addresses (the ROM sits at $40 0000); the image is 128
KiB, and its checksum (the sum of its words after the first four bytes,
kept in those bytes) is made 'LVDX' ($4C564458) by the padding, so Livadix
can also identify an exported image under any name (romdb.c). CMake embeds
the generated image, so the Built-in machine needs no external ROM file.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / 'src' / 'hw' / 'guest' / 'bin' / 'bootrom.bin'
FONTS = ROOT / 'src' / 'app' / 'ui_font.c'
DOCS = [('README', ROOT / 'README.md'), ('Changelog', ROOT / 'docs' / 'CHANGELOG.md')]
VERSION = ROOT / 'src' / 'VERSION'

BASE = 0x400000
CHECKSUM = 0x4C564458          # 'LVDX'
SIZE = 0x20000                 # romdb.c: 128 KiB
WRAP = 390                     # the text's width in the window (bootrom.s: bytes 2-50)
FIRST = 32
EXTRA = [0x2013, 0x2022, 0x2026, 0x2018, 0x2019, 0x201C, 0x201D]   # codes 127-133
SUBSTITUTES = {'—': '–', ' ': ' ', '×': 'x', '→': '->', '←': '<-', '≤': '<=',
               '≥': '>=', '†': '+', '·': '•', '−': '-'}

ICON = """
................................
....##################..........
....#................##.........
....#................#.#........
....#................#..#.......
....#................#...#......
....#................#....#.....
....#................######.....
....#.....................#.....
....#..###########........#.....
....#.....................#.....
....#..##############.....#.....
....#.....................#.....
....#..############.......#.....
....#.....................#.....
....#..################...#.....
....#.....................#.....
....#..##########.........#.....
....#.....................#.....
....#..##############.....#.....
....#.....................#.....
....#..#############......#.....
....#.....................#.....
....#..########...........#.....
....#.....................#.....
....#.....................#.....
....#.....................#.....
....#######################.....
................................
................................
................................
................................
"""

# Classic Macintosh arrow, redrawn to match the requested Finder 2.0 cursor.
# # black, o white, . transparent; hotspot (1,1).
CURSOR = """
oo..............
o#o.............
o##o............
o###o...........
o####o..........
o#####o.........
o######o........
o#######o.......
o########o......
o#####ooooo.....
o##o##o.........
o#o.o##o........
oo..o##o........
o....o##o.......
.....o##o.......
......ooo.......
"""


def rows(art: str) -> list[str]:
    return [r for r in art.strip().splitlines()]


def bits(row: str) -> int:
    v = 0
    for c in row:
        v = v << 1 | (c == '#')
    return v


def icon() -> bytes:
    art = rows(ICON)
    data = [bits(r) for r in art]
    # The mask: each row filled between its first and last set pixel.
    mask = []
    for r in art:
        first, last = r.find('#'), r.rfind('#')
        mask.append(bits(''.join('#' if first >= 0 and first <= i <= last else '.' for i in range(len(r)))))
    return b''.join(struct.pack('>I', v) for v in data + mask)


def cursor() -> bytes:
    art = rows(CURSOR)
    data = [bits(r) for r in art]
    mask = [bits(r.replace('o', '#')) for r in art]
    return b''.join(struct.pack('>H', v) for v in data + mask) + struct.pack('>HH', 1, 1)


# --- the fonts ------------------------------------------------------------------------

def parse_fonts() -> dict[str, dict]:
    text = FONTS.read_text(encoding='utf-8')
    fonts = {}
    for name in ('system12', 'small9'):
        body = text[text.index(f'glyph_src {name}[]'):]
        body = body[:body.index('};')]
        glyphs = {}
        for m in re.finditer(r"\{\s*('(?:\\.|[^'])'|0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*\"([^\"]*)\"\s*\}", body):
            code = m.group(1)
            if code.startswith('0x'):
                c = int(code, 16)
            else:
                ch = code[1:-1]
                c = ord(ch[1]) if ch.startswith('\\') else ord(ch)
            glyphs[c] = (int(m.group(2)), m.group(3).split('/'))
        fonts[name] = glyphs
    return fonts


def font_image(glyphs: dict, height: int, ascent: int) -> tuple[bytes, list[int]]:
    """The ROM font (bootrom.s, draw_char) and each code's width."""
    codes = list(range(FIRST, 127)) + EXTRA
    widths, data = [], b''
    for c in codes:
        top, rs = glyphs.get(c, glyphs[ord('?')])
        widths.append(len(rs[0]))
        pix = [0] * height
        for i, r in enumerate(rs):
            if top + i < height:
                pix[top + i] = bits(r.ljust(16, '.')[:16])
        data += b''.join(struct.pack('>H', v) for v in pix)
    head = struct.pack('>HHHH', height, ascent, FIRST, len(codes)) + bytes(widths)
    if len(head) % 2:
        head += b'\0'
    return head + data, widths


# --- the documents --------------------------------------------------------------------

def rom_text(s: str) -> bytes:
    out = bytearray()
    for ch in s:
        ch = SUBSTITUTES.get(ch, ch)
        for c in ch:
            o = ord(c)
            if FIRST <= o < 127:
                out.append(o)
            elif o in EXTRA:
                out.append(127 + EXTRA.index(o))
            else:
                plain = unicodedata.normalize('NFKD', c).encode('ascii', 'ignore')
                out += plain if plain else b'?'
    return bytes(out)


def inline(s: str) -> str:
    s = re.sub(r'!\[[^\]]*\]\([^)]*\)', '', s)                 # images
    s = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', s)             # links: their text
    s = s.replace('**', '').replace('`', '')
    return s


class Wrapper:
    def __init__(self, widths: list[int]):
        self.widths = widths

    def width(self, b: bytes) -> int:
        w = sum(self.widths[c - FIRST] + 1 if FIRST <= c < FIRST + len(self.widths) else 0 for c in b)
        return max(w - 1, 0)

    def wrap(self, text: bytes, first: bytes = b'', indent: int = 0) -> list[bytes]:
        """Words of text wrapped to WRAP pixels, after `first` on the first
        line and `indent` spaces on the others."""
        lines, line = [], first
        pad = b' ' * indent
        for word in text.split():
            trial = line + (b' ' if line.strip() and not line.endswith(b' ') else b'') + word
            if self.width(trial) <= WRAP or not line.strip():
                line = trial
                while self.width(line) > WRAP:          # a word longer than a line
                    cut = len(line)
                    while cut > 1 and self.width(line[:cut]) > WRAP:
                        cut -= 1
                    lines.append(line[:cut])
                    line = pad + line[cut:]
            else:
                lines.append(line)
                line = pad + word
        if line.strip():
            lines.append(line)
        return lines


def document(path: Path, wrapper: Wrapper) -> list[tuple[int, bytes]]:
    """Markdown as lines: (1 for a heading, the text)."""
    out: list[tuple[int, bytes]] = []
    blank = lambda: out and out[-1][1] and out.append((0, b''))
    para: list[str] = []
    item = None                                    # (marker, indent, words)
    fence = False

    def flush():
        nonlocal para, item
        if para:
            for line in wrapper.wrap(rom_text(inline(' '.join(para)))):
                out.append((0, line))
            para = []
        if item:
            marker, depth, words = item
            first = b'  ' * depth + marker + b' '
            for line in wrapper.wrap(rom_text(inline(' '.join(words))), first, round((wrapper.width(first) + 1) / 4)):
                out.append((0, line))
            item = None

    for raw in path.read_text(encoding='utf-8').splitlines():
        line = raw.rstrip()
        if line.startswith('```'):
            flush()
            fence = not fence
            continue
        if fence:
            for piece in wrapper.wrap(rom_text(line), b'  ', 2) or [b'']:
                out.append((0, b'  ' + piece.lstrip() if not piece.startswith(b'  ') else piece))
            continue
        if not line.strip():
            flush()
            blank()
            continue
        m = re.match(r'^(#{1,6})\s+(.*)$', line)
        if m:
            flush()
            blank()
            out.append((1, rom_text(inline(m.group(2)))))
            out.append((0, b''))
            continue
        m = re.match(r'^(\s*)([-*]|\d+\.)\s+(.*)$', line)
        if m:
            flush()
            marker = b'\x80' if m.group(2) in '-*' else m.group(2).encode()
            item = (marker, len(m.group(1)) // 2, [m.group(3)])
            continue
        if item and raw.startswith(' '):
            item[2].append(line.strip())
            continue
        if item:
            flush()
        para.append(line.strip())
    flush()
    while out and not out[-1][1]:
        out.pop()
    while out and not out[0][1]:
        out.pop(0)
    return out


# --- the image ------------------------------------------------------------------------

def build() -> bytes:
    code = CODE.read_bytes()
    fonts = parse_fonts()
    body, body_widths = font_image(fonts['small9'], 9, 7)
    ui, _ = font_image(fonts['system12'], 12, 10)
    wrapper = Wrapper(body_widths)
    image = bytearray(code)

    def put(data: bytes) -> int:
        while len(image) % 4:
            image.append(0)
        at = len(image)
        image.extend(data)
        return BASE + at

    table = {}
    table[0x10] = put(body)
    table[0x14] = put(ui)
    titles, docs = [], []
    for title, path in DOCS:
        lines = document(path, wrapper)
        strings = [put(bytes([flag]) + text + b'\0') for flag, text in lines]
        line_table = put(b''.join(struct.pack('>I', a) for a in strings))
        docs.append((put(rom_text(title) + b'\0'), line_table, len(lines)))
    entries = struct.pack('>HH', len(docs), 0) + b''.join(struct.pack('>IIHH', t, l, n, 0) for t, l, n in docs)
    table[0x18] = put(entries)
    table[0x1C] = put(icon())
    table[0x20] = put(rom_text('Version ' + VERSION.read_text(encoding='utf-8').strip()) + b'\0')
    table[0x24] = put(cursor())
    for offset, address in table.items():
        image[offset:offset + 4] = struct.pack('>I', address)
    while len(image) % 2:
        image.append(0)
    # The size and the padding that makes the checksum 'LVDX'.
    used = len(image)
    content = sum(struct.unpack(f'>{(used - 4) // 2}H', image[4:used]))
    size = SIZE
    need = CHECKSUM - content
    if used > size or not 0 <= need <= (size - used) // 2 * 0xFFFF:
        raise SystemExit(f'bootrom.py: the documents do not fit the {SIZE // 1024} KiB ROM')
    image.extend(bytes(size - used))
    at = used
    while need:
        word = min(need, 0xFFFF)
        image[at:at + 2] = struct.pack('>H', word)
        need -= word
        at += 2
    image[0:4] = struct.pack('>I', CHECKSUM)
    assert sum(struct.unpack(f'>{(size - 4) // 2}H', image[4:])) & 0xFFFFFFFF == CHECKSUM
    return bytes(image)


def write_changed(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def main(argv) -> int:
    ap = argparse.ArgumentParser(description="Builds Livadix's built-in boot ROM")
    ap.add_argument('--bin', type=Path, help='write the ROM image (unchanged when it is the same)')
    ap.add_argument('--header', type=Path, help='write the embedded C image (unchanged when it is the same)')
    args = ap.parse_args(argv)
    rom = build()
    if args.bin:
        write_changed(args.bin, rom)
    if args.header:
        data = '\n'.join('    ' + ','.join(f'0x{b:02x}' for b in rom[i:i + 16]) + ','
                         for i in range(0, len(rom), 16))
        header = '/* Generated by tools/bootrom.py; do not edit. */\nstatic const unsigned char mc_builtin_rom_image[]={\n' + data + '\n};\n'
        write_changed(args.header, header.encode('ascii'))
    if not args.bin and not args.header:
        print(f'{len(rom)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
