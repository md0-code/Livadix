# Notes

What the [README](../README.md) leaves out: [Configuration](#configuration), [ROMs](#roms),
[Emulated hardware](#emulated-hardware) and [Limitations](#limitations).

## Configuration

Livadix reads `livadix.yaml` from the working directory; without it, it runs its
embedded defaults and creates no file. **Save** in the configuration window
writes `livadix.yaml`, every setting explained in its comments; `--config FILE`
names another file. Paths in the file are relative to it. Unknown or
duplicate keys and invalid values are refused with the line (and profile)
at fault. `livadix --inspect` checks a configuration without starting it.

### Profiles

A profile pairs a Macintosh model with its disks and settings. All profiles
live in one file under `profiles:`; the sections at the top level are shared,
and a profile overrides any key it writes.

```yaml
default: "Quadra 700 – System 7.1"     # the profile started without --profile

emulation:                             # shared by every profile
  jit: true
video:
  filter: sharp

profiles:
  "Quadra 700 – System 7.1":
    model: quadra-700
    emulation: {ram_mb: 20}
    storage:
      disks:
        - image: disks/system-7.1.img
  "Quadra 700 – Mac OS 8.0":
    model: quadra-700
    emulation: {ram_mb: 36}
    storage:
      disks:
        - image: disks/mac-os-8.0.img
```

| Top-level key | Value |
|---|---|
| `default` | The profile started without `--profile`; else the first one |
| `profiles` | Profile names (up to 127 bytes) and their settings; at most 64 |
| `model`, `emulation`, `video`, `storage`, `sound`, `input`, `ports`, `network`, `sharing` | Shared settings (below) |

A profile's keys override the shared ones one by one; a list
(`storage.disks`, `storage.floppies`, `network.forward`, `sharing.folders`)
or a port replaces the shared one whole. A profile without `model` takes the
shared one, else `builtin`. A shared value the model cannot take (too much
memory, a screen it cannot show) gives way to the model's own; the same
value written in the profile is an error. `livadix --profile NAME` starts a
profile; a file without `profiles:` is a single configuration.

Each profile keeps its PRAM, clock and the machine's other nonvolatile
memory in the `nvram` folder beside the configuration, separately for each
model, ROM and startup disk: switching a profile's disk between, say,
Mac OS 9 and Mac OS X restores each system's own settings. Save keeps a
profile's records through renames; Duplicate starts fresh ones. Save before
moving a configuration folder. Older PRAM files are kept unchanged and never
imported.

### Settings

#### Model

| Key | Value |
|---|---|
| `model` | `builtin` (default) or a model key from [Models](#models). The model supplies the ROM, memory, clock and screen; any of them written in the file wins. `auto` from earlier files reads as `builtin` |

#### Emulation

| Key | Value |
|---|---|
| `emulation.rom` | A ROM file to use; without it Livadix finds the model's ROM in the `roms` folders ([ROMs](#roms)) |
| `emulation.ram_mb` | Memory in MiB, up to the real machine's maximum; another size is raised to the next one the board takes. Decimals for the half sizes (`2.5` on the Plus and SE). Default from the model |
| `emulation.cpu_mhz` | Processor clock, 1 to 10000 MHz (default the model's). 68k timing follows Motorola's tables; PowerPC timing is approximate, so the same MHz is not the same speed as the real machine |
| `emulation.jit` | `true` (default): guest code is translated to host code; `false` interprets it. Results and guest timing are the same |
| `emulation.m68k_accelerator` | `false` (default) or `true`, Power Macintoshes with `jit`: runs Mac OS's 68k code on Livadix's own 68k core instead of Apple's 68k emulator, several times faster for 68k-heavy software |
| `emulation.startup_sound` | `true` (default) or `false`: mutes the startup chime |
| `emulation.fpu_full` | `false` (default) or `true`, 68040 models: `true` runs the whole 68881 instruction set in hardware instead of leaving transcendental functions to the system's emulation package, as the real 68040 does |
| `emulation.pmmu` | `false` (default) or `true`, Macintosh II: fits a 68851 PMMU, which virtual memory and 32-bit addressing (with MODE32) need |
| `emulation.charger_start` | `false` (default) or `true`, PowerBook 100: starts as when the charger is plugged in while off (the battery-charging screen) |
| `emulation.controller` | `auto` (default), `protocol` or `firmware`: how Egret, Cuda and the PowerBook Power Managers are emulated. `firmware` runs Apple's firmware for them, which must be in the `roms` folders ([ROMs](#controller-firmware-and-card-roms)); `auto` uses it when present |
| `emulation.close_action` | `shutdown` (default): closing the window presses the power key and Shut Down, and Livadix quits when the Macintosh is off (closing again quits at once); `quit` quits at once, like pulling the plug |
| `emulation.close_timeout` | Seconds a shutdown may take before Livadix quits anyway: 1 to 3600, default 30 |
| `emulation.nvram` | Base path of the profile's PRAM records; default `nvram/<configuration>-<profile>.nvram` |
| `emulation.nvram_id` | The profile's identity, written by Livadix; leave it alone |

#### Video

| Key | Value |
|---|---|
| `video.resolution` | Screen size, one of the [resolutions](#resolutions); default from the model |
| `video.depth` | 1, 2, 4, 8, 16 or 32 bits; default from the model |
| `video.refresh` | Screen updates per second: 60 (default), 30, 20, 15, 12 or 10 |
| `video.card` | `auto` (default): the built-in video when it can show the mode, else the Livadix Display Card in a free slot; `builtin`; `livadix` (the card always) |
| `video.acceleration` | `true` (default) or `false`: the Livadix Display Card's accelerated QuickDraw (fills and copies on the card's screen, same pixels, faster) |
| `video.look` | `plain`, `crt` (curved glass, scanlines, glow) or `lcd` (a faint pixel grid); the model's kind of screen only. Default from the model |
| `video.monitor` | `color`, `mono` (white monochrome CRT) or `lcd-gray` (grayscale PowerBook panel), as the model's display allows ([Display](#display)) |
| `video.filter` | `sharp` (default: crisp pixels, edges blended), `nearest` or `linear` |
| `video.scale` | `fit` (default): the largest picture that fits; `integer`: the largest whole multiple |
| `video.fullscreen` | `false` (default) or `true`: start in borderless full screen; Alt+Enter switches |
| `video.vsync` | `true` (default) or `false`: show frames at the display's vertical blank |

#### Storage

| Key | Value |
|---|---|
| `storage.disks` | Up to ten hard disks: `image`, optional `readonly`, `scsi_id` and `bus` (`scsi`, or `ata` for the internal IDE disk of the models that have one). See [SCSI IDs](#scsi-ids) |
| `storage.floppies` | Up to two drives in order: `{}` for an empty drive, `off`, or `image` with optional `readonly`. Default one empty drive |
| `storage.cdrom` | A disc image (read-only), or `{image, bus}` with `bus: ata` for the drive in a PowerBook's expansion bay |
| `storage.boot_priority` | `disk` (default) or `cdrom`: start from the CD first |

| Drive | Images |
|---|---|
| Hard disk | A bare HFS or HFS+ volume, or an Apple-partitioned disk |
| Floppy | Raw 400, 720, 800 or 1440 KiB, or DiskCopy 4.2 |
| CD-ROM | HFS, ISO 9660 or hybrid: 2048-byte sectors (`.iso`, `.toast`, `.cdr`), raw 2352-byte sectors (`.bin`), or a `.cue` sheet with its data and audio tracks |

Hard disks and floppies are written in place (DiskCopy images keep a valid
checksum); `readonly: true` protects an image, and keeping a copy of an
image you want to return to is wise. Writes reach the host disk within about
a second and when Livadix quits. An image dropped on the window goes into an
empty floppy drive or the CD drive; `Ctrl+Alt+R` reinserts an ejected disk.

#### Sound

| Key | Value |
|---|---|
| `sound.enabled` | `true` (default) or `false`: the Macintosh's sound |
| `sound.floppy` | `true` (default) or `false`: synthesized floppy drive sounds, independent of the above |

Models with sound input (the LC, LC II, Classic II, Color Classic, LC
475/575, Quadra 605, the 630 and 580 families, the AV Quadras, the
PowerBook 1400 and Duo 2300c) record from the host's default
microphone, which Livadix opens only while the Macintosh records.

#### Input

| Key | Value |
|---|---|
| `input.keyboard.layout` | `usa` (default) or `iso` |
| `input.keyboard.command` | The host key for Command: `left-alt` (default), `right-alt`, `left-control` or `right-control`; the other Alt key is Option |
| `input.mouse.left`, `input.mouse.right` | `normal` (default), `disabled` or `control-click`; one stays `normal` |

#### Ports

`ports.a` (modem) and `ports.b` (printer) connect the Macintosh's serial
ports to one of:

| Value | Windows | Linux |
|---|---|---|
| `serial:` | `COMn`, at the speed set in Device Manager or `mode` | a tty such as `/dev/ttyUSB0`, at the speed set with `stty` |
| `pipe:` | `'\\.\pipe\name'` | a Unix-domain socket path |
| `output:` | a file that receives what the Macintosh sends | the same |
| `disabled: true` | no port | the same |

DTR, DCD and CTS pass through a serial port. A pipe is joined when another
program serves it, otherwise Livadix serves it; while nothing is connected,
what the Macintosh sends is dropped.

#### Network

| Key | Value |
|---|---|
| `network.mode` | `nat` (default): the Ethernet reaches the Internet through the host's connections, with no setup or administrator rights; `none`: no cable |
| `network.card` | `auto` (default), `none`, `nubus` or `lc`: the Ethernet card of a model without built-in Ethernet (Apple's EtherTalk NB Card in a NuBus slot, or the Ethernet LC Card in the LC-PDS slot). It needs the card's ROM in the `roms` folders |
| `network.forward` | Up to 16 host ports forwarded to the Macintosh: `tcp:HOSTPORT:GUESTPORT` or `udp:...`, optionally `tcp:0.0.0.0:2121:21` to listen beyond the host |

| Address | |
|---|---|
| 10.0.2.15 | The Macintosh, by DHCP (Open Transport) or BOOTP (MacTCP); by hand, mask 255.255.255.0 |
| 10.0.2.2 | The router, and the host itself (its 127.0.0.1) |
| 10.0.2.3 | DNS, answered by the host's resolver |

TCP, UDP and ping go out through the host. IPv6 and AppleTalk to other
machines are not carried.

#### Sharing

Livadix serves host folders to the Macintosh as AppleShare volumes, from a
file server named "Livadix" on its Ethernet; nothing is installed on the
Macintosh.

| Key | Value |
|---|---|
| `sharing.folders` | Up to 8 folders: `path`, optional `name` (the volume's name, up to 27 characters) and `readonly` |
| `sharing.name` | The server's name: `Livadix` (default) |
| `sharing.metadata` | `streams` (default, Windows): Mac file information in NTFS named streams; `appledouble`: in `._name` files. Drives without named streams, and Linux, use AppleDouble |
| `sharing.password` | Empty (default): guest access; else up to 8 characters for a registered user |

To connect: make AppleTalk use Ethernet (the AppleTalk control panel, or
EtherTalk in the Network control panel before Mac OS 7.6), open the Chooser,
click AppleShare, select "Livadix", connect as Guest and choose the volumes.
Changes on either side show on the other within seconds. Mac names map to
host names reversibly (characters Windows forbids become private-use
characters, as Services for Macintosh did); a host name the Macintosh cannot
spell is shortened with an ID.

```yaml
sharing:
  folders:
    - {name: PC, path: C:/Users/Me/MacShare}
    - {name: Archive, path: D:/Old Mac Files, readonly: true}
```

### Models

Every model below is emulated. The model supplies the defaults; the
configuration window lists only the models whose ROM is present.
"Systems" is the range of system software the model runs; the enabler is
the one a System from 7.1 to 7.5.x needs on that model. A PowerPC model
always has its FPU and runs 68k code in its ROM's own 68LC040 emulator.

| Model | Name | ROM | CPU | FPU | RAM MiB | Display | Systems | Enabler for 7.1-7.5.x |
|---|---|---|---|---|---|---|---|---|
| `builtin` | Built-in | `4C564458` | 68000 7.8336 MHz | no | 128 KiB | 512×342×1 | its own desktop | — |
| `mac-128k` | Macintosh 128K | `28BA61CE` | 68000 7.8336 MHz | no | 128 KiB | 512×342×1 | 1.0-3.2 | — |
| `mac-512k` | Macintosh 512K | `28BA4E50` | 68000 7.8336 MHz | no | 512 KiB | 512×342×1 | 1.1-4.1 | — |
| `mac-512ke` | Macintosh 512Ke | `4D1F8172` | 68000 7.8336 MHz | no | 512 KiB | 512×342×1 | 3.0-6.0.8 | — |
| `mac-plus` | Macintosh Plus | `4D1F8172` | 68000 7.8336 MHz | no | 4 | 512×342×1 | 3.0-7.5.5 | — |
| `mac-plus-v1` | Macintosh Plus (first ROM) | `4D1EEEE1` | 68000 7.8336 MHz | no | 4 | 512×342×1 | 3.0-7.5.5 | — |
| `mac-plus-v2` | Macintosh Plus (second ROM) | `4D1EEAE1` | 68000 7.8336 MHz | no | 4 | 512×342×1 | 3.0-7.5.5 | — |
| `mac-se` | Macintosh SE | `B2E362A8` | 68000 7.8336 MHz | no | 4 | 512×342×1 | 4.0-7.5.5 | — |
| `mac-se-fdhd` | Macintosh SE FDHD | `B306E171` | 68000 7.8336 MHz | no | 4 | 512×342×1 | 6.0.2-7.5.5 | — |
| `mac-classic` | Macintosh Classic | `A49F9914` | 68000 7.8336 MHz | no | 4 | 512×342×1 | 6.0.7-7.5.5 (ROM disk 6.0.3) | — |
| `mac-portable` | Macintosh Portable | `96CA3846` | 68HC000 15.6672 MHz | no | 4-9 | 640×400×1 | 6.0.4-7.5.5 | — |
| `powerbook-100` | PowerBook 100 | `96645F9C` | 68HC000 15.6672 MHz | no | 4-8 | 640×400×1 | 7.0.1-7.5.5 | — |
| `mac-ii-v1` | Macintosh II (1987 ROM) | `97851DB6` | 68020 15.6672 MHz | yes | 4-8 | 640×480×8 | 4.0-7.5.5 (24-bit ROM) | — |
| `mac-ii-v2` | Macintosh II | `9779D2C4` | 68020 15.6672 MHz | yes | 4-8 | 640×480×8 | 4.0-7.5.5 (24-bit ROM) | — |
| `mac-iix` | Macintosh IIx | `97221136` | 68030 15.6672 MHz | yes | 4-128 | 640×480×8 | 6.0-7.5.5 (24-bit ROM) | — |
| `mac-ii-fdhd` | Macintosh II (FDHD) | `97221136` | 68020 15.6672 MHz | yes | 4-68 | 640×480×8 | 6.0.2-7.5.5 (24-bit ROM) | — |
| `mac-iicx` | Macintosh IIcx | `97221136` | 68030 15.6672 MHz | yes | 4-128 | 640×480×8 | 6.0.3-7.5.5 (24-bit ROM) | — |
| `se-30` | Macintosh SE/30 | `97221136` | 68030 15.6672 MHz | yes | 4-128 | 512×342×1 | 6.0.3-7.5.5 (24-bit ROM) | — |
| `iici` | Macintosh IIci | `368CADFE` | 68030 25 MHz | yes | 4-128 | 640×480×8 | 6.0.4-7.6.1 | — |
| `iifx` | Macintosh IIfx | `4147DD77` | 68030 40 MHz | yes | 4-128 | 640×480×8 | 6.0.5-7.6.1 | — |
| `lc` | Macintosh LC | `350EACF0` | 68020 16 MHz | no | 4-10 | 512×384×8 | 6.0.7-7.5.5 | — |
| `iisi` | Macintosh IIsi | `36B7FB6C` | 68030 20 MHz | no | 5-65 | 640×480×8 | 6.0.7-7.6.1 | — |
| `classic-ii` | Macintosh Classic II | `3193670E` | 68030 16 MHz | no | 4-10 | 512×342×1 | 7.0.1-7.6.1 (also 6.0.8L) | — |
| `quadra-700` | Macintosh Quadra 700 | `420DBFF3` | 68040 25 MHz | yes | 4-68 | 1152×870×8 | 7.0.1-8.1 | — |
| `quadra-900` | Macintosh Quadra 900 | `420DBFF3` | 68040 25 MHz | yes | 4-256 | 1152×870×8 | 7.0.1-8.1 | — |
| `powerbook-140` | PowerBook 140 | `420DBFF3` | 68030 16 MHz | no | 4-8 | 640×400×1 | 7.0.1-7.6.1 | — |
| `powerbook-145` | PowerBook 145 | `420DBFF3` | 68030 25 MHz | no | 4-8 | 640×400×1 | 7.1-7.6.1 | — |
| `powerbook-145b` | PowerBook 145B | `420DBFF3` | 68030 25 MHz | no | 4-8 | 640×400×1 | 7.1-7.6.1 | — |
| `powerbook-170` | PowerBook 170 | `420DBFF3` | 68030 25 MHz | yes | 4-8 | 640×400×1 | 7.0.1-7.6.1 | — |
| `lc-ii` | Macintosh LC II | `35C28F5F` | 68030 16 MHz | no | 4-10 | 512×384×8 | 7.0.1-7.6.1 (also 6.0.8L) | — |
| `quadra-950` | Macintosh Quadra 950 | `3DC27823` | 68040 33 MHz | yes | 8-256 | 1152×870×8 | 7.0.1-8.1 | — |
| `iivx` | Macintosh IIvx | `4957EB49` | 68030 32 MHz | yes | 4-68 | 640×480×8 | 7.1-7.6.1 | 001 |
| `iivi` | Macintosh IIvi | `4957EB49` | 68030 16 MHz | no | 4-68 | 640×480×8 | 7.1-7.6.1 | 001 |
| `performa-600` | Macintosh Performa 600 | `4957EB49` | 68030 32 MHz | no | 4-68 | 640×480×8 | 7.1-7.6.1 | 304 |
| `powerbook-160` | PowerBook 160 | `E33B2724` | 68030 25 MHz | no | 4-14 | 640×400×4 | 7.1-7.6.1 | 131 |
| `powerbook-165` | PowerBook 165 | `E33B2724` | 68030 33 MHz | no | 4-14 | 640×400×4 | 7.1-7.6.1 | 131 |
| `powerbook-165c` | PowerBook 165c | `E33B2724` | 68030 33 MHz | yes | 4-14 | 640×400×8 | 7.1-7.6.1 | 131 |
| `powerbook-180` | PowerBook 180 | `E33B2724` | 68030 33 MHz | yes | 4-14 | 640×400×4 | 7.1-7.6.1 | 131 |
| `powerbook-180c` | PowerBook 180c | `E33B2724` | 68030 33 MHz | yes | 4-14 | 640×480×8 | 7.1-7.6.1 | 131 |
| `powerbook-duo-210` | PowerBook Duo 210 | `ECFA989B` | 68030 25 MHz | no | 4-24 | 640×400×4 | 7.1-7.6.1 | 201 |
| `powerbook-duo-230` | PowerBook Duo 230 | `ECFA989B` | 68030 33 MHz | no | 4-24 | 640×400×4 | 7.1-7.6.1 | 201 |
| `powerbook-duo-250` | PowerBook Duo 250 | `ECFA989B` | 68030 33 MHz | no | 4-24 | 640×400×4 | 7.1-7.6.1 | PowerBook Duo Enabler |
| `lc-iii-v1` | Macintosh LC III (early ROM) | `EC904829` | 68030 25 MHz | no | 4-36 | 640×480×8 | 7.1-7.6.1 | 003 |
| `lc-iii` | Macintosh LC III | `ECBBC41C` | 68030 25 MHz | no | 4-36 | 640×480×8 | 7.1-7.6.1 | 003 |
| `lc-iii-plus` | Macintosh LC III+ | `ECBBC41C` | 68030 33 MHz | no | 4-36 | 640×480×8 | 7.1-7.6.1 | 003 v1.1 |
| `color-classic` | Macintosh Color Classic | `ECD99DC0` | 68030 16 MHz | no | 4-10 | 512×384×8 | 7.1-7.6.1 | 401 |
| `centris-610` | Macintosh Centris 610 | `F1A6F343` | 68LC040 20 MHz | no | 4-68 | 640×480×8 | 7.1-8.1 | 040 |
| `centris-650` | Macintosh Centris 650 | `F1A6F343` | 68040 25 MHz | yes | 8-136 | 640×480×8 | 7.1-8.1 | 040 |
| `quadra-800` | Macintosh Quadra 800 | `F1ACAD13` | 68040 33 MHz | yes | 8-136 | 1152×870×8 | 7.1-8.1 | 040 |
| `quadra-610` | Macintosh Quadra 610 | `F1ACAD13` | 68040 25 MHz | yes | 8-68 | 640×480×8 | 7.1-8.1 | 040 |
| `quadra-650` | Macintosh Quadra 650 | `F1ACAD13` | 68040 33 MHz | yes | 8-136 | 640×480×8 | 7.1-8.1 | 040 |
| `powerbook-duo-270c` | PowerBook Duo 270c | `0024D346` | 68030 33 MHz | yes | 4-32 | 640×480×8 | 7.1-7.6.1 | PowerBook Duo Enabler |
| `color-classic-ii` | Macintosh Color Classic II | `EDE66CBD` | 68030 33 MHz | no | 4-36 | 512×384×8 | 7.1-7.6.1 | 403 |
| `lc-520` | Macintosh LC 520 | `EDE66CBD` | 68030 25 MHz | no | 4-36 | 640×480×8 | 7.1-7.6.1 | 403 |
| `lc-550` | Macintosh LC 550 | `EDE66CBD` | 68030 33 MHz | no | 5-36 | 640×480×8 | 7.1-7.6.1 | 403 |
| `performa-275` | Macintosh Performa 275 | `EDE66CBD` | 68030 33 MHz | no | 4-36 | 512×384×8 | 7.1-7.6.1 | 403 |
| `performa-550` | Macintosh Performa 550 | `EDE66CBD` | 68030 33 MHz | no | 5-36 | 640×480×8 | 7.1-7.6.1 | 332 or 364 |
| `performa-560` | Macintosh Performa 560 | `EDE66CBD` | 68030 33 MHz | no | 5-36 | 640×480×8 | 7.1-7.6.1 | 332 or 364 |
| `macintosh-tv` | Macintosh TV | `EAF1678D` | 68030 32 MHz | no | 5-8 | 640×480×8 | 7.1-7.6.1 | 404 |
| `quadra-605` | Macintosh Quadra 605 | `FF7439EE` | 68LC040 25 MHz | no | 4-36 | 832×624×8 | 7.1-8.1 | 065 |
| `lc-475` | Macintosh LC 475 | `FF7439EE` | 68LC040 25 MHz | no | 4-36 | 832×624×8 | 7.1-8.1 | 065 |
| `performa-475` | Macintosh Performa 475 | `FF7439EE` | 68LC040 25 MHz | no | 4-36 | 832×624×8 | 7.1-8.1 | 364 |
| `performa-476` | Macintosh Performa 476 | `FF7439EE` | 68LC040 25 MHz | no | 4-36 | 832×624×8 | 7.1-8.1 | 364 |
| `lc-575` | Macintosh LC 575 | `FF7439EE` | 68LC040 33 MHz | no | 5-36 | 640×480×16 | 7.1-8.1 | 065 |
| `performa-575` | Macintosh Performa 575 | `FF7439EE` | 68LC040 33 MHz | no | 5-36 | 640×480×16 | 7.1-8.1 | 364 |
| `performa-577` | Macintosh Performa 577 | `FF7439EE` | 68LC040 33 MHz | no | 5-36 | 640×480×16 | 7.1-8.1 | 364 |
| `performa-578` | Macintosh Performa 578 | `FF7439EE` | 68LC040 33 MHz | no | 8-36 | 640×480×16 | 7.1-8.1 | 364 |
| `powerbook-duo-280` | PowerBook Duo 280 | `015621D7` | 68LC040 33 MHz | no | 4-40 | 640×480×4 | 7.1.1-8.1 | PowerBook Duo Enabler |
| `powerbook-duo-280c` | PowerBook Duo 280c | `015621D7` | 68LC040 33 MHz | no | 4-40 | 640×480×8 | 7.1.1-8.1 | PowerBook Duo Enabler |
| `powerbook-520` | PowerBook 520 | `B6909089` | 68LC040 25 MHz | no | 4-36 | 640×480×4 | 7.1.1-8.1 | PowerBook 500 Series Enabler |
| `powerbook-520c` | PowerBook 520c | `B6909089` | 68LC040 25 MHz | no | 4-36 | 640×480×8 | 7.1.1-8.1 | PowerBook 500 Series Enabler |
| `powerbook-540` | PowerBook 540 | `B6909089` | 68LC040 33 MHz | no | 4-36 | 640×480×8 | 7.1.1-8.1 | PowerBook 500 Series Enabler |
| `powerbook-540c` | PowerBook 540c | `B6909089` | 68LC040 33 MHz | no | 4-36 | 640×480×8 | 7.1.1-8.1 | PowerBook 500 Series Enabler |
| `quadra-630` | Macintosh Quadra 630 | `06684214` | 68040 33 MHz | yes | 8-36 | 832×624×8 | 7.1.2-8.1 | 405 |
| `lc-630` | Macintosh LC 630 | `06684214` | 68LC040 33 MHz | no | 8-36 | 832×624×8 | 7.1.2-8.1 | 405 |
| `performa-630` | Macintosh Performa 630 | `06684214` | 68LC040 33 MHz | no | 8-36 | 832×624×8 | 7.1.2-8.1 | 405 |
| `powerbook-150` | PowerBook 150 | `FDA22562` | 68030 33 MHz | no | 4-40 | 640×480×2 | 7.1.1-7.6.1 | PowerBook 150 Enabler |
| `quadra-840av` | Macintosh Quadra 840AV | `5BF10FD1` | 68040 40 MHz | yes | 8-128 | 1152×870×8 | 7.1-8.1 | 088 |
| `quadra-660av` | Macintosh Quadra 660AV | `5BF10FD1` | 68040 25 MHz | yes | 8-68 | 640×480×16 | 7.1-8.1 | 088 |
| `performa-580` | Macintosh Performa 580 | `064DC91D` | 68LC040 33 MHz | no | 8-52 | 640×480×16 | 7.5.1-8.1 | — |
| `lc-580` | Macintosh LC 580 | `064DC91D` | 68LC040 33 MHz | no | 8-52 | 640×480×16 | 7.5.1-8.1 | — |
| `performa-588` | Macintosh Performa 588 | `064DC91D` | 68LC040 33 MHz | no | 8-52 | 640×480×16 | 7.5.1-8.1 | — |
| `powerbook-190` | PowerBook 190 | `4D27039C` | 68LC040 33 MHz | no | 8-40 | 640×480×4 | 7.5.2-8.1 | PowerBook 5300/2300/190 Enabler (before 7.6) |
| `powerbook-190cs` | PowerBook 190cs | `4D27039C` | 68LC040 33 MHz | no | 8-40 | 640×480×8 | 7.5.2-8.1 | PowerBook 5300/2300/190 Enabler (before 7.6) |
| `power-macintosh-6100` | Power Macintosh 6100 | `9FEB69B3` | PowerPC 601 60 MHz | yes | 8-72 | 640×480×8 | 7.1.2-9.1 | PowerPC Enabler |
| `power-macintosh-7100` | Power Macintosh 7100 | `9FEB69B3` | PowerPC 601 66 MHz | yes | 8-136 | 640×480×8 | 7.1.2-9.1 | PowerPC Enabler |
| `power-macintosh-8100` | Power Macintosh 8100 | `9FEB69B3` | PowerPC 601 80 MHz | yes | 8-264 | 640×480×8 | 7.1.2-9.1 | PowerPC Enabler |
| `workgroup-server-6150` | Workgroup Server 6150 | `9FEB69B3` | PowerPC 601 66 MHz | yes | 16-72 | 640×480×8 | 7.5-9.1 | PowerPC Enabler 1.1.1 (before 7.5.3) |
| `workgroup-server-8150` | Workgroup Server 8150 | `9FEB69B3` | PowerPC 601 80 MHz | yes | 16-264 | 640×480×8 | 7.1.2-9.1 | PowerPC Enabler |
| `workgroup-server-9150-80` | Workgroup Server 9150/80 | `9C7C98F7` | PowerPC 601 80 MHz | yes | 16-264 | 640×480×8 | 7.1.2-9.1 | PowerPC Enabler |
| `power-macintosh-7100-80` | Power Macintosh 7100/80 | `9B7A3AAD` | PowerPC 601 80 MHz | yes | 8-136 | 640×480×8 | 7.5-9.1 | PowerPC Enabler 1.1.1 (before 7.5.3) |
| `power-macintosh-8100-100` | Power Macintosh 8100/100 | `9B7A3AAD` | PowerPC 601 100 MHz | yes | 16-264 | 640×480×8 | 7.5-9.1 | PowerPC Enabler 1.1.1 (before 7.5.3) |
| `power-macintosh-8100-110` | Power Macintosh 8100/110 | `9B7A3AAD` | PowerPC 601 110 MHz | yes | 16-264 | 640×480×8 | 7.5-9.1 | PowerPC Enabler 1.1.1 (before 7.5.3) |
| `workgroup-server-9150-120` | Workgroup Server 9150/120 | `9B037F6F` | PowerPC 601 120 MHz | yes | 16-264 | 640×480×8 | 7.5-9.1 | PowerPC Enabler 1.1.1 (before 7.5.3) |
| `power-macintosh-5200` | Power Macintosh 5200 | `63ABFD3F` | PowerPC 603 75 MHz | yes | 8-64 | 640×480×16 | 7.5.1-9.1 | 406 (before 7.5.3) |
| `power-macintosh-5300` | Power Macintosh 5300 | `63ABFD3F` | PowerPC 603e 100 MHz | yes | 16-64 | 640×480×16 | 7.5.1-9.1 | 406 (before 7.5.3) |
| `power-macintosh-6200` | Power Macintosh 6200 | `63ABFD3F` | PowerPC 603 75 MHz | yes | 8-64 | 640×480×16 | 7.5.1-9.1 | 406 (before 7.5.3) |
| `power-macintosh-6300` | Power Macintosh 6300 | `63ABFD3F` | PowerPC 603e 100 MHz | yes | 16-64 | 640×480×16 | 7.5.1-9.1 | 406 (before 7.5.3) |
| `power-macintosh-7200-v1` | Power Macintosh 7200 (first ROM) | `96CD923D` | PowerPC 601 75 MHz | yes | 8-512 | 832×624×8 | 7.5.2-9.1 | 701 (before 7.5.3) |
| `power-macintosh-7200` | Power Macintosh 7200 | `9630C68B` | PowerPC 601 90 MHz | yes | 8-512 | 832×624×8 | 7.5.2-9.1 | 701 (before 7.5.3) |
| `power-macintosh-7500` | Power Macintosh 7500 | `9630C68B` | PowerPC 601 100 MHz | yes | 16-1024 | 832×624×16 | 7.5.2-9.1 | 701 (before 7.5.3) |
| `power-macintosh-8500` | Power Macintosh 8500 | `9630C68B` | PowerPC 604 120 MHz | yes | 16-1024 | 832×624×16 | 7.5.2-9.1 | 701 (before 7.5.3) |
| `power-macintosh-9500` | Power Macintosh 9500 | `9630C68B` | PowerPC 604 120 MHz | yes | 16-1536 | 1152×870×8 | 7.5.2-9.1 | 701 (before 7.5.3) |
| `powerbook-duo-2300c` | PowerBook Duo 2300c | `83C54F75` | PowerPC 603e 100 MHz | yes | 20-56 | 640×480×8 | 7.5.2-9.1 | PowerBook 5300/2300/190 Enabler (before 7.6) |
| `power-macintosh-6400` | Power Macintosh 6400 | `6F5724C0` | PowerPC 603e 180 MHz | yes | 16-136 | 640×480×16 | 7.5.3-9.1 | 410 (before 7.5.5) |
| `power-macintosh-5400` | Power Macintosh 5400 | `6F5724C0` | PowerPC 603e 120 MHz | yes | 16-136 | 640×480×16 | 7.5.3-9.1 | 410 (before 7.5.5) |
| `powerbook-1400` | PowerBook 1400c | `83A21950` | PowerPC 603e 133 MHz | yes | 16-64 | 800×600×16 | 7.5.3-9.1 | — |
| `powerbook-1400cs` | PowerBook 1400cs | `83A21950` | PowerPC 603e 117 MHz | yes | 12-64 | 800×600×16 | 7.5.3-9.1 | — |
| `power-macintosh-7300` | Power Macintosh 7300 | `960E4BE9` | PowerPC 604e 168 MHz | yes | 32-1024 | 832×624×8 | 7.5.5-9.1 (not 7.6) | 702 (before 7.6.1) |
| `power-macintosh-7600` | Power Macintosh 7600 | `960E4BE9` | PowerPC 604 132 MHz | yes | 16-1024 | 832×624×16 | 7.5.3-9.1 | — |
| `power-macintosh-8600` | Power Macintosh 8600 | `960E4BE9` | PowerPC 604e 200 MHz | yes | 32-1024 | 832×624×16 | 7.5.5-9.1 (not 7.6) | 702 (before 7.6.1) |
| `power-macintosh-9600` | Power Macintosh 9600 | `960E4BE9` | PowerPC 604e 200 MHz | yes | 32-1536 | 1152×870×8 | 7.5.5-9.1 (not 7.6) | 702 (before 7.6.1) |
| `power-macintosh-6500` | Power Macintosh 6500 | `6E92FE08` | PowerPC 603e 250 MHz | yes | 32-128 | 800×600×16 | 7.5.5-9.1 (not 7.6) | 411 (before 7.6.1) |
| `power-macintosh-5500` | Power Macintosh 5500 | `6E92FE08` | PowerPC 603e 225 MHz | yes | 32-128 | 640×480×16 | 7.5.5-9.1 (not 7.6) | 411 (before 7.6.1) |
| `twentieth-anniversary-macintosh` | Twentieth Anniversary Macintosh | `6E92FE08` | PowerPC 603e 250 MHz | yes | 32-128 | 800×600×16 | 7.6.1-9.1 | — |
| `power-macintosh-8600-300` | Power Macintosh 8600/300 | `960FC647` | PowerPC 604e 300 MHz | yes | 32-1024 | 832×624×16 | 7.6.1-9.1 | — |
| `power-macintosh-9600-350` | Power Macintosh 9600/350 | `960FC647` | PowerPC 604e 350 MHz | yes | 64-1536 | 1152×870×8 | 7.6.1-9.1 | — |
| `power-macintosh-g3-v1` | Power Macintosh G3 (first ROM) | `79D68D63` | PowerPC 750 233 MHz | yes | 32-768 | 1024×768×16 | 8.0-9.2.2, Mac OS X 10.2 | 770 (for 8.0) |
| `power-macintosh-g3` | Power Macintosh G3 | `78F57389` | PowerPC 750 266 MHz | yes | 32-768 | 1024×768×16 | 8.0-9.2.2, Mac OS X 10.2 | 770 (for 8.0) |
| `power-macintosh-g3-minitower` | Power Macintosh G3 Minitower | `78F57389` | PowerPC 750 266 MHz | yes | 32-768 | 1024×768×16 | 8.0-9.2.2, Mac OS X 10.2 | 770 (for 8.0) |
| `power-macintosh-g3-all-in-one` | Power Macintosh G3 All-in-One | `78F57389` | PowerPC 750 233 MHz | yes | 32-384 | 800×600×16 | 8.1-9.2.2, Mac OS X 10.2 | — |
| `powerbook-g3-wallstreet` | PowerBook G3 Series | `CBB01212` | PowerPC 750 233 MHz | yes | 32-512 | 1024×768×16 | 8.1-9.2.2, Mac OS X 10.2 | — |
| `powerbook-g3-pdq` | PowerBook G3 Series (PDQ) | `B46FFB63` | PowerPC 750 266 MHz | yes | 32-512 | 1024×768×16 | 8.1-9.2.2, Mac OS X 10.2 | — |
| `pippin` | Bandai Pippin | `2BEF21B7` | PowerPC 603 66 MHz | yes | 6-21 | 640×480×16 | the disc's | — |
| `pippin-dev` | Bandai Pippin (development ROM) | `2BF65931` | PowerPC 603 66 MHz | yes | 6-21 | 640×480×16 | the disc's | — |

#### The Built-in machine

`model: builtin`, the default, is Livadix's own machine: Macintosh 128K
hardware running Livadix's own ROM, embedded in the program. It needs no ROM,
disk or configuration; its desktop holds the README and changelog of the
Livadix that built it. It ignores memory and screen settings.

#### Compatibility

Livadix reads the startup disk (the CD with `boot_priority: cdrom`, else a
floppy, else the first hard disk with a System Folder) and warns, without
stopping, when the model does not run its System: the version is outside
the model's range, a System from 7.1 to 7.5.x lacks the model's enabler,
there is too little memory (System 7.5 4 MB, Mac OS 8 12 MB, 8.5 16 MB,
Mac OS 9 32 MB), or more than 8 MiB on a 24-bit setup without MODE32.
Systems before 6.0 and Mac OS Extended volumes are not checked.
`--inspect`, the start-up message and the configuration window all show
the result:

```
Startup disk: disk.img; HFS volume "Macintosh HD", System 7.1.1, Finder 7.1.3, enablers 001, 040
Compatibility warning: System 7.1.1 on a Macintosh Quadra 605 needs System Enabler 065, not found on the disk
```

### The configuration window

The Windows key (Super on Linux) opens the configuration window over the
Macintosh's screen, which pauses; the key again, Escape or the close box
goes back. When a profile cannot start (a missing ROM or image), Livadix opens
the window and says why. The window edits `livadix.yaml` in place: comments,
order and the other profiles stay as they were.

- **Profiles:** New, Duplicate, Rename, Delete and Make Default.
- **Machine:** the model (filtered by year, architecture, CPU and type; only
  models whose ROM is present; a filter that leaves out the chosen model
  selects the first one it lists), with the systems it runs; memory, clock,
  the controller, the startup disk's compatibility, and under Emulation the
  chime, translation, FPU, 68k processor, PMMU and what closing does.
- **Video**, **Storage** (disks with their SCSI ID or IDE bus, floppies,
  CD, boot order; image files dropped on the window are added), **Sound**,
  **Input**, **Ports**, **Network** and **Sharing**.

Only what the model can use is offered; choosing a model resets the
settings that depend on it. The buttons:

- **Stop** shuts the running Macintosh down, as closing Livadix's window
  does, and leaves it off: the window shows the Livadix logo, and a click or
  the Windows key opens the configuration window again.
- **Start** starts the selected profile as edited, saved or not; for the
  running profile it reads **Restart**: the Mac OS shuts down, as for Stop,
  and the Macintosh starts again with the edits.
- **Close** (the close box, Escape, the Windows key) closes the window and
  keeps the edits; a running Macintosh gets the mouse again. Changes that are not saved are warned about first.
  The running profile's changes that need no new start apply then: looks,
  sound, keys, and floppy and CD images, which go in and out as on a real
  Macintosh (a CD the Mac OS is using stays in until the Mac ejects it, by
  dragging its icon to the Trash, as a real drive ignores its eject button
  then; a floppy comes out at once, as with a paper clip). The others wait
  for Restart.
- **Save** (Ctrl+S) writes the file; when the running Macintosh needs a
  restart for what changed, it offers to restart it now.
- **Quit** quits Livadix, shutting a running Macintosh down first.

Livadix's own file picker is used on both Windows and Linux.
The window works from the keyboard: Tab, Space, the arrows and Escape. Its
layout needs 820 x 600 pixels; on a smaller window or display it is shown
scaled down.

### Machines

#### SCSI IDs

The CD-ROM drive is SCSI ID 3; hard disks take the IDs 0, 1, 2, 4, 5 and 6
in order, or the `scsi_id` given. Images beyond six become further
partitions of the last disk. The Macintosh starts from the disk its ROM
chooses unless the Startup Disk control panel (or `boot_priority`) names
one.

The 630 and 580 families, the PowerBook 150, 190, 1400, Duo 2300c and G3
Series, the Power Macintosh 5200 to 5500 and 6200 to 6500, the Twentieth
Anniversary Macintosh and the Beige G3s have an internal IDE (ATA) disk: `bus: ata` puts a disk there. The PowerBook 190, 1400 and G3
Series take a floppy or a CD-ROM module in their expansion bay, chosen at
start: `storage.cdrom: {image: ..., bus: ata}` fits the CD module.

```yaml
model: quadra-630
storage:
  disks:
    - {image: disks/mac-os-8.img, bus: ata}
    - {image: work.img, scsi_id: 5}
```

#### Resolutions

| `video.resolution` | The display it stands for | Deepest `video.depth` |
|---|---|---|
| `512x342` | The 9" built-in screen of the compact Macs (Plus, SE, Classic) | 32 |
| `512x384` | Apple's 12" RGB display, the Color Classic's built-in screen | 32 |
| `640x400` | A PowerBook's built-in screen (140, 170, Duo 210) | 32 |
| `640x480` | Apple's 13" and 14" displays, the most common Mac monitor | 32 |
| `800x600` | A VGA (SVGA) multisync monitor | 32 |
| `832x624` | Apple's 16" color display | 32 |
| `1024x768` | Apple's 19" display, a 17" multisync monitor | 16 |
| `1152x864` | A 20" or 21" multisync (VESA) monitor | 16 |
| `1152x870` | Apple's 21" display (and the Two-Page Monochrome) | 16 |
| `1280x1024` | A large multisync monitor | 8 |
| `1600x1200` | A large multisync monitor | 8 |
| `1920x1080` | A modern widescreen display (no period Mac had one) | 8 |

The Livadix Display Card, a NuBus or PCI card of Livadix's own with 2 MiB of video
memory, shows every mode above; a model's built-in video shows the modes
its hardware had, up to its own depth, and `video.card: auto` hands any
other mode to the card. On a model without slots only the built-in screen
is available. The ROM sets up a new PRAM at the first start, so a
configured depth or resolution of the built-in video applies from the
second start. On the card, the Monitors control panel switches depth and
(from Mac OS 8) resolution at once; its gamma list offers Livadix Linear and
Mac Standard (1.8) on sRGB.

#### Display

`video.monitor` offers what the real display could be:

| Model | `monitor` |
|---|---|
| Built-in monochrome screen (compact Macs, SE/30, Classic II) | `mono` |
| Grayscale PowerBooks | `lcd-gray` |
| Built-in color screens and color PowerBooks | `color` |
| Modular Macs | `color`; `mono` at 640x480, 640x870 or 1152x870 up to 8 bits (Apple's monochrome displays) |

The `crt` and `lcd` looks need OpenGL 3.3; without it Livadix shows the plain
picture. `sharp` keeps every pixel the same size at any window size,
blending only its edges; `nearest` repeats some rows; `linear` blurs. With
`vsync`, each frame is shown at the display's refresh.

Speed: the window keeps the Macintosh at its clock in real time, catching
up delays of up to 50 ms; `--unthrottled` runs it as fast as the host can.

#### Mac OS X 10.2 on the G3

All six G3 models (the Beige G3s and the PowerBook G3 Series) run Mac OS X
10.2 Jaguar, with at least 128 MiB (256 recommended), an ATA disk and the
ATAPI CD. A 6 GB disk image keeps the system inside the first 8 GB the
firmware can start from. Start the Install Disc, initialize the disk as
Mac OS Extended in Disk Utility, **restart** before choosing it as the
destination, swap to Install Disc 2 when asked, then remove the CD and set
`boot_priority: disk`. Select the startup volume with Apple's Startup Disk
on each machine: the firmware settings it writes belong to one ROM, and
Livadix keeps them per model and per disk. Other Old World Macs (XPostFacto)
and New World Macs are not supported.

#### The Pippin

The Bandai Pippin (`pippin`, or `pippin-dev` with the development ROM)
starts from the disc in `storage.cdrom`; hard disks and floppies are
refused. The retail ROM starts only discs signed by Apple, as the console
did; the development ROM starts any Pippin disc. Memory: 6 MiB (default),
5, or 7, 9, 13 and 21 MiB with a memory expansion; some titles (Gadget,
Internet Kit 2.0) need 9.

| Applejack controller | Host key | Game controller |
|---|---|---|
| Trackball | Mouse | Left stick |
| Left Fire | Mouse button | Left shoulder |
| Right Fire | F8 | Right shoulder |
| Directional pad | Arrow keys | Directional pad |
| Green, red, blue, yellow | F1-F4 | A, B, X, Y |
| Square, circle, diamond | F5-F7 | Back, Guide, Start |

Saved games live in the console's flash memory, kept with the profile's
PRAM when the console shuts down: close the window or choose Shut Down.

#### CD audio

A `.cue` sheet's audio tracks play as on a real drive (AppleCD Audio Player,
CD AutoPlay, the Control Strip), at the System's CD volume: play, pause,
stop, next and previous track, fast forward and rewind.

### Command line

| Option | Effect |
|---|---|
| `--config FILE` | Use this configuration instead of `livadix.yaml` |
| `--profile NAME` | Start this profile |
| `--list-profiles`, `--list-models`, `--list-roms` | List the file's profiles, the models, the known ROMs |
| `--inspect` | Check the configuration, ROM and images (opened read-only) and exit |
| `--hidden` | Run without a window |
| `--frames N`, `--screenshot FILE.bmp` | Stop after N frames; save the last one |
| `--unthrottled` | Run as fast as the host can |
| `--wav FILE.wav` | Record the sound to a file instead of playing it |
| `--close-after N[,N...]` | Close the window after N frames (again after the next count) |
| `--key CODE@N` | Send ADB key CODE (hex, +80 for a release) after N frames |
| `--script FILE` | Drive the configuration window from a script, for automated checks |
| `--version`, `--help` | |

Over Remote Desktop, Classic Mac OS, the Built-in machine and Mac OS X on
the G3s follow the host pointer directly; release the mouse (Ctrl+Alt)
before using the connection bar. The keyboard is the Apple Extended
Keyboard by position; Alt+Enter, Alt+F4 and the Ctrl+Alt keys stay with
Livadix.

Environment variables: `LIVADIX_NET_LOG` prints network events;
`LIVADIX_STATS=FILE` writes the emulation speed every second.

#### Machine controls

| Keys | Control |
|---|---|
| Ctrl+Alt+K | The Quadra 900/950's keyswitch: On, Secure, Off |
| Ctrl+Alt+F1, F2 | Brightness down, up (PowerBooks, LC 575 family) |
| Ctrl+Alt+F3, F4 | Contrast down, up (PowerBooks) |
| Ctrl+Alt+F5, F6 | Volume down, up (LC 575 family) |
| Ctrl+Alt+F7 | The programmer's switch's interrupt button (MacsBug, or the ROM's own monitor) |
| Ctrl+Alt+F8 | The programmer's switch's reset button (128K to Classic, Portable, PowerBook 100) |

## ROMs

Livadix includes no Apple ROM: each model needs a dump of its own ROM, which
you supply. Only the Built-in machine runs without one; its ROM is Livadix's
own and is embedded in the program.

Put ROM files in a `roms` folder beside the program or beside the
configuration file. Names do not matter: Livadix identifies every file by its
content (a Macintosh ROM by its header checksum and size, the other files
by size and CRC32) and checks a ROM's checksum before running it. The names
below are suggestions; when two files hold the same image, the one under
its suggested name is used. `livadix --inspect` lists what each file was
recognised as and which one a profile uses; `livadix --list-roms` lists every
image Livadix knows. `emulation.rom` names a file anywhere instead.

The checksum is the ROM's own header word, the sum of its 16-bit words.
For the 4 MiB Old World PowerPC ROMs it covers the first 3 MiB only; the
CRC32 and SHA-1 below cover every byte, so use them to check a dump.

### Macintosh ROMs

The 68000 Macs' ROMs and the Macintosh II family's (`mac-ii-*`, `mac-iix`)
address 24 bits: these machines run 32-bit only with MODE32 (see
[Configuration](#emulation), `emulation.pmmu`). The
PowerPC ROMs run 68k code in their own 68LC040 emulator.

| File | Size | Checksum | Models | CRC32 | SHA-1 |
|---|---|---|---|---|---|
| `mac-128k.rom` | 64 KiB | `28BA61CE` | Macintosh 128K | `6D0C8A28` | `9d86c883aa09f7ef5f086d9e32330ef85f1bc93b` |
| `mac-512k.rom` | 64 KiB | `28BA4E50` | Macintosh 512K | `CF759E0D` | `5b1ced181b74cecd3834c49c2a4aa1d7ffe944d7` |
| `mac-plus-v1.rom` | 128 KiB | `4D1EEEE1` | Macintosh Plus (first ROM) | `4FA5B399` | `e0da7165b92dee90d8b1522429c033729fa73fd2` |
| `mac-plus-v2.rom` | 128 KiB | `4D1EEAE1` | Macintosh Plus (second ROM) | `7CACD18F` | `73bf2eb215646e108daa0cdd874e6c843c8ce421` |
| `mac-plus.rom` | 128 KiB | `4D1F8172` | Macintosh Plus, 512Ke | `B2102E8E` | `7d2f808a045aa3a1b242764f0e2c7d13e288bf1f` |
| `mac-ii-v1.rom` | 256 KiB | `97851DB6` | Macintosh II (800K drive) | `8C8B9D03` | `5c264fe976f1e8495d364947c932a5e8309b4300` |
| `mac-se.rom` | 256 KiB | `B2E362A8` | Macintosh SE | `0F7FF80C` | `58532b7d0d49659fd5228ac334a1b094f0241968` |
| `mac-ii-v2.rom` | 256 KiB | `9779D2C4` | Macintosh II (800K drive) | `4DF6D054` | `db6b504744281369794e26ba71a6e385cf6227fa` |
| `mac-iix.rom` | 256 KiB | `97221136` | II FDHD, IIx, IIcx, SE/30 | `CE3B966F` | `753b94351d94c369616c2c87b19d568dc5e2764e` |
| `mac-se-fdhd.rom` | 256 KiB | `B306E171` | Macintosh SE FDHD | `F530CB10` | `d3670a90273d12e53d86d1228c068cb660b8c9d1` |
| `iici.rom` | 512 KiB | `368CADFE` | IIci | `46ADBF74` | `b54f9d2ed16b63c49ed55adbe4685ebe73eb6e80` |
| `mac-portable.rom` | 256 KiB | `96CA3846` | Macintosh Portable | `497348F8` | `79b468b33fc53f11e87e2e4b195aac981bf0c0a6` |
| `iifx.rom` | 512 KiB | `4147DD77` | IIfx | `EF441BBD` | `9fba3d4f672a630745d65788b1d1119afa2c6728` |
| `lc.rom` | 512 KiB | `350EACF0` | LC | `71681726` | `6bef5853ae736f3f06c2b4e79772f65910c3b7d4` |
| `iisi.rom` | 512 KiB | `36B7FB6C` | IIsi | `F304D973` | `f923de4125aae810796527ff6e25364cf1d54eec` |
| `mac-classic.rom` | 512 KiB | `A49F9914` | Macintosh Classic (the SE's ROM with a System 6.0.3 ROM disk) | `510D7D38` | `ccd10904ddc0fb6a1d216b2e9effd5ec6cf5a83d` |
| `classic-ii.rom` | 512 KiB | `3193670E` | Classic II | `96D2E1FD` | `50df69c1b6e805e12a405dc610bc2a1471b2eac2` |
| `quadra-700.rom` | 1 MiB | `420DBFF3` | Quadra 700, 900, PowerBook 140, 145, 145B, 170 | `88EA2081` | `7a8ee468d16e64f2ad10cb8d1a45e6f07cc9e212` |
| `powerbook-100.rom` | 256 KiB | `96645F9C` | PowerBook 100 | `29AC7EE9` | `7f3acf40b1f63612de2314a2e9fcfeafca0711fc` |
| `lc-ii.rom` | 512 KiB | `35C28F5F` | LC II | `A92145B3` | `d5786182b62a8ffeeb9fd3f80b5511dba70318a0` |
| `quadra-950.rom` | 1 MiB | `3DC27823` | Quadra 950 | `0E11206E` | `d61dba4a2d2cf9048244b713eaa294100063658d` |
| `iivx.rom` | 1 MiB | `4957EB49` | IIvx, IIvi, Performa 600 | `61BE06E5` | `560ce203d65178657ad09d03f532f86fa512bb40` |
| `powerbook-160.rom` | 1 MiB | `E33B2724` | PowerBook 160, 165, 165c, 180, 180c | `536C60F4` | `c0510682ae6d973652d7e17f3c3b27629c47afac` |
| `powerbook-duo-210.rom` | 1 MiB | `ECFA989B` | PowerBook Duo 210, 230, 250 | `B86ED854` | `ed1371c97117a5884da4a6605ecfc5abed48ae5a` |
| `lc-iii-v1.rom` | 1 MiB | `EC904829` | LC III (older) | `D326EB69` | `4ac4b266ed78f7f3876448b93b5cc2ff7617d1a2` |
| `lc-iii.rom` | 1 MiB | `ECBBC41C` | LC III, LC III+ | `E578F5F3` | `c77df3220c861f37a2c553b6ee9241b202dfdffc` |
| `color-classic.rom` | 1 MiB | `ECD99DC0` | Color Classic | `C84C3AA5` | `fd9e852e2d77fe17287ba678709b9334d4d74f1e` |
| `centris-610.rom` | 1 MiB | `F1A6F343` | Quadra/Centris 610, 650 | `3318A935` | `031f13bfd726a70cfe4c73c5967861bc77297a79` |
| `quadra-800.rom` | 1 MiB | `F1ACAD13` | Quadra/Centris 610, 650, Quadra 800 | `4E70E3C0` | `f2a9ce387019bf272c6e3459d961b30f28942ac5` |
| `powerbook-duo-270c.rom` | 1 MiB | `0024D346` | PowerBook Duo 270c | `94C4D04A` | `be7bd9637203e4513b896146ddfc85c37817d131` |
| `color-classic-ii.rom` | 1 MiB | `EDE66CBD` | Color Classic II, LC 520, LC 550, Performa 275, 550, 560 | `A893CB0F` | `c54ee2f45020a4adeb7451adce04cd6e5fb69790` |
| `macintosh-tv.rom` | 1 MiB | `EAF1678D` | Macintosh TV | `0644F05B` | `74975c60d3a560fac9ad63125bb65a750fceaede` |
| `quadra-605.rom` | 1 MiB | `FF7439EE` | LC 475, 575, Quadra 605, Performa 475..578 | `B8514689` | `1d833125adf553a50f5994746c2c01aa5a1dbbf2` |
| `powerbook-duo-280.rom` | 1 MiB | `015621D7` | PowerBook Duo 280, 280c | `568D28EB` | `d49dd69cf038784b8849793ad3c0e62c2d11f653` |
| `powerbook-520.rom` | 2 MiB | `B6909089` | PowerBook 520, 520c, 540, 540c | `66CF9F2F` | `8e724b8201926b880085c0e7ef296a3d6332ea3d` |
| `quadra-630.rom` | 1 MiB | `06684214` | LC 630, Quadra 630, Performa 630 | `1735E7A5` | `47cd505b6a7c46e5c0ffa29f0d5037c83e94a02f` |
| `powerbook-150.rom` | 1 MiB | `FDA22562` | PowerBook 150 | `7E283B7C` | `ef56fadc8adbe978f9d91fdf1543f66cb8468390` |
| `quadra-840av.rom` | 2 MiB | `5BF10FD1` | Quadra 660AV, 840AV | `6973886C` | `532d8cb9c928eb6f3c264561b17bee9a02aad9bb` |
| `performa-580.rom` | 1 MiB | `064DC91D` | LC 580, Performa 580, 588 | `59E6960F` | `f48a8adf06bce50beee033d0d814da0e5e916d08` |
| `powerbook-190.rom` | 2 MiB | `4D27039C` | PowerBook 190, 190cs | `9C71C823` | `ee11eab83347e75abb672e9233019a3d5ff1a33c` |
| `power-macintosh-6100.rom` | 4 MiB | `9FEB69B3` | Power Macintosh 6100, 7100, 8100, Workgroup Server 6150, 8150 | `A43FADBC` | `6fac1c4e920a077c077b03902fef9199d5e8f2c3` |
| `workgroup-server-9150-80.rom` | 4 MiB | `9C7C98F7` | Workgroup Server 9150/80 | `7D9CDD22` | `147b2bf015227d9c928bad11fb125cb0d16ca454` |
| `power-macintosh-7100-80.rom` | 4 MiB | `9B7A3AAD` | Power Macintosh 7100/80, 8100/100, 8100/110 | `22EF0260` | `ec58cdb4b7fe3282b2fb76bf9a39f1ec9c9635d7` |
| `power-macintosh-5200.rom` | 4 MiB | `63ABFD3F` | Power Macintosh and Performa 5200, 5300, 6200, 6300 | `2F47A6EA` | `0b34d7c692594695b39719c3bf21808985f89f2c` |
| `workgroup-server-9150-120.rom` | 4 MiB | `9B037F6F` | Workgroup Server 9150/120 | `46FB4FF3` | `8b80ac0cba631e38fc90f4edc7f2e43dabe5ca75` |
| `power-macintosh-7200-v1.rom` | 4 MiB | `96CD923D` | Power Macintosh 7200, 7500, 8500, 9500 (first ROM) | `A39FB2B9` | `6ddeabaa9faca21901d6448a3b8d23004219df35` |
| `power-macintosh-7200.rom` | 4 MiB | `9630C68B` | Power Macintosh 7200, 7500, 8500, 9500 | `FDAD904D` | `3a436af3f06680d270d1325ed7787a7bb91d978f` |
| `powerbook-duo-2300c.rom` | 4 MiB | `83C54F75` | PowerBook Duo 2300c | `24949DF7` | `42df30b601e00efd00f133f2f1f35ded64c4ab67` |
| `pippin-dev.rom` | 4 MiB | `2BF65931` | Bandai Pippin (development ROM) | `4FF875E6` | `eb8739cab1807c6c7c51acc7f4a3afc1f9c6ddbb` |
| `pippin.rom` | 4 MiB | `2BEF21B7` | Bandai Pippin | `29F06528` | `6478ee9e8d5cfe3b00f699f7072482210e00c5cc` |
| `power-macintosh-6400.rom` | 4 MiB | `6F5724C0` | Power Macintosh and Performa 5400, 6400 | `EC9914BE` | `822ab19b360b8fa25238e531dcb85a4459f7c8be` |
| `powerbook-1400.rom` | 4 MiB | `83A21950` | PowerBook 1400c, 1400cs | `E9B2BE0F` | `c4a51b69f18f416f092aa9a45c23778a20f03872` |
| `power-macintosh-7300.rom` | 4 MiB | `960E4BE9` | Power Macintosh 7300, 7600, 8600, 9600 | `7910CDF9` | `135532da4beaf1f9cdae202ec146660d9bcb0eba` |
| `power-macintosh-6500.rom` | 4 MiB | `6E92FE08` | Power Macintosh 5500, 6500, Twentieth Anniversary Macintosh | `084646F4` | `9c5fe05473650be61e582e42bc03ba4be6cf1072` |
| `power-macintosh-8600-300.rom` | 4 MiB | `960FC647` | Power Macintosh 8600, 9600 (later ROM) | `14D126F4` | `bd836eaafb1e839f25e07bfda45550b7d527c879` |
| `power-macintosh-g3-v1.rom` | 4 MiB | `79D68D63` | Power Macintosh G3 (first ROM) | `74A3BADF` | `e7fc183f62addc6499350c727252d3348184955e` |
| `power-macintosh-g3.rom` | 4 MiB | `78F57389` | Power Macintosh G3, G3 Minitower, G3 All-in-One | `A7A4DBD3` | `839f23def321006577eeb48d3dbdf0614d2bf31a` |
| `powerbook-g3-wallstreet.rom` | 4 MiB | `CBB01212` | PowerBook G3 Series | `58895478` | `9049344d94d6ceff0215f13e74994355c8656e8a` |
| `powerbook-g3-pdq.rom` | 4 MiB | `B46FFB63` | PowerBook G3 Series (PDQ) | `2892AAC8` | `f7f737659d75705ddc06bbf59bfd0690dfbea96c` |

### Controller firmware and card ROMs

Optional. The ADB, clock and power controllers run at a protocol level
unless their firmware is here (`emulation.controller`). The SE/30 needs
`video-se30.bin` and does not start without it. The Ethernet card ROMs fit
the cards of `network.card`; without them the card is not fitted. The two
Macintosh II video card ROMs are recognised only: Livadix's own display card
stands in for them.

| File | Part | Machines | CRC32 | SHA-1 |
|---|---|---|---|---|
| `egret-341s0850.bin` | Egret 341S0850 | LC, LC II | `4906ECD0` | `95e08ba0c5d4b242f115f104aba9905dbd3fd87c` |
| `egret-341s0851.bin` | Egret 341S0851 | Classic II, IIvx, IIvi, Performa 600, LC III family | `EA9EA6E4` | `8b0dae3ec66cdddbf71567365d2c462688aeb571` |
| `egret-344s0100.bin` | Egret 344S0100 | IIsi | `59E2B6B6` | `540e752b7da521f1bdb16e0ad7c5f46ddc92d4e9` |
| `cuda-341s0060.bin` | Cuda 341S0060 | LC 520 family, Quadra 630, LC 580 | `0F5E7B4A` | `972b3778146d9787b18c3a9874d505cf606b3e15` |
| `cuda-341s0417.bin` | Cuda 341S0417 | Color Classic | `571F24C9` | `a2ae12492389a00e5f4b1ef19b267d6f3a8eadc3` |
| `cuda-341s0788.bin` | Cuda 341S0788 | LC 475/575, Quadra 605, 660AV, 840AV | `DF6E1B43` | `ec23cc6214c472d61b98964928c40589517a3172` |
| `cuda-341s0789.bin` | Cuda 341S0789 | Macintosh TV | `682D2ACE` | `81a9e25204f58363ed2f5945763ac19a1a66234e` |
| `pmu-pb140.bin` | PMU (M50753) | PowerBook 140, 145, 170 | `1A32B5E5` | `7c096324763cfc8d2024893b3e8493b7729b3a92` |
| `pmu-pb160.bin` | PMU (M50753) | PowerBook 160, 165, 165c, 180, 180c | `F2DF696C` | `fc312cbfd407c6f0248c6463910e41ad6b5b0daa` |
| `pmu-portable.bin` | PMU (M50753) | Macintosh Portable, PowerBook 100 | `01DAE148` | `29d2fca7426c31f2b9334832ed3d257974a61bb1` |
| `pmu-pge-boot.bin` | PG&E (68HC05) boot ROM | PowerBook Duo 210, 230, 250 | `62D4DFED` | `79dc721651bf47aec53f57885779c84c4781761d` |
| `video-pb180c.bin` | Video declaration ROM | PowerBook 165c, 180c | `810C75AD` | `3a936e97dee5ceeb25e50197ef504e514ae689a4` |
| `video-se30.bin` | Built-in video declaration ROM | SE/30 | `B74C3463` | `584201cc67d9452b2488f7aaaf91619ed8ce8f03` |
| `card-mac-ii-video.bin` | Macintosh II Video Card 342-0008-A | Mac II family (optional card) | `BF50850D` | `abe85d8a882bb2b8187a28bd6707fc2f5d77eedd` |
| `card-display-8-24.bin` | Display Card 8•24 341-0868 | Mac II family (optional card) | `57F925FA` | `4d3c0632711b7b31c8e0c5cfdd7ec1904f178336` |
| `card-ethernet-nb.bin` | EtherTalk NB Card 670-0210 (Apple/3Com) | NuBus models without built-in Ethernet (optional card) | `E3AE8C26` | `01ddc15ee84b17128203cb812f29bac6b20fd642` |
| `card-ethernet-lc.bin` | Ethernet LC Card 341-0842 | LC-PDS models (optional card) | `92310CB3` | `ba4fd87765104701d5e6bf7962ed3a453e3c5711` |

### New World ROM files

The "Mac OS ROM" files of the iMac and later are recognised and listed,
but no model runs them.

| File | Version | CRC32 | SHA-1 |
|---|---|---|---|
| `mac-os-rom-1.1.rom` | 1.1 | `B5DF73DD` | `10ff26c0971d678433d0d3bf7825673da79973c2` |
| `mac-os-rom-1.1.2.rom` | 1.1.2 | `2172203C` | `7639af0e2c87b212846014ed3eb166b51e21e3cf` |
| `mac-os-rom-1.2.rom` | 1.2 | `CEACE951` | `a5bbb1c3ad49738a3e2020dc083bbdb32b877997` |
| `mac-os-rom-1.2.1.rom` | 1.2.1 | `497F0241` | `d544279040f428358732c9b058e03e205a521508` |
| `mac-os-rom-1.4.rom` | 1.4 | `FCCE2265` | `5f1519bea6fc4122604363149e9f9c7214886d99` |
| `mac-os-rom-1.6.rom` | 1.6 | `2F6572B4` | `a63053efcbf8c569f82ede6ec69a551c10b7e899` |
| `mac-os-rom-1.8.1.rom` | 1.8.1 | `5148B324` | `d84d89fb0da3e080808f60320f483fc5eff6922c` |
| `mac-os-rom-2.5.1.rom` | 2.5.1 | `FB6909A4` | `fc6cff78aab9bfb0766b13380d2a33ed14c3bbdc` |
| `mac-os-rom-3.0.rom` | 3.0 | `045CF212` | `a5884051dafba5e89317749aaf0c555bca85d3ed` |
| `mac-os-rom-3.7.rom` | 3.7 | `10251DD3` | `915670b6e65582f6ea8ac6c5ab3cbd427e4fc615` |
| `mac-os-rom-3.8.rom` | 3.8 | `65A0A6E9` | `3f6fc8148e641cd1985b8404c8452240d8556380` |
| `mac-os-rom-6.1.rom` | 6.1 | `8DA310EC` | `f367ba068191adddbcc151fdb82de955aed41825` |
| `mac-os-rom-7.5.1.rom` | 7.5.1 | `B84BB1EE` | `68917b25adb83828314c869cffb0d55085ab3d80` |
| `mac-os-rom-8.4.rom` | 8.4 | `D75E79CF` | `27539c4d64bb3bc8a4b2f89d3dd37d91c926a213` |
| `mac-os-rom-9.0.1.rom` | 9.0.1 | `4F3A69C5` | `4574600512b9560051b850629ac50703db9fee2e` |
| `mac-os-rom-9.1.1.rom` | 9.1.1 | `16C11580` | `5b116cade270146107903e03aa88d181d2c67000` |
| `mac-os-rom-9.6.1.rom` | 9.6.1 | `CC3C0AEC` | `bff16fe51a43b4361815fa3bb4e90270b47dacba` |
| `mac-os-rom-9.8.1.rom` | 9.8.1 | `0C50D766` | `971a99111cf9421384552a5bc0357a7609c47fcc` |
| `mac-os-rom-10.2.1.rom` | 10.2.1 | `5B788642` | `b166172b30a2766367fc71f5739c32baa987b214` |

## Emulated hardware

Livadix emulates each Macintosh at the level of its chips: the board's
processor, memory controller, I/O chips and video sit at their real
addresses, and Apple's ROM and system software run on them unmodified.
What is left out is listed in [Limitations](#limitations).

### Processors

| Processor | Models | Notes |
|---|---|---|
| 68000, 68HC000 | 128K to Classic, Portable, PowerBook 100 | Interpreted, with the 68000's cycle timing |
| 68020 | Macintosh II, LC | 68881 FPU; the Macintosh II's AMU or an optional 68851 PMMU |
| 68030 | IIx, IIcx, SE/30, IIci, IIsi, IIfx, IIvx/IIvi, LC II, LC III, Classic II, Color Classics, LC 520/550, TV, PowerBook 140-180c and 150, Duo 210-270c | Built-in MMU; 68882 FPU where fitted |
| 68040, 68LC040 | Quadras, Centrises, LC 475 to 630, PowerBook 520/540/190, Duo 280 | MMU, caches; the 68040's FPU subset, the rest left to the system's emulation as on the real chip |
| PowerPC 601 | 6100, 7100, 8100, 7200, 7500, Workgroup Servers | POWER instructions included |
| PowerPC 603, 603e | 5200-5500, 6200-6500, TAM, 1400, Duo 2300c, Pippin | Software TLB refill |
| PowerPC 604, 604e | 8500, 9500, 7300 to 9600 | |
| PowerPC 750 (G3) | Beige G3s, PowerBook G3 Series | L2 cache control |

The 68020-68040 and the PowerPC code run through translators to x64 or
ARM64 code, with the same results and guest timing as the interpreters.
68k timing follows Motorola's instruction tables; PowerPC timing is a fixed
cost per instruction. On Power Macintoshes an optional 68k accelerator runs
Mac OS's 68k code on Livadix's own 68k core instead of Apple's emulator.

Microcontrollers run their own firmware when its dump is supplied, else a
model of their commands: the 68HC05 of Egret, Cuda and the PG&E, the
M50753 of the PowerBook Power Managers, the 65C02 of the IIfx's and Quadra
900/950's I/O processors, and the AV Quadras' DSP3210.

### Common chips

| Function | Chips |
|---|---|
| Timers and ports | VIA (R6522) and its ASIC variants |
| Serial ports | Zilog 8530 SCC, LocalTalk assist on the PCI models |
| SCSI | NCR 5380/53C80 (pseudo-DMA and the IIfx's DMA), 53C96, 53C94, MESH |
| IDE | ATA hard disks and ATAPI CD-ROM drives |
| Floppy | IWM, SWIM, SWIM2, SWIM III, New Age: 400K and 800K GCR, 720K and 1.44M MFM |
| Sound | ASC, Enhanced ASC and its variants, Singer, AWAC, AWACS, Screamer; sound input where the machine had it |
| Keyboard, mouse, clock, PRAM, power | ADB through the VIA, Egret, Cuda, PMU, PMU99, PG&E and Caboose; the compact Macs' keyboard and quadrature mouse; the RTC |
| Ethernet | SONIC, MACE (Am79C940), BMac; Apple's EtherTalk NB and Ethernet LC cards (DP8390) |
| Other | DBDMA, PCI bridges, Open Firmware NVRAM, USB (OHCI, Livadix's absolute pointer only), the Pippin's flash memory |

### Machine boards

| Family | Models | Board chips and video |
|---|---|---|
| Compact 68000 Macs | 128K, 512K, 512Ke, Plus, SE, SE FDHD, Classic | Their gate arrays, the screen and sound buffers, IWM or SWIM, 5380 (Plus on) |
| 68000 portables | Portable, PowerBook 100 | Gate arrays, static RAM, 640×400 LCD, PMU |
| Macintosh II | II, II FDHD, IIx, IIcx, SE/30 | GLUE, 5380, IWM/SWIM, ASC; the SE/30's built-in video |
| IIci, IIsi | IIci, IIsi | MDU, RBV video, Egret (IIsi) |
| IIfx | IIfx | OSS, IOPs, the SCSI DMA chip, FMC |
| LC family | LC, LC II, Classic II, Color Classic | V8, Eagle, Spice; Egret or Cuda |
| LC III and LC 520 families, IIvx | LC III/III+, Color Classic II, LC 520/550, Performa 275-600, TV, IIvx, IIvi | Sonora, VASP, V8 (TV); Egret or Cuda |
| Quadra 700, 900, 950 | Quadra 700, 900, 950 | MCU, DAFB video, 53C96 (two on the 900/950), SONIC, EASC; IOPs and Caboose on the 900/950 |
| Centris/Quadra 610-800 | Centris 610/650, Quadra 610/650/800 | DJMEMC, IOSB, DAFB-type video, SONIC |
| LC 475 family | LC 475/575, Quadra 605, Performa 475-578 | MEMCjr, PrimeTime, Cuda |
| 630 and 580 families | Quadra/LC/Performa 630, LC/Performa 580/588 | F108 (memory, IDE), PrimeTime II, Valkyrie video, Cuda |
| AV Quadras | Quadra 840AV, Centris 660AV | PSC DMA, Civic video, MACE, DSP3210, New Age floppy, Cuda |
| 68030 PowerBooks | PowerBook 140-180c, Duo 210-270c | Jaws, Niagara, MSC; GSC, CSC and the 180c's color video; PMU or PG&E |
| 68040 PowerBooks | Duo 280/280c, PowerBook 150, 520/540, 190 | MSC, Pratt, CSC/GSC, IDE (150, 190), PMU99 |
| PDM Power Macs | 6100, 7100, 8100, Workgroup Servers 6150-9150 | HMC, AMIC, BART (NuBus), Ariel II video, AWAC, 53C94, MACE, Cuda |
| Capella | 5200, 5300, 6200, 6300 | Capella bridge, F108, Valkyrie, PrimeTime II, Cuda |
| First PCI generation | 7200, 7300, 7500, 7600, 8500, 8600, 9500, 9600 | Bandit, Grand Central, MESH, 53C94, Hammerhead or Platinum, Control or Platinum video, AWACS, SWIM III, MACE, Cuda |
| PSX | 5400, 6400 | PSX, O'Hare, Valkyrie-AR, IDE, MESH, AWACS, Cuda |
| PSX+ | 5500, 6500, TAM | PSX+, O'Hare, ATI mach64 GT-B video, IDE, MESH, AWACS, Cuda |
| PBX PowerBooks | 1400c/cs, Duo 2300c | PBX, Whitney, Baboon IDE, ECSC video, Singer sound, PMU |
| Beige G3 | Desktop, Minitower, All-in-One | Grackle, Heathrow, IDE, MESH, Screamer, BMac, Cuda |
| PowerBook G3 Series | Wallstreet, PDQ | Two Heathrows, IDE, PMU; the screen is the Livadix Display Card |
| Pippin | Retail and development ROMs | Aspen, Grand Central, Taos video, flash memory, Applejack controller, Cuda |

### Livadix's own devices

- **Livadix Display Card:** a NuBus card (24- and 32-bit) and a PCI card with
  2 MiB of video memory, its own declaration ROM and driver: every
  resolution of the [configuration guide](#resolutions) at
  1 to 32 bits, and accelerated QuickDraw. It stands in where a model's own
  video cannot show a mode, and is the screen of the 9500/9600 and the G3s.
- **Disks:** a bare HFS or HFS+ image is presented with a partition map
  and Livadix's own SCSI or ATA driver, so it needs no preparation;
  partitioned images keep their own drivers. CD images with audio tracks
  play as on a real drive.
- **Built-in machine:** Macintosh 128K hardware running Livadix's own ROM,
  which needs no Apple file.
- **Network:** the Ethernet leads to a small private network whose router
  reaches the Internet through the host, and to an AppleShare file server
  that serves host folders.
- **Floppy drive sounds:** synthesized from the drives' motor and head
  movements.

## Limitations

What Livadix does not emulate, or emulates differently from the real machine,
as far as a user can notice it. Known bugs are reported separately.

### All machines

- **Speed.** 68k timing follows Motorola's tables; PowerPC timing is a flat
  cost per instruction, without the real processors' pipelines and caches,
  so a PowerPC model at its own MHz is not exactly as fast as the original.
- **Network.** The Ethernet carries TCP, UDP and ping to the Internet and
  AppleTalk to Livadix's own file server only: no IPv6, and no AppleTalk to
  other machines.
- **CD audio** is mixed after the Macintosh's own sound rather than through
  its sound chip, so the sound chip's input settings do not apply. Only
  `.cue` sheets carry audio tracks; CD+G, CD-Text and ISRCs are not read.
- **Floppies:** no 1680 KiB (DMF) images.
- **Mac OS X:** only 10.2 Jaguar on the six G3 models; no New World Macs
  (iMac and later), no XPostFacto, no AltiVec.
- **ARM64:** the Windows and Linux ARM64 programs have not yet been tried
  on ARM64 hardware.

### 68k Macintoshes

- **Macintosh II, IIx, IIcx, SE/30:** 32-bit addressing needs MODE32 on the
  startup disk (and `emulation.pmmu` on the Macintosh II). Apple's Macintosh
  II Video Card and Display Card 8•24 are not emulated; Livadix's own display
  card stands in, without its modes above 1024x768 at 8 bits in 24-bit mode.
- **630 and 580 families:** the CD-ROM drive is on SCSI; their ROM's ATA
  Manager drives only the internal disk, not an ATAPI CD.
- **LC 475/575 and Quadra 605 family:** the LC 575's power key needs a
  press longer than about 40 ms with Apple's Cuda firmware.
- **Color Classic, LC 520/550 families:** front-panel buttons and the
  brightness/contrast controls are not emulated.
- **Quadra 840AV and 660AV:** video input is not shown.
- **PowerBooks and Duos:** no Duo Dock or MiniDock, internal modem or panel
  gamma; the battery is always charged; panels are a look, not a
  calibration. The PowerBook 145/145B report the 140's Gestalt type under
  System 7.1, as their ROM has no record of their own. The PowerBook 190's
  bay module is chosen at start (no hot swap).
- **Upgrade options** (68040 upgrades in the processor socket, communication
  slot cards other than Apple's Ethernet cards) are not offered.

### Power Macintoshes

- **Speed:** Mac OS 9 on a 6100 reaches the Finder in about 10 host seconds
  on a performance core; Windows may run Livadix on efficiency cores, slower.
- **Performa 5200-6300:** no communication-slot Ethernet card (no ROM dump
  is known); use the Ethernet LC Card in the PDS slot.
- **PowerBook 1400 and Duo 2300c:** no dock, PC Card or modem; the 1400cs
  uses the 1400c's color panel.
- **Beige G3 and PowerBook G3 Series:** the screen is the Livadix Display Card,
  not the ATI chip, so software that drives the ATI chip does not find it
  and the PowerBook's brightness keys do nothing. The PowerBook G3 has one
  battery, no lid switch, PC Card slots, infrared or second bay. The G3's
  Mac OS 8.0 needs the G3 CD's System Enabler 770. Cuda runs at the
  protocol level (no matching firmware dump). USB offers only Livadix's own
  absolute pointer.
- **A System installed on a 68k Macintosh** (or in an emulator of one) lacks
  the Power Macintosh drivers: its built-in Ethernet, for one, needs the
  "Ethernet (Built-In)" extension, which only a PowerPC (or Universal)
  installation of Mac OS puts in the Extensions folder; without it TCP/IP
  and AppleTalk find no Ethernet.

### Bandai Pippin

- One Applejack controller. No floppy drive, docking station or modem.
- **Gundam Virtual Modeler Light** opens to a black screen unless a
  controller button is pressed while its start-up screen shows. The Pippin's
  CD-ROM driver discards the first event an application receives after
  start-up, and without a press that is the one that opens the program.
