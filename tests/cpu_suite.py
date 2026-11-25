"""The CPU conformance material: the SingleStepTests 680x0 suite (68000 JSON
tests) fetched into work/cpu/sst and converted for the C runner.

    python tests/cpu_suite.py fetch     download the suite at the pinned commit,
                                        check every file's SHA-256 against the
                                        manifest written on first fetch, and
                                        convert each file to work/cpu/sst/*.sst
    python tests/cpu_suite.py run       run mc_cpu_tests over the converted suite
    python tests/cpu_suite.py status    show what is fetched

The suite is third-party test data (github.com/SingleStepTests/680x0); it is
never committed. work/cpu/sst/MANIFEST.txt records the commit and the hashes
of the downloaded files.

Converted format (little-endian), one file per JSON file:
    'SST2', u32 count, then per test:
    u16 name length, name bytes,
    initial state, final state, each:
        19 x u32: d0-d7, a0-a6, usp, ssp, sr, pc
        2 x u32: prefetch words
        u32 n, n x (u32 address, u32 byte)
    u32 length (clock cycles), u32 m, m x bus transactions of 10 bytes:
        u8 kind (0 idle, 1 read, 2 write, 3 read-modify-write), u8 cycles,
        u8 function code, u8 size (1 byte, 2 word), u32 address, u16 value
"""
from __future__ import annotations

import gzip
import hashlib
import json
import struct
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work' / 'cpu' / 'sst'
RUNNER = ROOT / 'build' / 'x64' / 'mc_cpu_tests.exe'
REPO = 'SingleStepTests/680x0'
COMMIT = 'e0d5ece9670205cc84a0101081837deb446f86a3'
FOLDER = '68000/v1'
FILES = """ABCD ADD.b ADD.l ADD.w ADDA.l ADDA.w ADDX.b ADDX.l ADDX.w AND.b AND.l AND.w ANDItoCCR ANDItoSR
ASL.b ASL.l ASL.w ASR.b ASR.l ASR.w BCHG BCLR BSET BSR BTST Bcc CHK CLR.b CLR.l CLR.w CMP.b CMP.l CMP.w
CMPA.l CMPA.w DBcc DIVS DIVU EOR.b EOR.l EOR.w EORItoCCR EORItoSR EXG EXT.l EXT.w JMP JSR LEA LINK
LSL.b LSL.l LSL.w LSR.b LSR.l LSR.w MOVE.b MOVE.l MOVE.q MOVE.w MOVEA.l MOVEA.w MOVEM.l MOVEM.w
MOVEP.l MOVEP.w MOVEfromSR MOVEfromUSP MOVEtoCCR MOVEtoSR MOVEtoUSP MULS MULU NBCD NEG.b NEG.l NEG.w
NEGX.b NEGX.l NEGX.w NOP NOT.b NOT.l NOT.w OR.b OR.l OR.w ORItoCCR ORItoSR PEA RESET ROL.b ROL.l ROL.w
ROR.b ROR.l ROR.w ROXL.b ROXL.l ROXL.w ROXR.b ROXR.l ROXR.w RTE RTR RTS SBCD SUB.b SUB.l SUB.w SUBA.l
SUBA.w SUBX.b SUBX.l SUBX.w SWAP Scc TAS TRAP TRAPV TST.b TST.l TST.w UNLINK""".split()
REGS = ['d0', 'd1', 'd2', 'd3', 'd4', 'd5', 'd6', 'd7', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6',
        'usp', 'ssp', 'sr', 'pc']


def manifest() -> dict[str, str]:
    path = WORK / 'MANIFEST.txt'
    out = {}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            if line.startswith('#') or not line.strip():
                continue
            digest, name = line.split(None, 1)
            out[name] = digest
    return out


def write_manifest(hashes: dict[str, str]):
    lines = [f'# {REPO} at {COMMIT}, folder {FOLDER}', '# sha256 of each downloaded file']
    lines += [f'{hashes[n]}  {n}' for n in sorted(hashes)]
    (WORK / 'MANIFEST.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def pack_state(s: dict) -> bytes:
    out = struct.pack('<19I', *[s[r] & 0xFFFFFFFF for r in REGS])
    out += struct.pack('<2I', *s['prefetch'])
    out += struct.pack('<I', len(s['ram']))
    out += b''.join(struct.pack('<II', a, v) for a, v in s['ram'])
    return out


MAGIC = b'SST2'
KINDS = {'n': 0, 'r': 1, 'w': 2, 't': 3}


def pack_transactions(t: dict) -> bytes:
    out = [struct.pack('<II', t['length'], len(t['transactions']))]
    for x in t['transactions']:
        if x[0] == 'n':
            out.append(struct.pack('<BBBBIH', 0, x[1], 0, 0, 0, 0))
        else:
            out.append(struct.pack('<BBBBIH', KINDS[x[0]], x[1], x[2], 1 if x[4] == '.b' else 2,
                                   x[3] & 0xFFFFFFFF, x[5] & 0xFFFF))
    return b''.join(out)


def convert(src: Path, dst: Path):
    tests = json.loads(gzip.decompress(src.read_bytes()))
    parts = [MAGIC, struct.pack('<I', len(tests))]
    for t in tests:
        name = t['name'].encode('utf-8')
        parts += [struct.pack('<H', len(name)), name, pack_state(t['initial']), pack_state(t['final']),
                  pack_transactions(t)]
    tmp = dst.with_suffix('.tmp')
    tmp.write_bytes(b''.join(parts))
    tmp.replace(dst)


def fetch() -> int:
    WORK.mkdir(parents=True, exist_ok=True)
    known = manifest()
    hashes = {}
    for name in FILES:
        fname = f'{name}.json.gz'
        gz = WORK / fname
        if not gz.exists():
            url = f'https://raw.githubusercontent.com/{REPO}/{COMMIT}/{FOLDER}/{fname}'
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
            gz.with_suffix('.part').write_bytes(data)
            gz.with_suffix('.part').replace(gz)
        digest = hashlib.sha256(gz.read_bytes()).hexdigest()
        if fname in known and known[fname] != digest:
            print(f'{fname}: hash differs from the manifest', file=sys.stderr)
            return 1
        hashes[fname] = digest
        sst = WORK / f'{name}.sst'
        if not current(sst) or sst.stat().st_mtime < gz.stat().st_mtime:
            convert(gz, sst)
    if not known:
        write_manifest(hashes)
    print(f'{len(hashes)} files at {COMMIT[:12]} in {WORK.relative_to(ROOT).as_posix()}')
    return 0


def current(sst: Path) -> bool:
    """Converted, in the current format."""
    if not sst.exists():
        return False
    with sst.open('rb') as f:
        return f.read(4) == MAGIC


def fetched() -> bool:
    return all(current(WORK / f'{n}.sst') for n in FILES)


def run(extra: list[str]) -> int:
    if not fetched():
        print('fetch first: python tests/cpu_suite.py fetch', file=sys.stderr)
        return 2
    return subprocess.run([str(RUNNER), 'sst', str(WORK)] + extra).returncode


def main(argv) -> int:
    if argv[:1] == ['fetch']:
        return fetch()
    if argv[:1] == ['run']:
        return run(argv[1:])
    if argv[:1] == ['status']:
        n = sum(current(WORK / f'{x}.sst') for x in FILES)
        print(f'{n}/{len(FILES)} files converted in {WORK}')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
