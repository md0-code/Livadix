![Livadix](assets/livadix-logo.png)

# Livadix

Livadix emulates 68k and Old World PowerPC Macintoshes, from the Macintosh
128K to the Power Macintosh G3, and the Bandai Pippin. It models their
hardware chip by chip and runs Apple's own ROMs and system software,
unmodified. It runs on Windows and Linux, on x64 and ARM64.

## First start

Extract the release and run `livadix.exe` (Windows) or `./livadix` (Linux). With
nothing else present, Livadix starts its Built-in machine: a small Macintosh
with Livadix's own ROM, whose desktop holds this README and the changelog.
Nothing else is needed for it.

To run a real Macintosh you need two things Livadix does not include:

1. **A ROM dump** of the machine. Put it in the `roms` folder beside the
   program; the file name does not matter. [ROMs](docs/NOTES.md#roms) lists every
   ROM Livadix knows.
2. **A disk image** with its system software: a hard disk image (a bare HFS
   volume or a partitioned disk), a floppy image (raw 400K/720K/800K/1.44M
   or DiskCopy 4.2) or a CD image (`.iso`, `.toast`, `.cdr` or `.bin`/`.cue`).

Then press the **Windows key** (**Super** on Linux) to open the
configuration window: **New** creates a profile, choose its **Model**, add
the disk images under **Storage** and press **Start**. **Save** writes
everything to `livadix.yaml` beside the program; the next start runs the
default profile.

## Supported machines

- **Compact Macs:** 128K, 512K/512Ke, Plus, SE/SE FDHD, SE/30, Classic,
  Classic II, Color Classic and Color Classic II.
- **68k desktops:** Macintosh II, IIx, IIcx, IIci, IIsi, IIfx, IIvx/IIvi;
  LC to LC 630 and their Performas; Macintosh TV; Centris and Quadra 605
  to 950, including the 660AV and 840AV.
- **68k portables:** Macintosh Portable; PowerBook 100 to 190 and 520/540;
  Duo 210 to 280c.
- **Power Macintoshes:** 5200 to 9600, Workgroup Servers 6150, 8150 and
  9150, Twentieth Anniversary Macintosh, Beige G3 (desktop, minitower,
  all-in-one).
- **PowerPC portables:** Duo 2300c, PowerBook 1400c/cs, PowerBook G3 Series
  (Wallstreet and PDQ).
- **Bandai Pippin**, retail and development ROMs.

Each model runs the systems it ran: from System 1 on the 128K to Mac OS
9.2.2 on the G3, and Mac OS X 10.2 on all six G3 models. `livadix --list-models`
lists the 125 models; the [configuration guide](docs/NOTES.md#models)
gives each one's ROM, memory, display and supported systems. What Livadix does
not emulate is in [Limitations](docs/NOTES.md#limitations).

## Features

- Profiles: any number of machines, each with its disks and settings, in
  one `livadix.yaml`; PRAM and clock kept per profile and per startup disk.
- Fast: the 68k and PowerPC code is translated to host code; an optional
  68k accelerator speeds up the 68k code of Power Macintoshes.
- Display: CRT and LCD looks, sharp scaling, full screen; a Livadix display
  card adds resolutions up to 1920x1080 and accelerated drawing.
- Media: hard disks, floppies and CDs with audio tracks; images dropped on
  the window are inserted.
- Ethernet to the Internet through the host (no setup, no administrator
  rights), and host folders shared with the Macintosh as AppleShare volumes.
- Serial ports to host ports, pipes or files; sound output and input.

## Controls

| Action | Keys |
|---|---|
| Capture the mouse / release it | Click in the window / Ctrl+Alt |
| Configuration window | Windows key (Super on Linux) |
| Command / Option | Left Alt / Right Alt |
| Full screen | Alt+Enter |
| Reinsert an ejected disk | Ctrl+Alt+R |
| Programmer's switch: interrupt / reset (68000 Macs) | Ctrl+Alt+F7 / Ctrl+Alt+F8 |

Closing the window shuts the Macintosh down cleanly first; closing it again
quits at once. More keys are in the
[configuration guide](docs/NOTES.md#machine-controls).

## When something does not start

`livadix --inspect` checks a configuration without starting it: which ROM file
each profile uses, what each disk image holds and whether the model runs
its system (a missing System Enabler, too little memory). When a profile
cannot start, Livadix opens the configuration window and says why.

## Documentation

The guides are in the `docs` folder:

- [Notes](docs/NOTES.md):
  - [Configuration](docs/NOTES.md#configuration): every setting, the models,
    the configuration window, networking, file sharing and the command line.
  - [ROMs](docs/NOTES.md#roms): the ROMs and firmware Livadix recognises, with
    their checksums.
  - [Emulated hardware](docs/NOTES.md#emulated-hardware): the processors and
    chips of each machine.
  - [Limitations](docs/NOTES.md#limitations): what is not emulated.
- [Changelog](docs/CHANGELOG.md).

## Building from source

Windows: Visual Studio's C++ tools, CMake 3.24+, Ninja, Python 3 and
PowerShell. `pwsh -File tools/build.ps1` builds and tests;
`pwsh -File tools/build_dist.ps1` makes the release (`-Arch arm64` for
ARM64). SDL is fetched and linked statically.

Linux (Debian 12, WSL included): `tools/build_linux.sh` builds, tests and
packages the x64 program, and with `arm64` cross-compiles the ARM64 one;
its header lists the packages it needs. The program needs a 64-bit glibc
system with X11 or Wayland.

## License

GPL 3.0 ([LICENSE](LICENSE)); bundled components in
[THIRD_PARTY.md](docs/THIRD_PARTY.md). No Apple ROM or software is included.
