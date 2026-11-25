| Livadix — livadixvideo24, the Livadix Display Card's video driver for its 24-bit
| declaration ROM (src/hw/guest/declrom/livadixcard24.yaml): livadixvideo.s
| assembled with CARD24.
|
| Assembler: GNU as for m68k (binutils), -m68000; built by
| tools/guest_build.py into src/hw/guest/bin/livadixvideo24.bin.

        .set    CARD24, 1
        .include "livadixvideo.s"
