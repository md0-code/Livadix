cmake_minimum_required(VERSION 3.24)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MC_VERSION_TEXT)
string(STRIP "${MC_VERSION_TEXT}" MC_VERSION_TEXT)
project(Livadix VERSION ${MC_VERSION_TEXT} LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(cmake/Warnings.cmake)
# ARM64 with GCC: plain char signed and no fused multiply-add, as the x86-64
# builds compile the code (GCC's AArch64 default is an unsigned char and
# contracted floating point).
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    add_compile_options(-fsigned-char -ffp-contract=off)
endif()
# The interpreter will live on inlining: full inlining in every optimised
# build, and whole-program optimisation for the shipped Release build.
if(MSVC)
    string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
# Profile-guided optimisation of livadix.exe (tools/build_dist.ps1): GENERATE
# builds the instrumented program, USE links with the recorded profile. A
# test DLL exporting core symbols must set INTERPROCEDURAL_OPTIMIZATION OFF:
# export-all reads symbols from the objects, which LTCG objects do not carry.
set(MC_PGO "" CACHE STRING "Profile-guided optimisation: empty, GENERATE or USE")
set(MC_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Profile database folder")
# GCC (tools/build_linux.sh): every object instrumented, then compiled with
# the recorded counts; code the training never reached is optimised as
# without a profile, and the counts of the program's other threads are
# reconciled rather than locked.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND MC_PGO STREQUAL "GENERATE")
    add_compile_options(-fprofile-generate=${MC_PGO_DIR})
    add_link_options(-fprofile-generate=${MC_PGO_DIR})
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND MC_PGO STREQUAL "USE")
    add_compile_options(-fprofile-use=${MC_PGO_DIR} -fprofile-partial-training -fprofile-correction -Wno-missing-profile
                        -Wno-coverage-mismatch)
endif()
configure_file(mc_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/mc_version.h @ONLY)
# Tests write only under the repository's git-ignored work/ folder.
get_filename_component(MC_WORK "${CMAKE_CURRENT_SOURCE_DIR}/../work" ABSOLUTE)
get_filename_component(MC_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests" ABSOLUTE)

file(GLOB MC_YAML_SOURCES third_party/libyaml/src/*.c)
add_library(mc_yaml STATIC ${MC_YAML_SOURCES})
target_include_directories(mc_yaml PUBLIC third_party/libyaml/include)
target_compile_definitions(mc_yaml PUBLIC YAML_DECLARE_STATIC PRIVATE
    YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5"
    _CRT_SECURE_NO_WARNINGS)

# The emulator core: no SDL, no wall clock. The machines (src/hw) and their
# chips, the storage helpers (virtual disk, floppy tracks) and Livadix's own
# guest code (the block driver image) are part of it.
add_library(mc_core STATIC core/nvram.c core/config.c core/profile.c core/yamledit.c core/models.c core/hfs.c core/compat.c core/romdb.c core/disk.c core/media.c core/stub_machine.c
    core/cpu/m68k_run.c core/cpu/m68k_ops.c core/cpu/m68k_table.c core/cpu/m68k_timing.c core/cpu/m68k_000.c
    core/cpu/mmu.c core/cpu/mmu040.c core/cpu/mmu030.c core/cpu/mmu851.c core/cpu/fpu.c core/cpu/fpu_soft.c core/cpu/fpu_trans.c core/cpu/jit_x64.c
    hw/machine.c hw/bus.c hw/amu.c hw/sched.c hw/irq.c hw/via.c hw/rtc.c hw/mcu.c hw/absent.c hw/video/dafb.c hw/adb.c hw/adb_via.c
    hw/cpu6805.c hw/adbctl_mcu.c hw/adb_wire.c hw/adbctl.c hw/egret.c hw/cuda.c
    hw/cpu6502.c hw/iop.c hw/dsp3210.c
    hw/asc.c hw/scc.c hw/sonic.c hw/swim.c hw/drivesnd.c hw/storage/floppy_track.c hw/nubus.c hw/livadix_card.c hw/dp8390.c hw/enet_card.c
    hw/am29f010.c hw/scsi_bus.c hw/ncr53c96.c hw/ncr5380.c hw/scsi_disk.c hw/scsi_cd.c hw/ata.c hw/storage/vdisk.c
    hw/cpu740.c hw/m50753.c hw/pmu.c hw/pmu_fw.c hw/pmu_pge.c
    hw/guest/guest.c
    hw/asic/mdu.c hw/asic/rbv.c hw/asic/djmemc.c hw/asic/iosb.c hw/asic/memcjr.c hw/asic/primetime.c
    hw/asic/f108.c hw/asic/primetime2.c hw/asic/v8.c hw/video/valkyrie.c hw/video/mach64.c hw/asic/sonora.c hw/asic/vasp.c hw/asic/glue.c hw/video/se30video.c
    hw/asic/mca.c hw/asic/oss.c hw/asic/psc.c hw/newage.c hw/am79c940.c hw/video/civic.c
    hw/machines/quadra700.c hw/machines/quadra900.c hw/machines/iifx.c hw/machines/quadra800.c hw/machines/iici.c hw/machines/quadra605.c
    hw/machines/quadra630.c hw/machines/iisi.c hw/machines/lc.c hw/machines/lc3.c hw/machines/iivx.c hw/machines/macii.c hw/machines/quadra840av.c
    hw/asic/jaws.c hw/asic/msc.c hw/video/csc.c hw/video/gsc.c hw/video/wd90c26.c hw/machines/powerbook.c
    hw/asic/pratt.c hw/machines/powerbook500.c hw/machines/portable.c
    hw/m0110.c hw/quadmouse.c hw/video/compactvideo.c hw/machines/compact.c
    hw/machines/se.c
    hw/ppc/chips/hmc.c hw/ppc/chips/cpuid.c hw/ppc/chips/amic.c hw/ppc/chips/pdm_video.c hw/ppc/chips/ariel.c
    hw/ppc/chips/awac.c hw/ppc/chips/bart.c hw/ppc/chips/swim3.c)
target_include_directories(mc_core PUBLIC core core/cpu hw hw/storage hw/guest PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mc_core PUBLIC mc_yaml)
if(NOT WIN32)
    target_link_libraries(mc_core PUBLIC m)
endif()
mc_set_warnings(mc_core)
# The chip tests link the chips through this name.
add_library(mc_hw INTERFACE)
target_link_libraries(mc_hw INTERFACE mc_core)

# The guest images (tools/guest_build.py builds them from src/hw/guest/drivers)
# as C arrays.
set(MC_GUEST_IMAGES livadixdisk livadixdisk_ata livadixcard livadixcard24 livadixcard_pci)
set(MC_GUEST_HEADER "")
foreach(image ${MC_GUEST_IMAGES})
    set(bin ${CMAKE_CURRENT_SOURCE_DIR}/hw/guest/bin/${image}.bin)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${bin})
    file(READ ${bin} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    string(APPEND MC_GUEST_HEADER "static const unsigned char mc_guest_${image}_bin[]={${hex}};\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/mc_guest_images.h "${MC_GUEST_HEADER}")

# The Built-in ROM is embedded in mc_core: no external ROM is needed.
# Rebuild its code/data, fonts, version and current README/changelog whenever
# an input changes. Keep a binary image in the build folder for diagnostics.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MC_BOOTROM_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/hw/guest/bin/bootrom.bin ${CMAKE_CURRENT_SOURCE_DIR}/app/ui_font.c
    ${CMAKE_CURRENT_SOURCE_DIR}/VERSION ${CMAKE_CURRENT_SOURCE_DIR}/../README.md ${CMAKE_CURRENT_SOURCE_DIR}/../docs/CHANGELOG.md
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/bootrom.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MC_BOOTROM_INPUTS})
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/bootrom.py
                        --bin ${CMAKE_CURRENT_BINARY_DIR}/roms/livadix.rom
                        --header ${CMAKE_CURRENT_BINARY_DIR}/generated/mc_builtin_rom.h
                RESULT_VARIABLE MC_BOOTROM_RESULT)
if(NOT MC_BOOTROM_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/bootrom.py could not build the built-in ROM")
endif()

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
    URL https://www.libsdl.org/release/SDL2-2.32.10.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL2)

# The network beside the Macintosh: the user-mode NAT on the host's sockets
# (src/net), outside the core: it keeps wall time and runs a thread.
add_library(mc_net STATIC net/nat.c net/net.c net/share.c
    net/appletalk/atalk.c net/appletalk/atp.c net/appletalk/asp.c
    net/afp/afp.c net/afp/afp_host.c net/afp/afp_meta.c net/afp/afp_store.c net/afp/macroman.c net/afp/des.c)
target_link_libraries(mc_net PUBLIC mc_core)
target_include_directories(mc_net PUBLIC net)
if(WIN32)
    target_link_libraries(mc_net PUBLIC ws2_32 iphlpapi)
else()
    target_sources(mc_net PRIVATE net/afp/afp_host_posix.c)
    find_package(Threads REQUIRED)
    target_link_libraries(mc_net PUBLIC Threads::Threads)
endif()
mc_set_warnings(mc_net)

# The shell: the platform layer and the application.
add_library(mc_platform STATIC platform/video.c platform/audio.c platform/overlay.c platform/serial.c)
target_link_libraries(mc_platform PUBLIC mc_core SDL2::SDL2-static)
target_compile_definitions(mc_platform PUBLIC SDL_MAIN_HANDLED)
mc_set_warnings(mc_platform)

# app/livadix.yaml is the default configuration written on first start.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/livadix.yaml)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/app/livadix.yaml MC_DEFAULT_YAML HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MC_DEFAULT_YAML "${MC_DEFAULT_YAML}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/livadix_default_yaml.h "static const unsigned char mc_default_yaml[]={${MC_DEFAULT_YAML}};\n")

# The configuration window (src/app/config_*.c and its widget toolkit,
# ui*.c), on the core's configuration code; drawn by livadix.exe over the
# Macintosh's screen. The application part builds without SDL for the tests.
add_library(mc_config_ui STATIC app/config_app.c app/config_host.c app/config_picker.c app/ui.c app/ui_font.c app/ui_icons.c)
target_include_directories(mc_config_ui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mc_config_ui PUBLIC mc_core)
mc_set_warnings(mc_config_ui)

add_executable(Livadix app/livadix.c app/desktop.c app/shutdown.c app/keymap.c)
# The file is livadix.exe, as livadix.yaml.
set_target_properties(Livadix PROPERTIES OUTPUT_NAME livadix)
target_include_directories(Livadix PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(Livadix PRIVATE mc_platform mc_net mc_config_ui)
if(WIN32)
    target_link_libraries(Livadix PRIVATE shell32)
    # A Windows program without a console window of its own (app/livadix.c main).
    set_target_properties(Livadix PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(Livadix PRIVATE app/livadix.manifest)   # UTF-8 file names
    # The executable's icon: assets/livadix.ico (tools/embed_icon.py).
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/livadix_icon.rc "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/../assets/livadix.ico\"\n")
    target_sources(Livadix PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/livadix_icon.rc)
endif()
if(MSVC)
    target_link_options(Livadix PRIVATE /ENTRY:mainCRTStartup)
endif()
if(MSVC AND MC_PGO STREQUAL "GENERATE")
    file(MAKE_DIRECTORY ${MC_PGO_DIR})
    target_link_options(Livadix PRIVATE /LTCG /GENPROFILE:PGD=${MC_PGO_DIR}/Livadix.pgd)
elseif(MSVC AND MC_PGO STREQUAL "USE")
    target_link_options(Livadix PRIVATE /LTCG /USEPROFILE:PGD=${MC_PGO_DIR}/Livadix.pgd)
endif()
mc_set_warnings(Livadix)

enable_testing()
# The core: configuration, profiles, ROM identification, disk images, the
# machine interface (stub machine).
add_executable(mc_tests ${MC_TESTS}/test_main.c ${MC_TESTS}/test_core.c)
target_link_libraries(mc_tests PRIVATE mc_core)
target_compile_definitions(mc_tests PRIVATE MC_WORK="${MC_WORK}")
target_include_directories(mc_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
mc_set_warnings(mc_tests)
add_test(NAME core COMMAND mc_tests core)
# The configuration window without a window: every tab, a disk added, the
# model changed (its presets), profiles switched and edited, Save, and the
# file read back equal.
add_executable(mc_config_ui_tests ${MC_TESTS}/test_config_ui.c)
target_link_libraries(mc_config_ui_tests PRIVATE mc_config_ui)
target_include_directories(mc_config_ui_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(mc_config_ui_tests PRIVATE MC_WORK="${MC_WORK}")
mc_set_warnings(mc_config_ui_tests)
add_test(NAME config-ui COMMAND mc_config_ui_tests)
# The CPU: unit tests from the manuals, the SingleStepTests 68000 suite
# (tests/cpu_suite.py fetches it into work/; skipped when absent) and the
# interpreter benchmark (tests/verify.py, check cpu-speed).
add_executable(mc_cpu_tests ${MC_TESTS}/cpu/main.c ${MC_TESTS}/cpu/test_cpu.c ${MC_TESTS}/cpu/test_mmu.c ${MC_TESTS}/cpu/test_68851.c ${MC_TESTS}/cpu/test_fpu.c ${MC_TESTS}/cpu/fpu_eval.c
    ${MC_TESTS}/cpu/fault_sweep.c ${MC_TESTS}/cpu/sst.c
    ${MC_TESTS}/cpu/bench.c ${MC_TESTS}/cpu/test_jit.c ${MC_TESTS}/cpu/test_timing.c)
target_link_libraries(mc_cpu_tests PRIVATE mc_core)
mc_set_warnings(mc_cpu_tests)
add_test(NAME cpu COMMAND mc_cpu_tests unit)
add_test(NAME cpu-mmu COMMAND mc_cpu_tests mmu)
add_test(NAME test_68851 COMMAND mc_cpu_tests 68851)
add_test(NAME cpu-fpu COMMAND mc_cpu_tests fpu)
add_test(NAME cpu-faults COMMAND mc_cpu_tests faults)
# The translator against the interpreter on random programs.
add_test(NAME cpu-timing COMMAND mc_cpu_tests timing)
# Seed 5669: a memory-indirect MOVEM must read its pointer only once.
add_test(NAME cpu-jit-movem-indirect COMMAND mc_cpu_tests jit 10 5660 1000000)
# The random programs (3000, seeds 1-3000) and the SingleStepTests groups in
# slices of 8, one test each, so that ctest -j runs them side by side.
foreach(i RANGE 0 7)
    math(EXPR jit_seed "1 + ${i} * 375")
    add_test(NAME cpu-jit-${i} COMMAND mc_cpu_tests jit 375 ${jit_seed})
    add_test(NAME cpu-sst-${i} COMMAND mc_cpu_tests sst ${MC_WORK}/cpu/sst %${i}/8)
    add_test(NAME cpu-sst-jit-${i} COMMAND mc_cpu_tests sst ${MC_WORK}/cpu/sst %${i}/8 --jit)
    set_tests_properties(cpu-sst-${i} cpu-sst-jit-${i} PROPERTIES SKIP_RETURN_CODE 77)
    # The same suite on the 68000 model, compared in full (bus cycles and clocks too).
    add_test(NAME cpu-sst000-${i} COMMAND mc_cpu_tests sst ${MC_WORK}/cpu/sst %${i}/8 --68000)
    set_tests_properties(cpu-sst000-${i} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
# A 68040 ROM from reset in a bare memory map (tests/verify.py, check rom-trace).
add_executable(mc_rom_trace ${MC_TESTS}/cpu/rom_trace.c)
target_link_libraries(mc_rom_trace PRIVATE mc_core)
mc_set_warnings(mc_rom_trace)
get_filename_component(MC_ROMS "${CMAKE_CURRENT_SOURCE_DIR}/../roms" ABSOLUTE)
# LIVADIX_ROMS names another ROM folder (a worktree reads copies under work/).
if(DEFINED ENV{LIVADIX_ROMS})
    file(TO_CMAKE_PATH "$ENV{LIVADIX_ROMS}" MC_ROMS)
endif()
add_test(NAME rom-trace COMMAND mc_rom_trace ${MC_ROMS}/quadra-700.rom --check)
add_test(NAME rom-trace-1m COMMAND mc_rom_trace ${MC_ROMS}/quadra-700.rom --check --continue 4096 --max 1000000)
set_tests_properties(rom-trace rom-trace-1m PROPERTIES SKIP_RETURN_CODE 77)
# The Macintosh Plus ROM on the 68000 model in the compact Macintosh's bare map.
add_test(NAME rom-trace-68000 COMMAND mc_rom_trace ${MC_ROMS}/mac-plus.rom --68000 --check)
add_test(NAME rom-trace-68000-1m COMMAND mc_rom_trace ${MC_ROMS}/mac-plus.rom --68000 --check --continue 4096 --max 1000000)
set_tests_properties(rom-trace-68000 rom-trace-68000-1m PROPERTIES SKIP_RETURN_CODE 77)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    # The FPU against exact rational arithmetic and mpmath (skipped without mpmath).
    add_test(NAME fpu-suite COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fpu_suite.py --cases 800)
    set_tests_properties(fpu-suite PROPERTIES SKIP_RETURN_CODE 77)
endif()
# The chips and machines: datasheet unit tests per chip, the bus,
# scheduler and interrupt router, and the Quadra 700 ROM's boot. Every chip
# test runs against a fake machine host (tests/hw/hw_test.c).
add_executable(mc_hw_tests ${MC_TESTS}/hw/main.c ${MC_TESTS}/hw/hw_test.c ${MC_TESTS}/hw/test_bus.c ${MC_TESTS}/hw/test_via.c ${MC_TESTS}/hw/test_rtc.c
    ${MC_TESTS}/hw/test_adb_via.c ${MC_TESTS}/hw/test_dafb.c)
target_link_libraries(mc_hw_tests PRIVATE mc_core)
target_include_directories(mc_hw_tests PRIVATE ${MC_TESTS}/hw)
target_compile_definitions(mc_hw_tests PRIVATE MC_WORK="${MC_WORK}")
mc_set_warnings(mc_hw_tests)
foreach(suite bus via rtc adb-via dafb)
    add_test(NAME hw-${suite} COMMAND mc_hw_tests ${suite})
endforeach()
# The real machine traced from reset (diagnosis; tests/verify.py boot gate).
add_executable(mc_hw_trace ${MC_TESTS}/hw/hw_trace.c)
target_link_libraries(mc_hw_trace PRIVATE mc_core mc_net)
mc_set_warnings(mc_hw_trace)
# One program per phase-2c chip, standalone against the fake host.
function(mc_hw_test name)
    add_executable(mc_${name} ${MC_TESTS}/hw/${name}.c ${MC_TESTS}/hw/hw_test.c ${ARGN})
    target_link_libraries(mc_${name} PRIVATE mc_hw)
    target_include_directories(mc_${name} PRIVATE ${MC_TESTS}/hw)
    target_compile_definitions(mc_${name} PRIVATE MC_WORK="${MC_WORK}")
    mc_set_warnings(mc_${name})
    add_test(NAME ${name} COMMAND mc_${name})
endfunction()
mc_hw_test(test_asc)
mc_hw_test(test_am29f010)
mc_hw_test(test_scc)
mc_hw_test(test_ltpc)
mc_hw_test(test_psx)
mc_hw_test(test_mach64)
mc_hw_test(test_sonic)
mc_hw_test(test_swim)
mc_hw_test(test_swim2)
mc_hw_test(test_iwm)
mc_hw_test(test_djmemc)
mc_hw_test(test_iosb)
mc_hw_test(test_memcjr)
mc_hw_test(test_primetime)
mc_hw_test(test_f108)
mc_hw_test(test_valkyrie)
mc_hw_test(test_primetime2)
mc_hw_test(test_psc)
mc_hw_test(test_civic)
mc_hw_test(test_newage)
mc_hw_test(test_am79c940)
mc_hw_test(test_dp8390)
mc_hw_test(test_vdisk)
mc_hw_test(test_scsi_target)
mc_hw_test(test_ncr53c96)
mc_hw_test(test_ncr5380)
mc_hw_test(test_740)
mc_hw_test(test_pmu)
target_compile_definitions(mc_test_pmu PRIVATE MC_ROMS="${MC_ROMS}")
# The PMU's protocol level against Apple's firmware on the 740 core (skipped
# without pmu-pb140.bin and pmu-pb160.bin in roms/).
add_test(NAME pmu-diff COMMAND mc_test_pmu --diff ${MC_ROMS})
set_tests_properties(pmu-diff PROPERTIES SKIP_RETURN_CODE 77)
mc_hw_test(test_ata)
# The Power Macintosh 6100/7100/8100 chips (src/hw/ppc/chips, phase 6.4).
foreach(chip hmc amic pdm_video bart swim3)
    mc_hw_test(test_${chip})
endforeach()
mc_hw_test(test_livadixdisk)
mc_hw_test(test_livadix_card)
# The 68HC05 core and the Egret/Cuda chip; Egret and Cuda at the protocol level.
mc_hw_test(test_6805)
mc_hw_test(test_egret ${MC_TESTS}/hw/adbctl_host.c ${MC_TESTS}/hw/adbctl_suite.c)
mc_hw_test(test_cuda ${MC_TESTS}/hw/adbctl_host.c ${MC_TESTS}/hw/adbctl_suite.c)
# Egret and Cuda: the protocol level against Apple's firmware, one run per
# dump (skipped without it).
add_executable(mc_test_adbctl_diff ${MC_TESTS}/hw/test_adbctl_diff.c ${MC_TESTS}/hw/adbctl_host.c ${MC_TESTS}/hw/hw_test.c)
target_link_libraries(mc_test_adbctl_diff PRIVATE mc_hw)
target_include_directories(mc_test_adbctl_diff PRIVATE ${MC_TESTS}/hw)
target_compile_definitions(mc_test_adbctl_diff PRIVATE MC_WORK="${MC_WORK}")
mc_set_warnings(mc_test_adbctl_diff)
foreach(fw egret-341s0850 egret-341s0851 egret-344s0100 cuda-341s0060 cuda-341s0417 cuda-341s0788 cuda-341s0789)
    add_test(NAME adbctl-diff-${fw} COMMAND mc_test_adbctl_diff ${MC_ROMS}/${fw}.bin)
    set_tests_properties(adbctl-diff-${fw} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
mc_hw_test(test_amu)
# The Macintosh IIci's MDU and RBV (5.2d).
mc_hw_test(test_mdu)
mc_hw_test(test_rbv)
# The Macintosh II family's GLUE, its boards, the SE/30's video (5.2i).
mc_hw_test(test_glue)
mc_hw_test(test_compact)
# The V8 family (5.2e).
mc_hw_test(test_v8)
# The 68030 PowerBooks (5.2g).
mc_hw_test(test_gsc)
mc_hw_test(test_csc)
mc_hw_test(test_jaws)
mc_hw_test(test_pratt)
# Sonora and VASP (5.2f).
mc_hw_test(test_sonora)
mc_hw_test(test_vasp)
# The 65C02 core and Apple's I/O Processor.
mc_hw_test(test_6502)
mc_hw_test(test_iop)
mc_hw_test(test_dsp3210)
# The IIfx's OSS (5.2j).
mc_hw_test(test_oss)
# The ROMs' own IOP kernels on the model (skipped without the image).
foreach(rom iifx quadra-950)
    add_test(NAME iop-kernels-${rom} COMMAND mc_test_iop ${MC_ROMS}/${rom}.rom)
    set_tests_properties(iop-kernels-${rom} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
# The user-mode NAT against servers on the host's loopback interface.
add_executable(mc_test_nat ${MC_TESTS}/net/test_nat.c)
target_link_libraries(mc_test_nat PRIVATE mc_net)
mc_set_warnings(mc_test_nat)
add_test(NAME test_nat COMMAND mc_test_nat)
# The file server: AppleTalk (AARP, DDP, NBP, ATP, ASP) and AFP against
# folders under work/.
add_executable(mc_test_appletalk ${MC_TESTS}/net/test_appletalk.c)
target_link_libraries(mc_test_appletalk PRIVATE mc_net)
mc_set_warnings(mc_test_appletalk)
add_test(NAME test_appletalk COMMAND mc_test_appletalk)
add_executable(mc_test_afp ${MC_TESTS}/net/test_afp.c)
target_link_libraries(mc_test_afp PRIVATE mc_net)
target_compile_definitions(mc_test_afp PRIVATE MC_WORK="${MC_WORK}")
mc_set_warnings(mc_test_afp)
add_test(NAME test_afp COMMAND mc_test_afp)
# The shell without a window: ADB key mapping, sound output (dummy driver).
add_executable(mc_shell_tests ${MC_TESTS}/test_shell.c app/keymap.c)
target_link_libraries(mc_shell_tests PRIVATE mc_platform)
target_compile_definitions(mc_shell_tests PRIVATE MC_WORK="${MC_WORK}")
mc_set_warnings(mc_shell_tests)
add_test(NAME shell COMMAND mc_shell_tests)
set_tests_properties(shell PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")
# The window's looks rendered in a hidden window (tests/verify.py); needs a
# GPU, so not a ctest.
add_executable(mc_look_shot ${MC_TESTS}/look_shot.c)
target_link_libraries(mc_look_shot PRIVATE mc_platform)
mc_set_warnings(mc_look_shot)

# The PowerPC CPU core (src/core/ppc): the 601, 603/603e, 604/604e and 750.
add_library(mc_ppc STATIC core/ppc/ppc_cpu.c core/ppc/ppc_mmu.c
    core/ppc/ppc_power.c core/ppc/ppc_fpu.c core/ppc/ppc_fp_soft.c
    core/ppc/ppc_model.c core/ppc/ppc_mmu_classic.c core/ppc/ppc_jit.c)
target_include_directories(mc_ppc PUBLIC core/ppc)
if(NOT WIN32)
    target_link_libraries(mc_ppc PUBLIC m)
endif()
mc_set_warnings(mc_ppc)
# GCC has no #pragma STDC FP_CONTRACT: no fused multiply-add in the PowerPC FPU's exact arithmetic.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(core/ppc/ppc_fpu.c PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
foreach(suite cpu mmu power fpu models model_memory model_fpu breakpoints)
    add_executable(mc_ppc_${suite}_tests ${MC_TESTS}/ppc/test_${suite}.c)
    target_link_libraries(mc_ppc_${suite}_tests PRIVATE mc_ppc)
    mc_set_warnings(mc_ppc_${suite}_tests)
    add_test(NAME ppc-${suite} COMMAND mc_ppc_${suite}_tests)
    # The same suite with each step through a one-instruction translation
    # (ppc_jit_step, phase 6.6).
    add_executable(mc_ppc_${suite}_jit_tests ${MC_TESTS}/ppc/test_${suite}.c)
    target_link_libraries(mc_ppc_${suite}_jit_tests PRIVATE mc_ppc)
    target_compile_definitions(mc_ppc_${suite}_jit_tests PRIVATE ppc_step=ppc_jit_step)
    mc_set_warnings(mc_ppc_${suite}_jit_tests)
    add_test(NAME ppc-${suite}-jit COMMAND mc_ppc_${suite}_jit_tests)
endforeach()
foreach(driver rom_trace oracle)
    add_executable(mc_ppc_${driver} ${MC_TESTS}/ppc/${driver}.c)
    target_link_libraries(mc_ppc_${driver} PRIVATE mc_ppc)
    mc_set_warnings(mc_ppc_${driver})
endforeach()
# The PowerPC machine back end (phase 6.3): the third
# branch of src/hw/machine.c, its bus, scheduler and time base, the boards.
target_sources(mc_core PRIVATE hw/ppc/ppc_machine.c hw/ppc/m68k_accel.c hw/ppc/ppc_bus.c hw/ppc/machines/pdm.c hw/ppc/machines/pci1.c hw/ppc/machines/g3.c
    hw/ppc/machines/capella.c hw/ppc/chips/capella.c hw/ppc/machines/psx.c
    hw/ppc/machines/pbx.c hw/ppc/chips/pbx.c hw/ppc/machines/pippin.c)
add_executable(mc_ppc_pbx_tests ${MC_TESTS}/ppc/test_pbx.c)
target_link_libraries(mc_ppc_pbx_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_pbx_tests)
add_test(NAME ppc-pbx COMMAND mc_ppc_pbx_tests)
add_executable(mc_ppc_pippin_tests ${MC_TESTS}/ppc/test_pippin.c)
target_link_libraries(mc_ppc_pippin_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_pippin_tests)
add_test(NAME ppc-pippin COMMAND mc_ppc_pippin_tests)
add_executable(mc_ppc_capella_tests ${MC_TESTS}/ppc/test_capella.c)
target_link_libraries(mc_ppc_capella_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_capella_tests)
add_test(NAME ppc-capella COMMAND mc_ppc_capella_tests)
target_link_libraries(mc_core PUBLIC mc_ppc)
add_executable(mc_ppc_machine_tests ${MC_TESTS}/ppc/test_machine.c)
target_link_libraries(mc_ppc_machine_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_machine_tests)
add_test(NAME ppc-machine COMMAND mc_ppc_machine_tests)
# power-macintosh-6100.rom on the PDM board of RAM and ROM, to its first hardware probe.
add_test(NAME ppc-pdm-probe COMMAND mc_ppc_machine_tests probe)
# The PDM machines from reset (phase 6.4; diagnosis and boot checks).
add_executable(mc_ppc_pdm_boot ${MC_TESTS}/ppc/pdm_boot.c)
target_link_libraries(mc_ppc_pdm_boot PRIVATE mc_core mc_net)
mc_set_warnings(mc_ppc_pdm_boot)
# power-macintosh-6100.rom on the 7100 from reset to the start-up screen (no disk: the
# floppy prompt), about 30 s of guest time.
add_test(NAME ppc-pdm-7100 COMMAND mc_ppc_pdm_boot ${MC_ROMS}/power-macintosh-6100.rom 7100 30 --expect-screen)
set_tests_properties(ppc-pdm-7100 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
set_tests_properties(ppc-pdm-probe PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
# The PowerPC translator against the interpreter (phase 6.6): random programs
# on every model, and power-macintosh-6100.rom to its first probe (tests/verify.py,
# checks ppc-jit-diff and ppc-jit-rom).
add_executable(mc_ppc_jit_tests ${MC_TESTS}/ppc/test_jit.c)
target_link_libraries(mc_ppc_jit_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_jit_tests)
add_test(NAME ppc-jit-diff COMMAND mc_ppc_jit_tests diff 8 1)
set_tests_properties(ppc-jit-diff PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
add_test(NAME ppc-jit-idle COMMAND mc_ppc_jit_tests idle)
add_test(NAME ppc-fpu-fast COMMAND mc_ppc_jit_tests fp 2000000)
set_tests_properties(ppc-jit-idle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
add_test(NAME ppc-jit-rom COMMAND mc_ppc_jit_tests rom)
set_tests_properties(ppc-jit-rom PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
if(Python3_Interpreter_FOUND)
    add_test(NAME ppc-oracle COMMAND Python3::Interpreter
        ${MC_TESTS}/ppc/oracle.py
        --livadix $<TARGET_FILE:mc_ppc_oracle> --work ${CMAKE_CURRENT_SOURCE_DIR}/../work)
    set_tests_properties(ppc-oracle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    foreach(model 603 603e 604 604e 750)
        add_test(NAME ppc-oracle-${model} COMMAND Python3::Interpreter
            ${MC_TESTS}/ppc/oracle.py
            --livadix $<TARGET_FILE:mc_ppc_oracle> --model ${model} --programs 16
            --work ${CMAKE_CURRENT_SOURCE_DIR}/../work/ppc-oracle-${model})
        set_tests_properties(ppc-oracle-${model} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endforeach()
endif()
# The first PCI Power Macintosh generation's chips (phase 6.8).
target_sources(mc_core PRIVATE hw/ppc/chips/pci.c hw/ppc/chips/grackle.c hw/ppc/chips/heathrow.c hw/ppc/chips/ltpc.c hw/ppc/chips/psx.c hw/ppc/chips/heathrow_ide.c hw/ppc/chips/sdram_spd.c hw/ppc/chips/bmac.c hw/ppc/chips/screamer.c hw/ppc/chips/bandit.c hw/ppc/chips/dbdma.c
    hw/ppc/chips/grand_central.c hw/ppc/chips/mesh.c hw/ppc/chips/awacs.c hw/ppc/chips/nvram.c
    hw/ppc/chips/hammerhead.c hw/ppc/chips/platinum.c hw/ppc/chips/control.c hw/ppc/chips/radacal.c
    hw/ppc/chips/planb.c hw/ppc/chips/ohci.c)
add_executable(mc_ppc_pci_tests ${MC_TESTS}/ppc/test_pci.c)
target_link_libraries(mc_ppc_pci_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_pci_tests)
add_test(NAME ppc-pci COMMAND mc_ppc_pci_tests)
# power-macintosh-7200.rom's POST on a test board of these chips, through its memory sizing.
add_test(NAME ppc-pci-probe COMMAND mc_ppc_pci_tests probe)
set_tests_properties(ppc-pci-probe PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# MPC106/Grackle programming contract (G3 bring-up, manual fixtures).
add_executable(mc_ppc_grackle_tests ${MC_TESTS}/ppc/test_grackle.c)
target_link_libraries(mc_ppc_grackle_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_grackle_tests)
add_test(NAME ppc-grackle COMMAND mc_ppc_grackle_tests)

add_executable(mc_ppc_heathrow_tests ${MC_TESTS}/ppc/test_heathrow.c)
target_link_libraries(mc_ppc_heathrow_tests PRIVATE mc_core)
mc_set_warnings(mc_ppc_heathrow_tests)
add_test(NAME ppc-heathrow COMMAND mc_ppc_heathrow_tests)
