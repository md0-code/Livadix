#!/usr/bin/env bash
# Build, test and package Livadix for Linux: linux-x64 (native) and linux-arm64 (cross-compiled).
#
#   tools/build_linux.sh [x64|arm64|all] [--test] [--train] [--no-package] [--no-pgo] [--clean] [--verify] [--oracle]
#                        [--no-shell] [--checks NAME,NAME] [--in-place] [--build-root DIR] [--sysroot DIR]
#   tools/build_linux.sh sysroot [--sysroot DIR]       (as root, once: the arm64 root for the cross build)
#
# Run it inside WSL Debian 12 (written and tried there) from the repository, which may be on a Windows drive
# (/mnt/c/...). A Windows drive is slow, so the sources are first copied to /tmp/livadix-build/<repository
# folder name>/repo (--build-root; --in-place builds in the repository itself instead), where the build tree, the
# test output (work/) and the smoke run are too. The git-ignored roms/ and data/ are not copied: the tests read
# them where they are (the repository's, or the main checkout's when the repository is a worktree under
# work/wt/; LIVADIX_ROMS and LIVADIX_DATA name other folders), and so does the SingleStepTests suite (work/cpu, 540 MB,
# copied once into the build copy). Only the archive goes to the repository's dist/releases/.
#
# What it does:
#   configure and build (Release, SDL2 built from source and linked statically, every warning an error), with --test
#   ctest on SDL's offscreen video driver and dummy audio driver and a headless start of the program (the Built-in
#   ROM, five frames, a screenshot), and the archive dist/releases/Livadix-<version>-linux-x64.tar.gz: what the Windows release holds
#   (tools/build_dist.ps1) but the program, which is `livadix`.
#
#   --verify     also runs tests/verify.py (the quick tier, or the checks --checks names) on this build; most of it
#                needs the ROMs
#   --oracle     also runs ppc-oracle*, which compare the PowerPC core with QEMU's qemu-ppc (/usr/local/bin/qemu-ppc);
#                qemu-ppc under WSL 2 ends silently on a program file that is on the ext4 file system
#   --no-pgo     builds the x64 archive without profile-guided optimisation (see below)
#   --no-shell   builds and tests only the program-less targets (the core, the chips, the network and the CPU tests),
#                without the program, the shell tests, the smoke run and the archive
#
# Profile-guided optimisation (the x64 archive, as tools/build_dist.ps1 for Windows): the program built with the
# profile kept in work/pgo/linux-x64 (none kept: without it). Only --train trains it again and keeps it: the program
# built instrumented (GCC's -fprofile-generate), trained on what is there of the Windows release's workloads, the Quadra 700 ROM's start
# (roms/quadra-700.rom) and the private G3 / Mac OS X fixture (work/pgo/osx.yaml with its osx-seed.img and nvram-seed/,
# copied under the build root, paths made Linux ones), then built again with the counts (-fprofile-use). Without
# either workload the archive is built without it.
#
# arm64: every target cross-compiled with aarch64-linux-gnu-gcc against an arm64 root (Debian 12's packages, which
# `sysroot` unpacks into /tmp/livadix-build/sysroot-arm64, or --sysroot DIR); nothing of it runs here, and its archive
# dist/releases/Livadix-<version>-linux-arm64.tar.gz takes the ROM catalogue from the x64 program (built first when missing).
# Packages: gcc-aarch64-linux-gnu, debootstrap.
#
# Packages needed (Debian, root): build-essential cmake ninja-build pkg-config python3 git rsync
# libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
# libxss-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl-dev libgles-dev libegl-dev libdbus-1-dev libudev-dev
# libwayland-dev libdecor-0-dev wayland-protocols libsndio-dev   (SDL2's own build dependencies; at run time SDL
# loads X11, Wayland, ALSA, PulseAudio and the GL libraries when present, so the program itself links only the C
# library).
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
version=$(head -n 1 "$root/src/VERSION" | tr -d '[:space:]')
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "src/VERSION must hold MAJOR.MINOR.PATCH, not '$version'" >&2; exit 1; }

test=0 train=0 package=1 pgo=1 clean=0 verify=0 oracle=0 shell=1 in_place=0 checks=
build_root="${LIVADIX_BUILD_ROOT:-/tmp/livadix-build/$(basename "$root")}"
sysroot="${LIVADIX_SYSROOT:-/tmp/livadix-build/sysroot-arm64}"
targets=()
while [[ $# -gt 0 ]]; do
    case $1 in
        x64|arm64|sysroot) targets+=("$1") ;;
        all) targets+=(x64 arm64) ;;
        --sysroot) sysroot=$2; shift ;;
        --test) test=1 ;;
        --no-test) test=0 ;;
        --train) train=1 ;;
        --no-package) package=0 ;;
        --no-pgo) pgo=0 ;;
        --clean) clean=1 ;;
        --verify) verify=1 ;;
        --oracle) oracle=1 ;;
        --no-shell) shell=0 ;;
        --in-place) in_place=1 ;;
        --checks) checks=${2//,/ }; verify=1; shift ;;
        --build-root) build_root=$2; shift ;;
        -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1 (tools/build_linux.sh --help)" >&2; exit 2 ;;
    esac
    shift
done
[[ ${#targets[@]} -gt 0 ]] || targets=(x64)

# The arm64 root: Debian 12's own packages, unpacked (not configured: nothing of arm64 runs), so the cross
# compiler finds the C library and the headers SDL's back ends need.
sysroot_packages=(libc6-dev linux-libc-dev libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev
    libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl-dev libgles-dev
    libegl-dev libdbus-1-dev libudev-dev libwayland-dev libdecor-0-dev wayland-protocols libsndio-dev)
if [[ " ${targets[*]} " == *" sysroot "* ]]; then
    [[ $EUID -eq 0 ]] || { echo "making the sysroot needs root: sudo tools/build_linux.sh sysroot --sysroot $sysroot" >&2; exit 1; }
    command -v debootstrap > /dev/null || { echo "debootstrap is not installed (apt-get install debootstrap)" >&2; exit 1; }
    # The user who asked (sudo's) or the owner of the closest existing folder gets the sysroot: the build does not
    # run as root.
    existing=$sysroot
    while [[ ! -d $existing ]]; do existing=$(dirname "$existing"); done
    owner=${SUDO_UID:+$SUDO_UID:$SUDO_GID}
    [[ -n $owner ]] || owner=$(stat -c %u:%g "$existing")
    rm -rf "$sysroot"
    mkdir -p "$sysroot"
    made=$sysroot
    while [[ $made != "$existing" ]]; do chown "$owner" "$made"; made=$(dirname "$made"); done
    debootstrap --foreign --variant=minbase --arch=arm64 --include="$(IFS=,; echo "${sysroot_packages[*]}")" \
        bookworm "$sysroot" http://deb.debian.org/debian
    # The first stage unpacks only the base packages and downloads the others: unpack those too.
    for deb in "$sysroot"/var/cache/apt/archives/*.deb; do dpkg-deb -x "$deb" "$sysroot"; done
    chown -R "$owner" "$sysroot"
    chmod -R u+rX "$sysroot"
    echo "sysroot: $sysroot"
    exit 0
fi

for tool in cmake ninja gcc python3 rsync; do
    command -v "$tool" > /dev/null || { echo "$tool is not installed (the packages are listed in this script's header)" >&2; exit 1; }
done

# The main checkout, when the repository is one of its worktrees (<main>/work/wt/<name>).
main=$root
[[ $(basename "$(dirname "$root")") == wt && $(basename "$(dirname "$(dirname "$root")")") == work ]] && main=$(dirname "$(dirname "$(dirname "$root")")")
roms=${LIVADIX_ROMS:-}
[[ -n $roms ]] || { [[ -d $root/roms ]] && roms=$root/roms || { [[ -d $main/roms ]] && roms=$main/roms || true; }; }
data=${LIVADIX_DATA:-}
[[ -n $data ]] || { [[ -d $root/data ]] && data=$root/data || { [[ -d $main/data ]] && data=$main/data || true; }; }
[[ -z $roms ]] || export LIVADIX_ROMS=$roms
[[ -z $data ]] || export LIVADIX_DATA=$data

# The tree that is built and tested: a copy of the repository on the Linux file system, or the repository itself.
tree=$root
if [[ $in_place -eq 0 ]]; then
    tree="$build_root/repo"
    mkdir -p "$tree/work"
    # Everything but git, the Windows builds, the archives, the user's ROMs and disks, the work of the tests and the
    # configuration of a run in the repository's root.
    rsync -a --delete --exclude /.git --exclude /work --exclude /dist --exclude /roms --exclude /data \
        --exclude /build --exclude /livadix.yaml --exclude /nvram "$root/" "$tree/"
    [[ ! -d $main/work/cpu || $main == "$tree" ]] || rsync -a "$main/work/cpu/" "$tree/work/cpu/"
fi
version=$(head -n 1 "$tree/src/VERSION" | tr -d '[:space:]')    # what is built, whatever the repository says by now

# Trains the instrumented program $1: the workloads tools/build_dist.ps1 trains on, in $build_root/pgo-train (removed
# after). Returns 1 when there is none.
pgo_train() {
    local program=$1 train="$build_root/pgo-train" fixture="$main/work/pgo" trained=0
    rm -rf "$train"; mkdir -p "$train"
    local env_run=(env "SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen}" "SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}")
    if [[ -n $roms && -f $roms/quadra-700.rom ]]; then
        printf 'model: quadra-700\nemulation:\n  rom: %s\nstorage:\n  disks: []\n' "$roms/quadra-700.rom" > "$train/training.yaml"
        echo "training: Quadra 700"
        (cd "$train" && "${env_run[@]}" "$program" --config "$train/training.yaml" --hidden --frames 600 > /dev/null)
        trained=1
    fi
    if [[ -f $fixture/osx.yaml && -f $fixture/osx-seed.img ]]; then
        # A disposable copy: the image and NVRAM from their seeds, the Windows paths made Linux ones.
        cp --sparse=always "$fixture/osx-seed.img" "$train/osx.img"
        mkdir -p "$train/nvram"
        [[ ! -d $fixture/nvram-seed ]] || cp "$fixture/nvram-seed/"* "$train/nvram/" 2> /dev/null || true
        python3 - "$fixture/osx.yaml" "$train" "${roms:-$main/roms}" > "$train/osx.yaml" <<'PYTHON'
import re, sys
from pathlib import PurePosixPath
src, train, roms = sys.argv[1:4]
def place(m):
    key, path = m.group(1), m.group(2).strip().strip("'\"")
    name = PurePosixPath(path.replace('\\', '/')).name
    if '/' not in path.replace('\\', '/'): return m.group(0)        # not a path (rom: auto)
    if key == 'image' and name == 'osx.img': return f'{key}: {train}/osx.img'
    if key == 'nvram': return f'{key}: {train}/nvram/{name}'
    if key == 'rom': return f'{key}: {roms}/{name}'
    drive = re.match(r'([A-Za-z]):/(.*)', path.replace('\\', '/'))
    return f'{key}: /mnt/{drive.group(1).lower()}/{drive.group(2)}' if drive else m.group(0)
for line in open(src, encoding='utf-8'):
    sys.stdout.write(re.sub(r'\b(image|nvram|rom):\s*(\S.*)$', place, line.rstrip('\n')) + '\n')
PYTHON
        echo "training: G3 / Mac OS X"
        (cd "$train" && "${env_run[@]}" "$program" --hidden --unthrottled --config "$train/osx.yaml" --frames 4500 \
            --screenshot "$train/osx-training.bmp" > "$train/osx-training.log" 2>&1) ||
            { echo "the Mac OS X training run failed" >&2; tail -5 "$train/osx-training.log" >&2; exit 1; }
        trained=1
    fi
    rm -rf "$train"
    [[ $trained -eq 1 ]]
}

# Builds one target in $build_root/<arch>; x64 also runs the tests with --test, and with want_pgo builds the
# program with the kept profile (with --train trained again first).
build_target() {
    local arch=$1 run_tests=$2 want_pgo=${3:-0}
    local dir="$build_root/$arch"
    [[ $clean -eq 0 ]] || rm -rf "$dir"
    # a build tree made from another source folder starts again
    if [[ -f $dir/CMakeCache.txt ]] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$tree/src" "$dir/CMakeCache.txt"; then rm -rf "$dir"; fi
    mkdir -p "$dir"
    local args=(-S "$tree/src" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release)
    if [[ $arch == arm64 ]]; then
        command -v aarch64-linux-gnu-gcc > /dev/null || { echo "aarch64-linux-gnu-gcc is not installed (apt-get install gcc-aarch64-linux-gnu)" >&2; exit 1; }
        [[ -f $sysroot/usr/include/X11/Xlib.h ]] || { echo "no arm64 sysroot at $sysroot: sudo tools/build_linux.sh sysroot --sysroot $sysroot" >&2; exit 1; }
        cat > "$dir/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_SYSROOT "$sysroot")
set(CMAKE_FIND_ROOT_PATH "$sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
        args+=(-DCMAKE_TOOLCHAIN_FILE="$dir/toolchain.cmake")
        export PKG_CONFIG_SYSROOT_DIR="$sysroot"
        export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
        unset PKG_CONFIG_PATH
    fi
    echo "== linux-$arch: $dir (sources: $tree${roms:+, ROMs: $roms})"
    local pgodir="$dir/pgo" pgo_mode=""
    local kept="$main/work/pgo/linux-$arch"
    if [[ $want_pgo -eq 1 && $train -eq 1 ]]; then
        rm -rf "$pgodir"
        cmake "${args[@]}" -DMC_PGO=GENERATE -DMC_PGO_DIR="$pgodir"
        cmake --build "$dir" --target Livadix
        if pgo_train "$dir/livadix"; then
            pgo_mode=USE
            rm -rf "$kept"; mkdir -p "$(dirname "$kept")"; cp -r "$pgodir" "$kept"
        else echo "no training media: built without profile-guided optimisation"; fi
    elif [[ $want_pgo -eq 1 && -d $kept ]]; then
        echo "linking with the kept profile ($kept)"
        rm -rf "$pgodir"; cp -r "$kept" "$pgodir"
        pgo_mode=USE
    elif [[ $want_pgo -eq 1 ]]; then
        echo "no profile kept (--train trains one): built without profile-guided optimisation"
    fi
    cmake "${args[@]}" -DMC_PGO="$pgo_mode" -DMC_PGO_DIR="$pgodir"
    if [[ $shell -eq 1 && ( $run_tests -eq 0 || $arch != x64 ) ]]; then
        cmake --build "$dir" --target Livadix
    elif [[ $shell -eq 1 ]]; then
        cmake --build "$dir"
    else
        # every library and test program that does not need src/platform or the window
        local names
        names=$(ninja -C "$dir" -t targets all | sed -n 's/^\(mc_[A-Za-z0-9_]*\): .*/\1/p' | sort -u |
                grep -Ev '^(mc_platform|mc_config_ui|mc_config_ui_tests|mc_shell_tests|mc_look_shot|mc_paced)$' | tr '\n' ' ')
        # shellcheck disable=SC2086
        cmake --build "$dir" --target $names
    fi
    [[ $arch == x64 && $run_tests -eq 1 ]] || return 0
    local exclude=()
    [[ $oracle -eq 1 ]] || exclude+=(ppc-oracle)
    [[ $shell -eq 1 ]] || exclude+=(config-ui shell)
    local ctest_args=(--test-dir "$dir" --output-on-failure --timeout 900 -j "$(nproc)")
    if [[ ${#exclude[@]} -gt 0 ]]; then ctest_args+=(-E "^($(IFS='|'; echo "${exclude[*]}"))"); fi
    SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} ctest "${ctest_args[@]}"
    if [[ $shell -eq 1 ]]; then
        # one start of the program itself: the Built-in machine, five frames, a screenshot
        local scratch="$build_root/smoke"
        rm -rf "$scratch"; mkdir -p "$scratch"
        local run=(env "SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen}" "SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}" "$dir/livadix")
        (cd "$scratch" && [[ $("${run[@]}" --version) == "Livadix $version" ]] &&
            "${run[@]}" --hidden --frames 5 --screenshot "$scratch/test.bmp" > "$scratch/out.txt" &&
            [[ -s $scratch/test.bmp ]]) || { echo "the smoke run of the program failed" >&2; cat "$scratch/out.txt" >&2 || true; exit 1; }
        echo "smoke run of the program: ok"
    fi
    if [[ $verify -eq 1 ]]; then
        # the SingleStepTests suite is in the build copy's work/cpu
        LIVADIX_BUILD="$dir" SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
            python3 "$tree/tests/verify.py" $checks
    fi
}

# The archive of a target: what tools/build_dist.ps1 puts in the Windows zip, but for the program.
package_target() {
    local arch=$1 dir="$build_root/$1"
    local stage="$build_root/stage"
    rm -rf "$stage"; mkdir -p "$stage" "$root/dist/releases"
    # The archive holds exactly what tools/build_dist.ps1 puts in the Windows zip, but for the program: the file list
    # is read from that script, the notes are written as it writes them, the archive is opened again and every member
    # compared with what was staged.
    python3 - "$tree" "$dir" "$stage" "$root/dist/releases" "$version" "$arch" "$build_root/x64/livadix" <<'PYTHON'
import hashlib, os, re, shutil, subprocess, sys, tarfile, tempfile
from pathlib import Path

tree, build, stage, out, version, arch, lister = (Path(a) if i < 4 or i == 6 else a for i, a in enumerate(sys.argv[1:]))
name = 'Livadix'
folder = f'{name}-{version}-linux-{arch}'
program = build / 'livadix'
# The ROM catalogue comes from a program that runs here: the x64 one for an arm64 archive.
listing_program = program if arch == 'x64' else lister

script = (tree / 'tools' / 'build_dist.ps1').read_text(encoding='utf-8')
def names(variable):
    m = re.search(r'\$' + variable + r'\s*=\s*@\((.*?)\)', script, re.S)
    found = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    if not found:
        raise SystemExit(f'tools/build_dist.ps1 has no {variable} list')
    return found
user_docs = names('userDocs')

# the program must be a 64-bit little-endian ELF for the platform: x86-64 (62) or AArch64 (183)
head = program.read_bytes()[:20]
machine = {'x64': 0x3E, 'arm64': 0xB7}[arch]
if head[:4] != b'\x7fELF' or head[4] != 2 or head[5] != 1 or int.from_bytes(head[18:20], 'little') != machine:
    raise SystemExit(f'{program} is not a 64-bit {arch} ELF program')
if not listing_program.is_file():
    raise SystemExit(f'{listing_program} is missing: the ROM catalogue is read from it')

root = stage / folder
(root / 'docs').mkdir(parents=True)
(root / 'roms').mkdir()
shutil.copy2(program, root / 'livadix')
os.chmod(root / 'livadix', 0o755)
# README.md without its first lines, the logo from assets/ that only GitHub shows.
readme = re.sub(r'\A!\[[^\]]*\]\(assets/[^)]*\)\n\n?', '', (tree / 'README.md').read_text(encoding='utf-8'))
(root / 'README.md').write_text(readme, encoding='utf-8', newline='')
for doc in user_docs:
    shutil.copy2(tree / 'docs' / doc, root / 'docs')

# The ROM note: the standard names Livadix looks for in roms/ (livadix --list-roms), from a folder without roms/.
with tempfile.TemporaryDirectory() as empty:
    p = subprocess.run([str(listing_program), '--list-roms'], cwd=empty, capture_output=True, text=True,
                       env={**os.environ, 'SDL_VIDEODRIVER': 'offscreen', 'SDL_AUDIODRIVER': 'dummy'})
if p.returncode:
    raise SystemExit(f'livadix --list-roms failed ({p.returncode})')
listing = [l for l in p.stdout.splitlines() if not re.match(r'^(ROM folder |  )', l)]
if len(listing) < 2 or not listing[0].startswith('standard name'):
    raise SystemExit('livadix --list-roms produced no ROM catalogue')
roms = [f'{name} {version} - Macintosh ROMs', '',
        f'{name} includes no Apple ROM. Its Built-in ROM is embedded in livadix;',
        'the executable starts by itself. Copy your own Macintosh ROMs into',
        f'this folder under their standard names below, or under any name: {name} finds a ROM by its',
        'checksum, and emulation.rom in livadix.yaml names a file anywhere. The model: setting',
        'picks the model and so the ROM (livadix --list-models; README.md).', '']
roms += ['  ' + l for l in listing] + ['', "The ROM's own checksum is checked at start-up."]
(root / 'roms' / 'PUT_ROMS_HERE.txt').write_text('\n'.join(roms) + '\n', encoding='utf-8', newline='\n')


# nothing but these
expected = sorted(['livadix', 'README.md', 'roms/PUT_ROMS_HERE.txt'] +
                  [f'docs/{d}' for d in user_docs])
actual = sorted(str(f.relative_to(root)) for f in root.rglob('*') if f.is_file())
if actual != expected:
    raise SystemExit(f'the release does not hold exactly the release files:\n  expected {expected}\n  found    {actual}')

# the archive, a folder of its own name, then opened again
archive = stage / f'{folder}.tar.gz'
def entry(info):
    info.uid = info.gid = 0
    info.uname = info.gname = ''
    info.mtime = int(os.environ.get('SOURCE_DATE_EPOCH', info.mtime))
    return info
with tarfile.open(archive, 'w:gz', compresslevel=9) as tar:
    tar.add(root, arcname=folder, recursive=False, filter=entry)
    for path in sorted(root.rglob('*')):
        tar.add(path, arcname=f'{folder}/{path.relative_to(root).as_posix()}', recursive=False, filter=entry)
with tarfile.open(archive) as tar:
    members = {m.name: m for m in tar.getmembers() if m.isfile()}
    if sorted(members) != sorted(f'{folder}/{e}' for e in expected):
        raise SystemExit('the archive does not hold the staged files')
    for e in expected:
        a = hashlib.sha256((root / e).read_bytes()).hexdigest()
        b = hashlib.sha256(tar.extractfile(members[f'{folder}/{e}']).read()).hexdigest()
        if a != b:
            raise SystemExit(f'the archive\'s {e} differs from the staged one')
    if not members[f'{folder}/livadix'].mode & 0o111:
        raise SystemExit('the archive\'s program is not executable')

out.mkdir(parents=True, exist_ok=True)
for old in out.glob(f'{name}-*'):    # releases/ holds the current version only
    if old.is_file() and not old.name.startswith(f'{name}-{version}-'):
        old.unlink()
for old in out.parent.glob(f'{name}-*-linux-{arch}.tar.gz'):    # archives of the earlier layout, in dist/ itself
    old.unlink()
shutil.move(str(archive), out / archive.name)
print(f'release: {folder}')
for e in expected:
    print(f'  {e:<40} {(root / e).stat().st_size:>12,} bytes')
print(f'OK: dist/releases/{archive.name}')
PYTHON
    rm -rf "$stage"
}

for t in "${targets[@]}"; do
    if [[ $t == arm64 && $package -eq 1 && $shell -eq 1 && ! -x $build_root/x64/livadix ]]; then
        build_target x64 0      # the arm64 archive's ROM catalogue comes from the x64 program
    fi
    build_target "$t" "$test" "$([[ $t == x64 && $pgo -eq 1 && $package -eq 1 && $shell -eq 1 ]] && echo 1 || echo 0)"
    if [[ $package -eq 1 && $shell -eq 1 ]]; then package_target "$t"; fi
done
