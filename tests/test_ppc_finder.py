"""Original synthetic regression fixtures; no Apple screen/font data."""
import importlib.util
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('livadix_verify', root / 'tests' / 'verify.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)
work = root / 'work' / 'verify' / 'finder-fixtures'
work.mkdir(parents=True, exist_ok=True)
w, h = 640, 480
stride = w * 3

def bitmap(name, titles):
    pixels = bytearray(b'\xff' * (stride * h))
    def black(x, y):
        at = (h - 1 - y) * stride + 3 * x
        pixels[at:at + 3] = b'\0\0\0'
    for x in range(w):
        black(x, 19)
    for x in range(470, 510):
        black(x, 10)  # A clock alone used to satisfy the old global test.
    if titles:
        for x in range(40, 80):
            black(x, 10)
    header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    path = work / name
    path.write_bytes(header + pixels)
    return path

clock = bitmap('clock-only.bmp', False)
verify.finder_menu_bar(clock, (w, h))  # reproduce the prior false positive
try:
    verify.finder_menu_bar(clock, (w, h), titles=True)
except AssertionError as exc:
    assert 'titles' in str(exc), exc
else:
    raise AssertionError('clock-only startup was accepted as the Finder')
verify.finder_menu_bar(bitmap('menu-titles.bmp', True), (w, h), titles=True)
print('PowerPC Finder: clock-only startup rejected; menu titles accepted')
