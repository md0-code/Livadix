"""Builds the PCI Livadix Display Card's Open Firmware FCode expansion ROM and
PowerPC native video driver (PEF container, 'ndrv' / 'vido').

Called by tools/guest_build.py from src/hw/guest/declrom/livadixcard_pci.yaml
and the assembled src/hw/guest/pci/livadixvideo_pci.bin.
"""
from __future__ import annotations

import struct
from pathlib import Path


def pef_hash(name: bytes) -> int:
    """The Code Fragment Manager's export hash word (Mac OS Runtime
    Architectures, chapter 8, "PEF Structure", "Hashing Functions"): a
    signed 32-bit value pseudo-rotated left and XORed with each unsigned
    character, folded to 16 bits under the name's length."""
    h = 0
    for b in name:
        h = ((h << 1) - (h >> 16)) ^ b      # >> on a signed value: arithmetic
        h &= 0xFFFFFFFF
        if h & 0x80000000:
            h -= 1 << 32
    return (len(name) << 16) | ((h ^ (h >> 16)) & 0xFFFF)


def build_pef(code: bytes, data: bytes, libraries: list[tuple[str, list[str]]],
              exports: list[tuple[str, int, int, int]],
              data_relocs: list[tuple[str, int]], main: int | None = None) -> bytes:
    """Packs `code` (section 0), `data` (section 1) and a loader section
    (section 2) into a PowerPC PEF container ('Joy!peffpwpc'); `main`, the
    offset in the data of the main symbol's transition vector (the loader
    header's mainSection and mainOffset), or none."""
    # Loader string table: library names, imported symbol names, exported symbol names.
    strings = bytearray()
    str_off: dict[str, int] = {}

    def add_str(s: str) -> int:
        if s not in str_off:
            str_off[s] = len(strings)
            strings.extend(s.encode('ascii') + b'\x00')
        return str_off[s]

    lib_entries = bytearray()
    sym_entries = bytearray()
    first_sym = 0
    for lib_name, syms in libraries:
        name_off = add_str(lib_name)
        lib_entries += struct.pack('>IIIIII', name_off, 0, 0, len(syms), first_sym, 0)
        for sym in syms:
            s_off = add_str(sym)
            # kPEFTVectorSymbol = 2 in the high byte of classAndName.
            sym_entries += struct.pack('>I', (2 << 24) | s_off)
        first_sym += len(syms)
    total_imports = first_sym

    # Relocation instructions for section 1 (data).
    # Each entry in `data_relocs` is (op, count):
    #   ('skip', words)   -> RelocBySectDWithSkip (000_0000_ssssssss_dddddd, skipWords=words, relocCount=0)
    #   ('import', count) -> RelocImportRun       (010_0101_rrrrrrrrr, runLength=count)
    #   ('code', count)   -> RelocBySectC         (010_0000_rrrrrrrrr, runLength=count)
    #   ('data', count)   -> RelocBySectD         (010_0001_rrrrrrrrr, runLength=count)
    relocs = bytearray()
    for op, n in data_relocs:
        if op == 'skip':
            relocs += struct.pack('>H', (n << 6))
        elif op == 'import':
            relocs += struct.pack('>H', 0x4A00 | (n - 1))
        elif op == 'code':
            relocs += struct.pack('>H', 0x4000 | (n - 1))
        elif op == 'data':
            relocs += struct.pack('>H', 0x4200 | (n - 1))
        else:
            raise ValueError(f'unknown relocation op {op}')
    if len(relocs) & 2:
        relocs += b'\x00\x00'

    # Export table (1 hash slot: exportHashTablePower = 0).
    export_key_table = bytearray()
    export_sym_table = bytearray()
    for name, sym_class, offset, section in exports:
        n_off = add_str(name)
        export_key_table += struct.pack('>I', pef_hash(name.encode('ascii')))
        export_sym_table += struct.pack('>IIh', (sym_class << 24) | n_off, offset, section)
    export_hash_slot = struct.pack('>I', (len(exports) << 18) | 0)

    while len(strings) & 3:
        strings.append(0)

    # PEFLoaderRelocationHeader: sectionIndex, reservedA, relocCount (16-bit
    # instructions), firstRelocOffset (from relocInstrOffset).
    reloc_hdr = struct.pack('>HHII', 1, 0, len(relocs) // 2, 0)
    reloc_instr_offset = 56 + len(lib_entries) + len(sym_entries) + len(reloc_hdr)
    loader_strings_offset = reloc_instr_offset + len(relocs)
    export_hash_offset = loader_strings_offset + len(strings)

    # PEFLoaderInfoHeader: main/init/term section (SInt32) + offset (UInt32) each,
    # then importedLibraryCount .. exportedSymbolCount (UInt32).
    loader_hdr = struct.pack(
        '>iIiIiIIIIIIIII',
        -1 if main is None else 1, main or 0, -1, 0, -1, 0,
        len(libraries), total_imports,
        1, reloc_instr_offset,
        loader_strings_offset,
        export_hash_offset, 0, len(exports),
    )
    loader = bytes(loader_hdr + lib_entries + sym_entries + reloc_hdr + relocs +
                   strings + export_hash_slot + export_key_table + export_sym_table)

    # Container header (40 bytes) + 3 section headers (3 x 28 = 84 bytes) = 124, padded to 128.
    code_aligned = code + b'\x00' * ((-len(code)) & 15)
    data_aligned = data + b'\x00' * ((-len(data)) & 15)
    code_off = 128
    data_off = code_off + len(code_aligned)
    loader_off = data_off + len(data_aligned)

    # PEFContainerHeader (Mac OS Runtime Architectures, chapter 8, Table 8-1):
    # tag1, tag2, architecture, formatVersion, dateTimeStamp, oldDefVersion,
    # oldImpVersion, currentVersion (UInt32), sectionCount, instSectionCount
    # (UInt16), reservedA (UInt32): 40 bytes.
    container_hdr = struct.pack(
        '>4s4s4sIIIIIHHI',
        b'Joy!', b'peff', b'pwpc', 1,
        0, 0, 0, 0,
        3, 2, 0,
    )
    assert len(container_hdr) == 40
    # Section headers (Table 8-3): nameOffset (-1), defaultAddress (0),
    # totalSize, unpackedSize, packedSize, containerOffset, sectionKind
    # (0 code, 1 unpacked data, 4 loader), shareKind (1 process share),
    # alignment (a power of 2), reserved. As in Apple's ROM display drivers
    # (ROM 9630C68B, file $26BD50): every section shared per process, the
    # loader section not instantiated (its total and unpacked sizes 0).
    sec0 = struct.pack('>iIIIIIBBBx', -1, 0, len(code), len(code), len(code), code_off, 0, 1, 2)
    sec1 = struct.pack('>iIIIIIBBBx', -1, 0, len(data), len(data), len(data), data_off, 1, 1, 3)
    sec2 = struct.pack('>iIIIIIBBBx', -1, 0, 0, 0, len(loader), loader_off, 4, 1, 0)
    hdr = container_hdr + sec0 + sec1 + sec2 + b'\x00' * 4
    return hdr + code_aligned + data_aligned + loader


# --- Open Firmware FCode assembler -------------------------------------------

def f_lit(v: int) -> bytes:
    """b(lit) (0x10) followed by a big-endian 32-bit integer."""
    return b'\x10' + struct.pack('>I', v & 0xFFFFFFFF)


def f_str(s: bytes | str) -> bytes:
    """b(") (0x12) followed by a length byte and up to 255 bytes."""
    if isinstance(s, str):
        s = s.encode('ascii')
    assert len(s) <= 255
    return bytes([0x12, len(s)]) + s


def f_tok(tok: int) -> bytes:
    """1- or 2-byte FCode token."""
    if tok < 0x100:
        return bytes([tok])
    return struct.pack('>H', tok)


def f_new_token(tok: int, bdef: int) -> bytes:
    """new-token (0xB5): anonymous token `tok` of definition type `bdef`."""
    return bytes([0xB5]) + struct.pack('>H', tok) + bytes([bdef])


def f_external_token(name: str, tok: int, bdef: int) -> bytes:
    """external-token (0xCA): named token `name` (`tok`) of type `bdef`."""
    nb = name.encode('ascii')
    return bytes([0xCA, len(nb)]) + nb + struct.pack('>H', tok) + bytes([bdef])


def f_branch_skip(op: int, skip: int) -> bytes:
    """start1 uses signed 16-bit offsets relative to the offset field."""
    return bytes([op]) + struct.pack('>h', 2 + skip)


def f_branch_back(op: int, before: int) -> bytes:
    """Branch to a token `before` bytes before this branch instruction."""
    return bytes([op]) + struct.pack('>h', -before - 1)


def f_if(body: bytes, else_body: bytes = b'') -> bytes:
    """b?branch / bbranch / b(>resolve) (0xB2) for if ... [else ...] then."""
    if else_body:
        br_else = f_branch_skip(0x13, len(else_body))
        body_with_br = body + br_else
        return f_branch_skip(0x14, len(body_with_br)) + body_with_br + else_body + b'\xB2'
    return f_branch_skip(0x14, len(body)) + body + b'\xB2'


def f_do(body: bytes) -> bytes:
    """?do (b?do = 0x18) ... loop (bloop = 0x15)."""
    br_back = f_branch_back(0x15, len(body))
    br_out = f_branch_skip(0x18, len(body) + len(br_back))
    for _ in range(3):
        br_back = f_branch_back(0x15, len(body))
        br_out = f_branch_skip(0x18, len(body) + len(br_back))
    return br_out + body + br_back


def f_begin_until(body: bytes) -> bytes:
    """b(<mark) (0xB1) ... b?branch (0x14) back."""
    return b'\xB1' + body + f_branch_back(0x14, 1 + len(body))


def f_begin_while_repeat(cond: bytes, body: bytes) -> bytes:
    """b(<mark) (0xB1) cond b?branch body bbranch b(>resolve) (0xB2)."""
    br_back = f_branch_back(0x13, 1 + len(cond) + 2 + len(body))
    br_fwd = f_branch_skip(0x14, len(body) + len(br_back))
    for _ in range(3):
        br_back = f_branch_back(0x13, 1 + len(cond) + len(br_fwd) + len(body))
        br_fwd = f_branch_skip(0x14, len(body) + len(br_back))
    return b'\xB1' + cond + br_fwd + body + br_back + b'\xB2'


def build_fcode(ndrv: bytes, spec: dict) -> bytes:
    """Builds the FCode program for the PCI Livadix Display Card:
      - device properties ("name", "device_type", "model", "reg",
        "iso6429-1983-colors", "driver,AAPL,MacOS,PowerPC")
      - Open Firmware display package methods (is-install, is-remove,
        color@, color!, set-colors, get-colors, draw-rectangle,
        fill-rectangle, read-rectangle) so Open Firmware can open the card as
        its screen and Mac OS can load its native video driver."""
    # Standard FCode tokens (IEEE 1275-1994).
    T_PLUS = 0x1E
    T_MINUS = 0x1F
    T_MUL = 0x20
    T_DIV = 0x21
    T_AND = 0x23
    T_OR = 0x24
    T_LSHIFT = 0x27
    T_RSHIFT = 0x28
    T_GT = 0x3A
    T_EQ = 0x3C
    T_DROP = 0x46
    T_DUP = 0x47
    T_OVER = 0x48
    T_SWAP = 0x49
    T_ROT = 0x4A
    T_2DROP = 0x52
    T_2DUP = 0x53
    T_MIN = 0x57
    T_R_TO = 0x30       # >r
    T_FROM_R = 0x31     # r>
    T_R_FETCH = 0x32    # r@
    T_C_FETCH = 0x71    # c@
    T_C_STORE = 0x75    # c!
    T_MOVE = 0x78       # move
    T_FILL = 0x79       # fill
    T_FALSE = 0xA3
    T_TRUE = 0xA4
    T_0 = 0xA5
    T_1 = 0xA6
    T_2 = 0xA7
    T_3 = 0xA8
    T_COLON = 0xB7      # b(:)
    T_VALUE = 0xB8      # b(value)
    T_BUFFER = 0xBD     # b(buffer:)
    T_SEMI = 0xC2       # b(;)
    T_TO = 0xC3         # b(to)
    T_CASE = 0xC4       # b(case)
    T_ENDCASE = 0xC5    # b(endcase)
    T_END_OF = 0xC6     # b(endof)
    T_I = 0x19          # i
    T_TICK = 0x11       # b(')

    T_MY_SPACE = 0x0103
    T_PROPERTY = 0x0110
    T_ENCODE_INT = 0x0111
    T_ENCODE_PLUS = 0x0112
    T_ENCODE_PHYS = 0x0113
    T_ENCODE_STRING = 0x0114
    T_ENCODE_BYTES = 0x0115
    T_MODEL = 0x0119
    T_DEVICE_TYPE = 0x011A
    T_IS_INSTALL = 0x011C
    T_IS_REMOVE = 0x011D
    T_LINE_NUM = 0x0150         # #lines
    T_COLUMN_NUM = 0x0151       # #columns
    T_FRAME_BUFFER_ADR = 0x0162
    T_WINDOW_TOP = 0x0165
    T_WINDOW_LEFT = 0x0166
    T_DEFAULT_FONT = 0x016A     # default-font
    T_SET_FONT = 0x016B
    T_CHAR_HEIGHT = 0x016C
    T_CHAR_WIDTH = 0x016D
    T_FB8_INSTALL = 0x018B
    T_CALL_PARENT = 0x0209      # $call-parent
    T_GET_MY_PROP = 0x021A      # get-my-property
    T_DECODE_INT = 0x021B
    T_L_FETCH = 0x006E          # l@ (big-endian 32-bit fetch)
    T_L_STORE = 0x0073          # l! (big-endian 32-bit store)

    # Package-private tokens (0x800+).
    V_REGS = 0x0800
    V_FB = 0x0801
    V_W = 0x0802
    V_H = 0x0803
    B_PAL = 0x0804
    W_REG_FETCH = 0x0805
    W_REG_STORE = 0x0806
    W_CLUT_IDX = 0x0807
    W_CLUT_RD = 0x0808
    W_CLUT_WR = 0x0809
    W_COLOR_GET = 0x080A
    W_COLOR_SET = 0x080B
    W_SET_COLORS = 0x080C
    W_GET_COLORS = 0x080D
    W_DRAW_RECT = 0x080E
    W_FILL_RECT = 0x080F
    W_READ_RECT = 0x0810
    W_OPEN = 0x0811
    W_CLOSE = 0x0812

    out = bytearray()

    # "name", "device_type", "model".
    out += f_str(spec['name']) + f_tok(T_ENCODE_STRING) + f_str('name') + f_tok(T_PROPERTY)
    out += f_str('display') + f_tok(T_DEVICE_TYPE)
    out += f_str(spec['model']) + f_tok(T_MODEL)

    # "reg": config space (my-space, 0, 0), BAR0 VRAM (0x02000010, 2 MiB), BAR1 regs (0x02000014, 4 KiB).
    out += (f_tok(T_0) + f_tok(T_0) + f_tok(T_MY_SPACE) + f_tok(T_ENCODE_PHYS) +
            f_tok(T_0) + f_tok(T_ENCODE_INT) + f_tok(T_ENCODE_PLUS) +
            f_tok(T_0) + f_tok(T_ENCODE_INT) + f_tok(T_ENCODE_PLUS))
    for bar_off, bar_size in ((0x02000010, 0x00200000), (0x02000014, 0x00001000)):
        out += (f_tok(T_0) + f_tok(T_0) + f_tok(T_MY_SPACE) + f_lit(bar_off) + f_tok(T_OR) +
                f_tok(T_ENCODE_PHYS) + f_tok(T_ENCODE_PLUS) +
                f_tok(T_0) + f_tok(T_ENCODE_INT) + f_tok(T_ENCODE_PLUS) +
                f_lit(bar_size) + f_tok(T_ENCODE_INT) + f_tok(T_ENCODE_PLUS))
    out += f_str('reg') + f_tok(T_PROPERTY)

    # "iso6429-1983-colors" boolean property.
    out += f_tok(T_0) + f_tok(T_0) + f_tok(T_ENCODE_BYTES) + f_str('iso6429-1983-colors') + f_tok(T_PROPERTY)

    # Default geometry so Open Firmware can choose this node as the console
    # before open fills in the mapped AAPL,address values.
    out += (f_lit(640) + f_tok(T_ENCODE_INT) + f_str('width') + f_tok(T_PROPERTY) +
            f_lit(480) + f_tok(T_ENCODE_INT) + f_str('height') + f_tok(T_PROPERTY) +
            f_lit(8) + f_tok(T_ENCODE_INT) + f_str('depth') + f_tok(T_PROPERTY) +
            f_lit(640) + f_tok(T_ENCODE_INT) + f_str('linebytes') + f_tok(T_PROPERTY))

    # Values: V_REGS, V_FB, V_W, V_H; buffer B_PAL (48 bytes for the 16-entry OF palette).
    for vtok, init in ((V_REGS, 0), (V_FB, 0), (V_W, 640), (V_H, 480)):
        out += f_lit(init) + f_new_token(vtok, T_VALUE)
    out += f_lit(48) + f_new_token(B_PAL, T_BUFFER)

    # Helper words:
    # W_REG_FETCH ( off -- val ): V_REGS + l@
    out += f_new_token(W_REG_FETCH, T_COLON) + f_tok(V_REGS) + f_tok(T_PLUS) + f_tok(T_L_FETCH) + f_tok(T_SEMI)
    # W_REG_STORE ( val off -- ): V_REGS + l!
    out += f_new_token(W_REG_STORE, T_COLON) + f_tok(V_REGS) + f_tok(T_PLUS) + f_tok(T_L_STORE) + f_tok(T_SEMI)
    # W_CLUT_IDX ( idx -- ): 0x50 W_REG_STORE
    out += f_new_token(W_CLUT_IDX, T_COLON) + f_lit(0x50) + f_tok(W_REG_STORE) + f_tok(T_SEMI)
    # W_CLUT_RD ( -- byte ): 0x54 W_REG_FETCH
    out += f_new_token(W_CLUT_RD, T_COLON) + f_lit(0x54) + f_tok(W_REG_FETCH) + f_tok(T_SEMI)
    # W_CLUT_WR ( byte -- ): 0x54 W_REG_STORE
    out += f_new_token(W_CLUT_WR, T_COLON) + f_lit(0x54) + f_tok(W_REG_STORE) + f_tok(T_SEMI)

    # Standard display methods (Designing PCI Cards, chapter 5, pp. 52):
    # color@ ( index -- r g b )
    out += (f_external_token('color@', W_COLOR_GET, T_COLON) +
            f_tok(W_CLUT_IDX) + f_tok(W_CLUT_RD) + f_tok(W_CLUT_RD) + f_tok(W_CLUT_RD) + f_tok(T_SEMI))
    # color! ( r g b index -- )
    out += (f_external_token('color!', W_COLOR_SET, T_COLON) +
            f_tok(W_CLUT_IDX) + f_tok(T_ROT) + f_tok(W_CLUT_WR) + f_tok(T_SWAP) + f_tok(W_CLUT_WR) + f_tok(W_CLUT_WR) + f_tok(T_SEMI))
    # set-colors ( adr index #indices -- )
    set_loop = (f_tok(T_DUP) + f_tok(T_I) + f_tok(T_PLUS) + f_tok(T_C_FETCH) + f_tok(W_CLUT_WR))
    out += (f_external_token('set-colors', W_SET_COLORS, T_COLON) +
            f_tok(T_SWAP) + f_tok(W_CLUT_IDX) + f_tok(T_3) + f_tok(T_MUL) + f_tok(T_0) +
            f_do(set_loop) + f_tok(T_DROP) + f_tok(T_SEMI))
    # get-colors ( adr index #indices -- )
    get_loop = (f_tok(W_CLUT_RD) + f_tok(T_OVER) + f_tok(T_I) + f_tok(T_PLUS) + f_tok(T_C_STORE))
    out += (f_external_token('get-colors', W_GET_COLORS, T_COLON) +
            f_tok(T_SWAP) + f_tok(W_CLUT_IDX) + f_tok(T_3) + f_tok(T_MUL) + f_tok(T_0) +
            f_do(get_loop) + f_tok(T_DROP) + f_tok(T_SEMI))

    # draw-rectangle ( adr x y w h -- ): copies h rows of w bytes from adr to frame-buffer-adr + y*V_W + x.
    # Stack without locals: keep ( adr dst w ) across the loop over h rows.
    # Entry: ( adr x y w h )
    # >r >r ( adr x y ) (R: h w)
    # V_W * + frame-buffer-adr + ( adr dst ) (R: h w)
    # r> r> 0 ?do ( adr dst w )
    #   >r 2dup r@ move ( adr dst ) (R: ... w)
    #   V_W + swap r@ + swap r>
    # loop 2drop drop
    draw_row = (f_tok(T_R_TO) +
                f_tok(T_2DUP) + f_tok(T_R_FETCH) + f_tok(T_MOVE) +
                f_tok(V_W) + f_tok(T_PLUS) + f_tok(T_SWAP) +
                f_tok(T_R_FETCH) + f_tok(T_PLUS) + f_tok(T_SWAP) + f_tok(T_FROM_R))
    out += (f_external_token('draw-rectangle', W_DRAW_RECT, T_COLON) +
            f_tok(T_R_TO) + f_tok(T_R_TO) +
            f_tok(V_W) + f_tok(T_MUL) + f_tok(T_PLUS) + f_tok(T_FRAME_BUFFER_ADR) + f_tok(T_PLUS) +
            f_tok(T_FROM_R) + f_tok(T_FROM_R) + f_tok(T_0) +
            f_do(draw_row) +
            f_tok(T_2DROP) + f_tok(T_DROP) + f_tok(T_SEMI))

    # fill-rectangle ( color x y w h -- ): fills h rows of w bytes at frame-buffer-adr + y*V_W + x with color.
    # Entry: ( color x y w h )
    # >r >r ( color x y ) (R: h w)
    # V_W * + frame-buffer-adr + ( color dst ) (R: h w)
    # r> r> 0 ?do ( color dst w )
    #   >r 2dup r@ rot fill ( color dst ) (R: ... w)
    #   V_W + r>
    # loop 2drop drop
    fill_row = (f_tok(T_R_TO) +
                f_tok(T_2DUP) + f_tok(T_R_FETCH) + f_tok(T_ROT) + f_tok(T_FILL) +
                f_tok(V_W) + f_tok(T_PLUS) + f_tok(T_FROM_R))
    out += (f_external_token('fill-rectangle', W_FILL_RECT, T_COLON) +
            f_tok(T_R_TO) + f_tok(T_R_TO) +
            f_tok(V_W) + f_tok(T_MUL) + f_tok(T_PLUS) + f_tok(T_FRAME_BUFFER_ADR) + f_tok(T_PLUS) +
            f_tok(T_FROM_R) + f_tok(T_FROM_R) + f_tok(T_0) +
            f_do(fill_row) +
            f_tok(T_2DROP) + f_tok(T_DROP) + f_tok(T_SEMI))

    # read-rectangle ( adr x y w h -- ): copies h rows of w bytes from frame-buffer-adr + y*V_W + x to adr.
    read_row = (f_tok(T_R_TO) +
                f_tok(T_2DUP) + f_tok(T_SWAP) + f_tok(T_R_FETCH) + f_tok(T_MOVE) +
                f_tok(V_W) + f_tok(T_PLUS) + f_tok(T_SWAP) +
                f_tok(T_R_FETCH) + f_tok(T_PLUS) + f_tok(T_SWAP) + f_tok(T_FROM_R))
    out += (f_external_token('read-rectangle', W_READ_RECT, T_COLON) +
            f_tok(T_R_TO) + f_tok(T_R_TO) +
            f_tok(V_W) + f_tok(T_MUL) + f_tok(T_PLUS) + f_tok(T_FRAME_BUFFER_ADR) + f_tok(T_PLUS) +
            f_tok(T_FROM_R) + f_tok(T_FROM_R) + f_tok(T_0) +
            f_do(read_row) +
            f_tok(T_2DROP) + f_tok(T_DROP) + f_tok(T_SEMI))

    # Open (is-install): map BAR0/BAR1 from config BARs via parent's map-in,
    # enable MEMORY, program 8-bit mode, install fb8. map-in stack is
    # ( phys.lo phys.mid phys.hi size -- virt ).
    iso_colors = (
        bytes([0x00, 0x00, 0x00,  0x00, 0x00, 0xAA,  0x00, 0xAA, 0x00,  0x00, 0xAA, 0xAA,
               0xAA, 0x00, 0x00,  0xAA, 0x00, 0xAA,  0xAA, 0x55, 0x00,  0xAA, 0xAA, 0xAA,
               0x55, 0x55, 0x55,  0x55, 0x55, 0xFF,  0x55, 0xFF, 0x55,  0x55, 0xFF, 0xFF,
               0xFF, 0x55, 0x55,  0xFF, 0x55, 0xFF,  0xFF, 0xFF, 0x55,  0xFF, 0xFF, 0xFF])
    )

    def map_bar(bar_off: int, bar_flag: int, size: int, value_tok: int) -> bytes:
        # phys.lo = config-l@ (my-space + bar_off); phys.mid = 0;
        # phys.hi = my-space | bar_flag; then map-in.
        return (f_tok(T_MY_SPACE) + f_lit(bar_off) + f_tok(T_PLUS) + f_str('config-l@') + f_tok(T_CALL_PARENT) +
                f_tok(T_0) +
                f_tok(T_MY_SPACE) + f_lit(bar_flag) + f_tok(T_OR) +
                f_lit(size) + f_str('map-in') + f_tok(T_CALL_PARENT) +
                f_tok(T_TO) + f_tok(value_tok))

    open_body = bytearray()
    # Enable PCI memory decoding first.
    open_body += (f_tok(T_MY_SPACE) + f_lit(4) + f_tok(T_PLUS) + f_str('config-w@') + f_tok(T_CALL_PARENT) +
                  f_tok(T_2) + f_tok(T_OR) +
                  f_tok(T_MY_SPACE) + f_lit(4) + f_tok(T_PLUS) + f_str('config-w!') + f_tok(T_CALL_PARENT))
    open_body += map_bar(0x10, 0x02000010, 0x00200000, V_FB)
    open_body += map_bar(0x14, 0x02000014, 0x00001000, V_REGS)
    # Read MONITOR index ($0C) and RES[monitor] ($100 + 4*monitor).
    open_body += (f_lit(0x0C) + f_tok(W_REG_FETCH) + f_lit(2) + f_tok(T_LSHIFT) + f_lit(0x100) + f_tok(T_PLUS) +
                  f_tok(W_REG_FETCH) +
                  f_tok(T_DUP) + f_lit(16) + f_tok(T_RSHIFT) + f_tok(T_TO) + f_tok(V_W) +
                  f_lit(0xFFFF) + f_tok(T_AND) + f_tok(T_TO) + f_tok(V_H))
    open_body += (f_tok(V_W) + f_lit(0x20) + f_tok(W_REG_STORE) +
                  f_tok(V_H) + f_lit(0x24) + f_tok(W_REG_STORE) +
                  f_lit(8) + f_lit(0x28) + f_tok(W_REG_STORE) +
                  f_tok(V_W) + f_lit(0x2C) + f_tok(W_REG_STORE) +
                  f_lit(0x40) + f_lit(0x30) + f_tok(W_REG_STORE) +
                  f_tok(T_1) + f_lit(0x44) + f_tok(W_REG_STORE) +
                  f_tok(T_1) + f_lit(0x40) + f_tok(W_REG_STORE))
    open_body += (f_str(iso_colors) + f_tok(T_DROP) + f_tok(B_PAL) + f_lit(48) + f_tok(T_MOVE) +
                  f_tok(B_PAL) + f_tok(T_0) + f_lit(16) + f_tok(W_SET_COLORS) +
                  f_tok(T_0) + f_tok(T_0) + f_tok(T_0) + f_lit(255) + f_tok(W_COLOR_SET))
    open_body += (f_tok(V_FB) + f_tok(T_ENCODE_INT) +
                  f_tok(V_REGS) + f_tok(T_ENCODE_INT) + f_tok(T_ENCODE_PLUS) +
                  f_str('AAPL,address') + f_tok(T_PROPERTY) +
                  f_tok(V_FB) + f_lit(0x40) + f_tok(T_PLUS) + f_tok(T_ENCODE_INT) +
                  f_str('address') + f_tok(T_PROPERTY) +
                  f_tok(V_W) + f_tok(T_ENCODE_INT) + f_str('width') + f_tok(T_PROPERTY) +
                  f_tok(V_H) + f_tok(T_ENCODE_INT) + f_str('height') + f_tok(T_PROPERTY) +
                  f_tok(V_W) + f_tok(T_ENCODE_INT) + f_str('linebytes') + f_tok(T_PROPERTY) +
                  f_lit(8) + f_tok(T_ENCODE_INT) + f_str('depth') + f_tok(T_PROPERTY))
    # Install routine for is-install: stack ( -- ). is-install builds open
    # (which returns true) around this xt; do not leave a flag here.
    open_body += (f_tok(V_FB) + f_lit(0x40) + f_tok(T_PLUS) + f_tok(T_TO) + f_tok(T_FRAME_BUFFER_ADR) +
                  f_tok(T_DEFAULT_FONT) + f_tok(T_SET_FONT) +
                  f_tok(V_W) + f_tok(V_H) +
                  f_tok(T_OVER) + f_tok(T_CHAR_WIDTH) + f_tok(T_DIV) +
                  f_tok(T_OVER) + f_tok(T_CHAR_HEIGHT) + f_tok(T_DIV) +
                  f_tok(T_FB8_INSTALL))

    # Install/remove xts for is-install / is-remove (Apple listing 5-4: named
    # my-open / my-close, not the package's open/close methods themselves).
    out += f_external_token('livadix-install', W_OPEN, T_COLON) + open_body + f_tok(T_SEMI)

    close_body = (f_tok(V_FB) + f_lit(0x00200000) + f_str('map-out') + f_tok(T_CALL_PARENT) +
                  f_tok(V_REGS) + f_lit(0x00001000) + f_str('map-out') + f_tok(T_CALL_PARENT))
    out += f_external_token('livadix-remove', W_CLOSE, T_COLON) + close_body + f_tok(T_SEMI)

    out += f_tok(T_TICK) + f_tok(W_OPEN) + f_tok(T_IS_INSTALL)
    out += f_tok(T_TICK) + f_tok(W_CLOSE) + f_tok(T_IS_REMOVE)

    # "driver,AAPL,MacOS,PowerPC" last: a large PEF; keep it after is-install so the
    # display methods are registered even if property creation is slow.
    chunk_size = 240
    for i in range(0, len(ndrv), chunk_size):
        out += f_str(ndrv[i:i + chunk_size]) + f_tok(T_ENCODE_BYTES)
        if i > 0:
            out += f_tok(T_ENCODE_PLUS)
    out += f_str('driver,AAPL,MacOS,PowerPC') + f_tok(T_PROPERTY)

    # end0 (0x00).
    out += b'\x00'

    # IEEE 1275 5.2.2.4-5: start1 selects 16-bit branch offsets;
    # the checksum is the 16-bit sum of body bytes, excluding the header.
    return struct.pack('>BBHI', 0xF1, 0x08, sum(out) & 0xFFFF, 8 + len(out)) + bytes(out)



def build_pci_rom(fcode: bytes, spec: dict) -> bytes:
    """Wraps an FCode image in a PCI Expansion ROM (PCI Local Bus Specification,
    Revision 2.0, 6.3 "Expansion ROMs"):
      +$00  ROM header (55 AA, FCode offset at +$02, PCIR offset at +$18)
      +$1C  PCI Data Structure ('PCIR', vendor, device, class code, image length
            in 512-byte units, code type 1 = Open Firmware FCode, indicator $80)"""
    vendor = int(spec['vendor_id'])
    device = int(spec['device_id'])
    class_code = int(spec['class_code'])
    revision = int(spec.get('revision_id', 1))

    # ROM header (28 bytes) + 24-byte PCI Data Structure ('PCIR') = 52;
    # FCode follows (PCI 2.0, 6.3.1.2).
    rom_hdr = bytearray(0x1C)
    fcode_off = 0x1C + 0x18
    raw_len = fcode_off + len(fcode)
    blocks = (raw_len + 511) // 512
    image_size = blocks * 512

    # PCI 2.0 6.3.1.2: 'PCIR', vendor, device, VPD, length $18, structure
    # revision, class code (3 bytes), image length (512-byte units), code
    # revision (16-bit), code type 1 (Open Firmware FCode), indicator $80
    # (last image), reserved (16-bit).
    pcir = struct.pack(
        '<4sHHHHBBBBHHBBH',
        b'PCIR',
        vendor,
        device,
        0,
        0x18,
        0,
        class_code & 0xFF,
        (class_code >> 8) & 0xFF,
        (class_code >> 16) & 0xFF,
        blocks,
        revision,
        1,
        0x80,
        0,
    )
    assert len(pcir) == 0x18

    rom_hdr[0] = 0x55
    rom_hdr[1] = 0xAA
    # PCI Bus Binding to IEEE 1275 ch. 9: the FCode pointer at +$02 is
    # little-endian (as is the PCIR pointer at +$18).
    struct.pack_into('<H', rom_hdr, 0x02, fcode_off)
    struct.pack_into('<H', rom_hdr, 0x18, 0x1C)

    image = bytes(rom_hdr) + pcir + fcode
    return image + b'\x00' * (image_size - len(image))


def build_fragment(spec: dict, bins: Path) -> bytes:
    """A code fragment of its own (`fragment`: the assembled binary in `bins`,
    whose first 16 bytes give the code size, the data size and the offset in
    the data of the main symbol's transition vector): its PEF container,
    imports and data relocations from the specification."""
    raw = (bins / spec['fragment']).read_bytes()
    code_size, data_size, main_off, _ = struct.unpack('>IIII', raw[:16])
    code = raw[16:16 + code_size]
    data = raw[16 + code_size:16 + code_size + data_size]
    libraries = [(lib['name'], lib['symbols']) for lib in spec['imports']]
    data_relocs = [(r[0], int(r[1])) for r in spec['relocations']]
    return build_pef(code, data, libraries, [], data_relocs, main_off)


def build(spec: dict, bins: Path) -> bytes:
    """Builds the PCI Livadix Display Card's expansion ROM from its specification
    and the assembled PowerPC driver code in `bins` (or, for a `fragment`
    specification, that fragment's PEF container)."""
    if 'fragment' in spec:
        return build_fragment(spec, bins)
    raw = (bins / spec['driver']).read_bytes()
    # The assembled binary starts with a 16-byte header:
    #   +0: code_size (4), data_size (4), dodriverio_tvec_off (4), descr_off (4)
    # followed by `code_size` bytes of .text and `data_size` bytes of initialized data.
    code_size, data_size, tvec_off, descr_off = struct.unpack('>IIII', raw[:16])
    code = raw[16:16 + code_size]
    data = raw[16 + code_size:16 + code_size + data_size]

    libraries = [(lib['name'], lib['symbols']) for lib in spec['imports']]
    data_relocs = [(r[0], int(r[1])) for r in spec['relocations']]
    exports = [
        ('DoDriverIO', 2, tvec_off, 1),             # kPEFTVectorSymbol = 2
        ('TheDriverDescription', 1, descr_off, 1),  # kPEFDataSymbol = 1
    ]
    ndrv = build_pef(code, data, libraries, exports, data_relocs)
    fcode = build_fcode(ndrv, spec)
    return build_pci_rom(fcode, spec)
