| Livadix — the PrimaryInit of the Livadix Display Card's 24-bit declaration ROM
| (src/hw/guest/declrom/livadixcard24.yaml): livadixcard_init.s assembled with
| CARD24.
|
| Assembler: GNU as for m68k (binutils), -m68020; built by
| tools/guest_build.py into src/hw/guest/bin/livadixcard_init24.bin.

        .set    CARD24, 1
        .include "livadixcard_init.s"
