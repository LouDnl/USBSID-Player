####
# USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
# command line playback, websites and for embedding on RP2040/RP2350.
#
# CMakeLists.txt
# This file is part of USBSID-Player (https://github.com/LouDnl/USBSID-Player)
# File author: LouD
#
# Copyright (c) 2026 LouD
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
####

cmake_minimum_required(VERSION 3.17)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

### Target is ${EXECUTABLE} (default usbsid), output file is usplayer (see OUTPUT_NAME below)
set(PROJECT_NAME usbsid)
set(EXECUTABLE ${PROJECT_NAME} CACHE STRING "EXECUTABLE")

### Player version, one line in .version beside this file
### Configure dependency, editing .version reruns CMake on the following build
set(US_VERSION_FILE ${CMAKE_CURRENT_LIST_DIR}/.version)
if(EXISTS ${US_VERSION_FILE})
  file(STRINGS ${US_VERSION_FILE} US_PLAYER_VERSION LIMIT_COUNT 1)
  string(STRIP "${US_PLAYER_VERSION}" US_PLAYER_VERSION)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${US_VERSION_FILE})
endif()
if("${US_PLAYER_VERSION}" STREQUAL "")
  message(WARNING "No version in ${US_VERSION_FILE}, the player reports 'unknown'")
  set(US_PLAYER_VERSION "unknown")
endif()
message(STATUS "USBSID-Player version: ${US_PLAYER_VERSION}")

### Target selection, mutually exclusive
set(DESKTOP 1 CACHE STRING "Build the desktop command line player")
set(EMBEDDED 0 CACHE STRING "Build for the RP2350 firmware")
### Browser player, a sub variant of DESKTOP (same emulation code, other SID backend)
### emcmake cmake -S . -B build-web -DWEB=1 or ./build.sh web, which also copies the result for the config tool
set(WEB 0 CACHE STRING "Build the WebAssembly player")
### Tests, desktop only
set(BUILD_TESTS 1 CACHE STRING "Build the test suite")
### Per cycle counters for the cost per chip. Off by default, plain increments on the hottest path
### See src/util/profile.h and temp/tools/dbg_cost.cpp
set(PROFILE 0 CACHE STRING "Build with the per cycle profiling counters")
### Warnings as errors
set(STRICT 1 CACHE STRING "Treat warnings as errors")
### Fail the configure when software audio cannot be built (releases and CI). See the reSIDfp block
set(REQUIRE_AUDIO 0 CACHE STRING "Refuse to configure without software audio")

### Use ymfm (lib/ymfm, YM3812) as FM/OPL engine, command line only
### Web and embedded builds keep Nuked. ./build.sh cli-ymfm configures it in its own build directory
set(FM_YMFM 0 CACHE STRING "Use ymfm instead of Nuked-OPL3-fast for FM/OPL (CLI only)")

### Compile the stock Commodore BASIC, KERNAL and character ROMs in
### With 0 tunes start on the stub KERNAL (src/mem/roms/stub_kernal.cpp), real images via usplayer_set_roms()
### BASIC programs without a SYS line need them
set(EMBED_ROMS 1 CACHE STRING "Compile the stock C64 ROM images in")

if(${WEB} EQUAL 1)
  set(DESKTOP 1)
  set(EMBEDDED 0)
  set(BUILD_TESTS 0) ### the suite is native only
endif()

if(${FM_YMFM} EQUAL 1 AND (${WEB} EQUAL 1 OR ${EMBEDDED} EQUAL 1))
  message(FATAL_ERROR "FM_YMFM=1 is for the command line player only, not WEB or EMBEDDED! Exiting...")
endif()

if(${EMBED_ROMS} EQUAL 0 AND ${EMBEDDED} EQUAL 1)
  message(FATAL_ERROR "EMBED_ROMS=0 is not supported for EMBEDDED! Exiting...")
endif()

if(${DESKTOP} EQUAL 1 AND ${EMBEDDED} EQUAL 1)
  message(FATAL_ERROR "DESKTOP and EMBEDDED are mutually exclusive! Exiting...")
endif()
if(${DESKTOP} EQUAL 0 AND ${EMBEDDED} EQUAL 0)
  message(FATAL_ERROR "Please set either DESKTOP or EMBEDDED to 1! Exiting...")
endif()

### Default to an optimised build, -O3 is required for the embedded player and used on desktop too
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
### NDEBUG as in the Release flags, reSIDfp asserts are debug checks and abort the wasm module
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O3 -g -DNDEBUG")
### No debug info in wasm, DWARF has to be downloaded
if(${WEB} EQUAL 1)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

project(${PROJECT_NAME} C CXX ASM)

### EMSCRIPTEN only exists after the toolchain file ran in project()
if(${WEB} EQUAL 1 AND NOT EMSCRIPTEN)
  message(FATAL_ERROR
    "WEB=1 needs the Emscripten toolchain: configure with 'emcmake cmake ...'. Exiting...")
endif()

### Common compile options
set(US_COMPILE_OPTS
  -Wall
  -Wextra
  -Wno-unused-function
  -fno-exceptions
  -fno-rtti
)
if(${STRICT} EQUAL 1)
  list(APPEND US_COMPILE_OPTS -Werror)
endif()

set(US_COMPILE_DEFS
  DESKTOP=${DESKTOP}
  EMBEDDED=${EMBEDDED}
  WEB=${WEB}
  US_PROFILE=${PROFILE}
  US_EMBED_ROMS=${EMBED_ROMS}
)

### reSIDfp, vendored in lib/residfp from the libsidplayfp release tarball
### System libresidfp is 1.0.0 and the source release is 1.2.1, CLI and browser must run the same SID engine
### One copy for every target needs no package on the four release platforms, wasm compiles from source anyway
### Two headers generated by autoconf upstream are made with configure_file (eight substitutions)
### Compiled from a patched copy in the build tree, lib/residfp is not touched
### Local changes are in patches/residfp/*.patch, see its README.md
### Copy directory is named by a hash of the patches and sources, nothing is deleted
set(US_RESIDFP_SRC ${CMAKE_CURRENT_LIST_DIR}/lib/residfp)
set(US_RESIDFP_GEN ${CMAKE_CURRENT_BINARY_DIR}/residfp-generated)
set(US_RESIDFP_DIR ${US_RESIDFP_SRC})

if(EXISTS ${US_RESIDFP_SRC}/siddefs-fp.h.in)
  file(GLOB US_RESIDFP_PATCHES ${CMAKE_CURRENT_LIST_DIR}/patches/residfp/*.patch)
  list(SORT US_RESIDFP_PATCHES)
  file(GLOB_RECURSE US_RESIDFP_UPSTREAM LIST_DIRECTORIES false
       RELATIVE ${US_RESIDFP_SRC} ${US_RESIDFP_SRC}/*)
  list(SORT US_RESIDFP_UPSTREAM)
  ### Reconfigure when a patch or upstream file changes
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${US_RESIDFP_PATCHES})
  set(US_RESIDFP_KEY "")
  foreach(f ${US_RESIDFP_UPSTREAM})
    file(SHA256 ${US_RESIDFP_SRC}/${f} h)
    string(APPEND US_RESIDFP_KEY "${f} ${h}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${US_RESIDFP_SRC}/${f})
  endforeach()
  foreach(p ${US_RESIDFP_PATCHES})
    file(SHA256 ${p} h)
    get_filename_component(n ${p} NAME)
    string(APPEND US_RESIDFP_KEY "patch ${n} ${h}\n")
  endforeach()
  string(SHA256 US_RESIDFP_HASH "${US_RESIDFP_KEY}")
  string(SUBSTRING ${US_RESIDFP_HASH} 0 12 US_RESIDFP_HASH)
  set(US_RESIDFP_DIR ${CMAKE_CURRENT_BINARY_DIR}/residfp-patched/${US_RESIDFP_HASH})

  if(NOT EXISTS ${US_RESIDFP_DIR}/.patched)
    file(MAKE_DIRECTORY ${US_RESIDFP_DIR})
    file(COPY ${US_RESIDFP_SRC}/ DESTINATION ${US_RESIDFP_DIR})
    find_program(US_PATCH_EXE patch)
    if(NOT US_PATCH_EXE)
      find_package(Git QUIET)
    endif()
    if(US_RESIDFP_PATCHES AND NOT US_PATCH_EXE AND NOT GIT_FOUND)
      message(FATAL_ERROR
        "reSIDfp: patches/residfp needs 'patch' or 'git' to apply. Exiting...")
    endif()
    foreach(p ${US_RESIDFP_PATCHES})
      if(US_PATCH_EXE)
        execute_process(
          COMMAND ${US_PATCH_EXE} -p1 -N -t -i ${p}
          WORKING_DIRECTORY ${US_RESIDFP_DIR}
          RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
      else()
        ### Stop git looking above the copy, patch paths would resolve against the repo root
        execute_process(
          COMMAND ${CMAKE_COMMAND} -E env
                  GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}/residfp-patched
                  ${GIT_EXECUTABLE} apply -p1 --whitespace=nowarn ${p}
          WORKING_DIRECTORY ${US_RESIDFP_DIR}
          RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
      endif()
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR
          "reSIDfp: ${p} does not apply to lib/residfp, refresh it "
          "(see patches/residfp/README.md).\n${out}${err}")
      endif()
    endforeach()
    file(WRITE ${US_RESIDFP_DIR}/.patched "${US_RESIDFP_KEY}")
  endif()
  list(LENGTH US_RESIDFP_PATCHES US_RESIDFP_NPATCHES)
  message(STATUS "reSIDfp: ${US_RESIDFP_NPATCHES} patch(es), build copy ${US_RESIDFP_DIR}")
endif()

if(EXISTS ${US_RESIDFP_DIR}/siddefs-fp.h.in)
  set(PACKAGE_VERSION "1.2.1")
  set(LIB_MAJOR 1)
  set(LIB_MINOR 2)
  set(LIB_LEVEL 1)
  ### gcc, clang and emcc have __builtin_expect, the reSIDfp hot paths use the branch hints
  set(HAVE_BUILTIN_EXPECT 1)
  set(RESIDFP_BRANCH_HINTS 1)
  set(RESIDFP_INLINING 1)
  set(RESIDFP_INLINE inline)

  file(MAKE_DIRECTORY ${US_RESIDFP_GEN}/residfp)
  configure_file(${US_RESIDFP_DIR}/siddefs-fp.h.in
                 ${US_RESIDFP_GEN}/siddefs-fp.h @ONLY)
  configure_file(${US_RESIDFP_DIR}/residfp/sidversion.h.in
                 ${US_RESIDFP_GEN}/residfp/sidversion.h @ONLY)

  file(GLOB US_RESIDFP_SOURCES
       ${US_RESIDFP_DIR}/*.cpp
       ${US_RESIDFP_DIR}/residfp/*.cpp
       ${US_RESIDFP_DIR}/resample/*.cpp)
  ### The tarball ships resample/test.cpp with its own main(), filtered out of the glob
  ### Stays filtered when re-vendoring
  list(FILTER US_RESIDFP_SOURCES EXCLUDE REGEX "/test[^/]*\\.cpp$")
  set(US_RESIDFP_INCLUDE_DIRS
      ${US_RESIDFP_DIR} ${US_RESIDFP_DIR}/residfp ${US_RESIDFP_GEN})
  set(US_HAVE_RESIDFP 1)
  message(STATUS "Software audio: vendored reSIDfp ${PACKAGE_VERSION}")
elseif(${REQUIRE_AUDIO} EQUAL 1)
  ### Without software audio a release has no --output=audio and --output=wav, only a STATUS line reports it
  ### Workflows ask for a hard error
  message(FATAL_ERROR
    "REQUIRE_AUDIO=1 but lib/residfp is empty: no ${US_RESIDFP_DIR}/siddefs-fp.h.in. "
    "The vendored reSIDfp sources are part of the repository; a checkout that "
    "does not have them cannot build the player that is shipped. Exiting...")
else()
  set(US_RESIDFP_SOURCES "")
  set(US_RESIDFP_INCLUDE_DIRS "")
  set(US_HAVE_RESIDFP 0)
  message(STATUS "Software audio: OFF, lib/residfp is empty")
endif()

### Nuked-OPL3-fast (tgies), bit-exact fork of Nuked OPL3 pinned to upstream 1.8 (commit cfedb09)
### FM/OPL side of a SID+FM tune. Three files (opl3.c, opl3.h, wf_rom.h), LGPL-2.1-or-later, vendored in lib/nukedopl
### CLI and browser run the same engine. Only built beside software audio, embedded sends FM writes to the real chip
set(US_NUKEDOPL_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/nukedopl)
### ymfm (Aaron Giles, BSD-3-Clause), opt in with FM_YMFM=1
### YM3812 model (FM-YAM), the YM3526 of the SFX Sound Expander is register compatible
### ymfm_opl.cpp includes ymfm_fm.ipp and defines Y8950 and YMF278B too (adpcm and pcm sources needed)
set(US_YMFM_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/ymfm)
set(US_YMFM_SOURCES
  ${US_YMFM_DIR}/ymfm_opl.cpp
  ${US_YMFM_DIR}/ymfm_adpcm.cpp
  ${US_YMFM_DIR}/ymfm_pcm.cpp)
if(${FM_YMFM} EQUAL 1)
  if(NOT US_HAVE_RESIDFP)
    message(FATAL_ERROR "FM_YMFM=1 needs software audio, but lib/residfp is empty! Exiting...")
  endif()
  if(NOT EXISTS ${US_YMFM_DIR}/ymfm_opl.cpp)
    message(FATAL_ERROR "FM_YMFM=1 but ${US_YMFM_DIR}/ymfm_opl.cpp is missing! Exiting...")
  endif()
  set(US_FM_SOURCES
    ${US_YMFM_SOURCES}
    ${CMAKE_CURRENT_LIST_DIR}/src/fm/opl_chip.cpp)
  set(US_FM_INCLUDE_DIRS
    ${US_YMFM_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src/fm)
  set(US_HAVE_FM 1)
  message(STATUS "FM/OPL: vendored ymfm YM3812 (lib/ymfm, FM_YMFM=1)")
elseif(US_HAVE_RESIDFP AND EXISTS ${US_NUKEDOPL_DIR}/opl3.c)
  set(US_FM_SOURCES
    ${US_NUKEDOPL_DIR}/opl3.c
    ${CMAKE_CURRENT_LIST_DIR}/src/fm/opl_chip.cpp)
  set(US_FM_INCLUDE_DIRS
    ${US_NUKEDOPL_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src/fm)
  set(US_HAVE_FM 1)
  message(STATUS "FM/OPL: vendored Nuked-OPL3-fast 1.8-fast.3")
else()
  set(US_FM_SOURCES "")
  set(US_FM_INCLUDE_DIRS "")
  set(US_HAVE_FM 0)
endif()

### RtMidi, output for ASID compatible devices
### Vendored: 1 source file, 2 headers and the license file from https://github.com/thestk/rtmidi
set(US_RTMIDI_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/rtmidi)
if(EXISTS ${US_RTMIDI_DIR}/RtMidi.cpp)
  set(US_RTMIDI_SOURCES
    ${US_RTMIDI_DIR}/RtMidi.cpp)
  set(US_RTMIDI_INCLUDE_DIR
    ${US_RTMIDI_DIR})
  set(US_HAVE_RTMIDI 1)
else()
  set(US_RTMIDI_INCLUDE_DIR "")
  set(US_RTMIDI_SOURCES "")
  set(US_HAVE_RTMIDI 0)
endif()

### Core library with everything except the frontends. Tests, CLI and embedded API link the same objects
set(US_CORE_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/src/api/usplayer.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_voice3.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/core/bus.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/core/machine.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/cia/mos6526.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/mos6510.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/io/keyboard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/util/logging.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/mem/mmu.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/mem/mos906114_pla.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/mem/ram.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/driver/psiddrv_install.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/driver/reloc65.c
  ${CMAKE_CURRENT_LIST_DIR}/src/file/prgfile.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/file/sidfile.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/file/songlengths.cpp
  ### Firmware hook pointers in every build, web reaches for the clock without the embedded backend
  ${CMAKE_CURRENT_LIST_DIR}/src/sid/firmware_hooks.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/player/pacing.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/player/player.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/sid/mos6581_8580.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/vic/mos6569.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/mem/roms/stub_kernal.cpp
)
if(${EMBED_ROMS} EQUAL 1)
  list(APPEND US_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/mem/roms/rom_data.cpp)
  message(STATUS "C64 ROMs: stock images compiled in")
else()
  message(STATUS "C64 ROMs: none compiled in, stub KERNAL until supplied at runtime")
endif()

### SID backend the C API is compiled against
### Embedded binds to firmware bus functions via weak symbols, an undefined weak function is not reliably null under wasm-ld
### Web leaves it out, desktop takes both (tests use the web ring natively)
if(${WEB} EQUAL 1)
  list(APPEND US_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_web.cpp)
  ### Software audio in the browser, same backend as the CLI compiled to wasm with reSIDfp
  ### The write ring above stays, a page can drive hardware, make sound or both (chosen at runtime)
  if(US_HAVE_RESIDFP)
    list(APPEND US_CORE_SOURCES
      ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_residfp.cpp
      ${US_RESIDFP_SOURCES}
      ${US_FM_SOURCES})
  endif()
else()
  list(APPEND US_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_embedded.cpp)
  if(${DESKTOP} EQUAL 1)
    list(APPEND US_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_web.cpp)
  endif()
endif()

set(US_CORE_INCLUDE_DIRS
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${CMAKE_CURRENT_LIST_DIR}/src/api
  ${CMAKE_CURRENT_LIST_DIR}/src/core
  ${CMAKE_CURRENT_LIST_DIR}/src/cia
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu
  ${CMAKE_CURRENT_LIST_DIR}/src/io
  ${CMAKE_CURRENT_LIST_DIR}/src/mem
  ${CMAKE_CURRENT_LIST_DIR}/src/mem/roms
  ${CMAKE_CURRENT_LIST_DIR}/src/driver
  ${CMAKE_CURRENT_LIST_DIR}/src/file
  ${CMAKE_CURRENT_LIST_DIR}/src/player
  ${CMAKE_CURRENT_LIST_DIR}/src/sid
  ${CMAKE_CURRENT_LIST_DIR}/src/util
  ${CMAKE_CURRENT_LIST_DIR}/src/vic
)

if(${WEB} EQUAL 1 AND US_HAVE_RESIDFP)
  list(APPEND US_CORE_INCLUDE_DIRS ${US_RESIDFP_INCLUDE_DIRS})
endif()
### FM headers are needed wherever sid_residfp.h is included (the CLI on desktop)
if(US_HAVE_FM)
  list(APPEND US_CORE_INCLUDE_DIRS ${US_FM_INCLUDE_DIRS})
endif()
### The RtMidi headers
if(US_HAVE_RTMIDI)
  list(APPEND US_CORE_INCLUDE_DIRS ${US_RTMIDI_INCLUDE_DIR})
endif()

### Playback session for interactive frontends (src/host/session.h), runs an engine thread (not in the browser build)
if(${DESKTOP} EQUAL 1 AND ${WEB} EQUAL 0)
  list(APPEND US_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/host/session.cpp)
  list(APPEND US_CORE_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/host)
endif()

add_library(usbsidcore STATIC ${US_CORE_SOURCES})
target_include_directories(usbsidcore PUBLIC ${US_CORE_INCLUDE_DIRS})
if(${DESKTOP} EQUAL 1 AND ${WEB} EQUAL 0)
  find_package(Threads REQUIRED)
  target_link_libraries(usbsidcore PUBLIC Threads::Threads)
endif()
### -fno-rtti is C++ only, reloc65.c is third party C with its own warning set
target_compile_options(usbsidcore PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:${US_COMPILE_OPTS}>)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/driver/reloc65.c
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-parameter")
### reSIDfp is vendored upstream code, own warning set like reloc65.c
### -Werror makes a re-vendor a porting job
### clang objects to struct/class tag mismatches, only relevant for the Microsoft C++ ABI
if(US_HAVE_FM)
  ### Nuked OPL3 is upstream C, own warning set like reloc65.c and reSIDfp
  set_source_files_properties(${US_NUKEDOPL_DIR}/opl3.c PROPERTIES COMPILE_OPTIONS "-w")
  ### ymfm is upstream C++, own warning set
  set_source_files_properties(${US_YMFM_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(US_HAVE_RESIDFP)
  set_source_files_properties(${US_RESIDFP_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-fexceptions")
  ### Filter tables on one thread in every build (USP_NO_THREADS vendor patch in FilterModelConfig*.cpp)
  ### wasm has no std::thread without -pthread (needs SharedArrayBuffer with COOP/COEP)
  ### The six native table threads share one dither noise sequence (FilterModelConfig::rnd) and race on it
  ### One thread costs a few ms, once per process
  set_source_files_properties(${US_RESIDFP_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "USP_NO_THREADS=1")
endif()
target_compile_definitions(usbsidcore PUBLIC ${US_COMPILE_DEFS})
### opl_chip.h is included from the CLI target too, must be PUBLIC
target_compile_definitions(usbsidcore PUBLIC US_FM_YMFM=${FM_YMFM})

### Desktop player. The driver it builds against is set below
### lib/driver by default and always in CI, a local working copy with US_DRIVER_SIBLING
if(${DESKTOP} EQUAL 1 AND ${WEB} EQUAL 0)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)
  set(US_LINK_LIBS PkgConfig::libusb)
  ### MSYS2/MinGW: pkg-config gives libusb-1.0.dll.a (DLL import library), name the static archive
  if(MINGW)
    find_library(US_LIBUSB_STATIC NAMES libusb-1.0.a HINTS ${libusb_LIBRARY_DIRS})
    if(US_LIBUSB_STATIC)
      set(US_LINK_LIBS ${US_LIBUSB_STATIC})
      include_directories(${libusb_INCLUDE_DIRS})
    endif()
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(udev REQUIRED IMPORTED_TARGET libudev)
    list(APPEND US_LINK_LIBS PkgConfig::udev)
  endif()

  ### Which USBSID driver to build against
  ### Neither knob has to be set, both empty builds against lib/driver
  ### Locally lib/driver is a symlink to the driver working copy, in CI the workflows check the driver out into it
  ###   US_DRIVER_SIBLING  driver working copy, overrides lib/driver (ignored in GitHub Actions)
  ###   US_DRIVER_DIR      build against exactly this directory, wins over everything incl. CI
  ### GITHUB_ACTIONS is true on every Actions runner, that is the signal for ignoring US_DRIVER_SIBLING
  ### STRING and not PATH, the PATH cache type makes a relative value absolute against the cwd of cmake
  ### As STRING the value arrives as written and is resolved below against this file
  set(US_DRIVER_DIR "" CACHE STRING
    "Directory holding USBSID.cpp. Overrides everything, including in CI. Empty to work it out")
  set(US_DRIVER_SIBLING "" CACHE STRING
    "Driver working copy to use instead of lib/driver. Ignored in GitHub Actions. Empty to use lib/driver")

  if(NOT US_DRIVER_DIR STREQUAL "")
    get_filename_component(US_DRIVER_DIR
      ${US_DRIVER_DIR} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_LIST_DIR})
    set(US_DRIVER_WHY "asked for with -DUS_DRIVER_DIR")
  elseif(DEFINED ENV{GITHUB_ACTIONS})
    set(US_DRIVER_WHY "GitHub Actions, lib/driver as the workflow checked it out")
  elseif(NOT US_DRIVER_SIBLING STREQUAL "")
    ### A relative path is resolved against this file, -DUS_DRIVER_SIBLING=../driver-repo/src works from anywhere
    ### ABSOLUTE and not REALPATH, a symlink path is kept as given
    get_filename_component(US_DRIVER_SIBLING_ABS
      ${US_DRIVER_SIBLING} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_LIST_DIR})
    if(EXISTS ${US_DRIVER_SIBLING_ABS}/USBSID.cpp)
      set(US_DRIVER_DIR ${US_DRIVER_SIBLING_ABS})
      set(US_DRIVER_WHY "US_DRIVER_SIBLING")
    else()
      ### Named and not there, stop. Falling back to lib/driver would build the wrong driver silently
      message(FATAL_ERROR
        "US_DRIVER_SIBLING was given as\n"
        "    ${US_DRIVER_SIBLING}\n"
        "  which resolves to\n"
        "    ${US_DRIVER_SIBLING_ABS}\n"
        "  and there is no USBSID.cpp there. Fix the path, or leave\n"
        "  US_DRIVER_SIBLING empty to build against lib/driver.")
    endif()
  else()
    set(US_DRIVER_WHY "no US_DRIVER_SIBLING, lib/driver")
  endif()

  ### Fall back to lib/driver wherever its sources sit
  ### A driver checkout gives lib/driver/src/ (workflows), a symlink straight at src/ gives lib/driver/ (local)
  ### Take whichever is present
  if(US_DRIVER_DIR STREQUAL "")
    set(US_DRIVER_ROOT ${CMAKE_CURRENT_LIST_DIR}/lib/driver)
    if(EXISTS ${US_DRIVER_ROOT}/src/USBSID.cpp)
      set(US_DRIVER_DIR ${US_DRIVER_ROOT}/src)
    elseif(EXISTS ${US_DRIVER_ROOT}/USBSID.cpp)
      set(US_DRIVER_DIR ${US_DRIVER_ROOT})
    endif()
  endif()

  if(US_DRIVER_DIR STREQUAL "" OR NOT EXISTS ${US_DRIVER_DIR}/USBSID.cpp)
    message(FATAL_ERROR
      "No USBSID driver found (${US_DRIVER_WHY}).\n"
      "  Looked for USBSID.cpp in, in order:\n"
      "    -DUS_DRIVER_DIR, if given\n"
      "    -DUS_DRIVER_SIBLING, if given and not in GitHub Actions\n"
      "    ${CMAKE_CURRENT_LIST_DIR}/lib/driver/src\n"
      "    ${CMAKE_CURRENT_LIST_DIR}/lib/driver\n"
      "  Locally, lib/driver is a symlink to the driver working copy:\n"
      "    ln -s ../../driver-repo/src lib/driver")
  endif()
  ### The board backend drives boards through USBSID_Manager, carried beside USBSID.cpp since multiboard support
  ### An older driver checkout has USBSID.cpp only, stop at configure and not at link time
  if(NOT EXISTS ${US_DRIVER_DIR}/USBSID_Manager.cpp)
    message(FATAL_ERROR
      "The USBSID driver at\n"
      "    ${US_DRIVER_DIR}\n"
      "  has no USBSID_Manager.cpp. Update the driver checkout to one with\n"
      "  multiboard support.")
  endif()
  message(STATUS "USBSID driver sources: ${US_DRIVER_DIR} (${US_DRIVER_WHY})")

  ### Software audio: reSIDfp for synthesis, miniaudio for the device
  ### reSIDfp is optional, the release matrix has four platforms incl. MSYS2/UCRT64
  ### Without it the binary is the same and --output=audio|wav says it was not compiled in
  ### miniaudio is vendored (lib/miniaudio), public domain or MIT-0, fits GPLv2
  set(US_SOFTSID_SOURCES "")
  if(US_HAVE_RESIDFP)
    set(US_SOFTSID_SOURCES
      ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_residfp.cpp
      ${CMAKE_CURRENT_LIST_DIR}/src/host/audio_out.cpp
      ${CMAKE_CURRENT_LIST_DIR}/src/host/session_audio.cpp
      ${US_RESIDFP_SOURCES}
      ${US_FM_SOURCES}
    )
  endif()

  ### Network SID Device client backend (--output netdevice), BSD sockets on Linux/macOS, Winsock2 on Windows (src/sid/net_compat.h)
  ### See US_HAVE_NETDEVICE below and its guards in main_cli.cpp
  set(US_HAVE_NETDEVICE 1)
  set(US_NETDEVICE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_netdevice.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/host/session_net.cpp)

  ### RtMidi backend per platform (WINDOWS_COMPILE on Windows, UNIX_COMPILE on Linux and macOS, only read by RtMidi.cpp)
  ### Midi ASID backend (--output asid): ALSA on Linux, CoreMIDI on macOS, WinMM on Windows. Set on the target below
  if(US_HAVE_RTMIDI)
    list(APPEND US_RTMIDI_SOURCES
      ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_asid.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      pkg_check_modules(alsa REQUIRED IMPORTED_TARGET alsa)
      list(APPEND US_LINK_LIBS PkgConfig::alsa)
    endif()
  endif()

  add_executable(${EXECUTABLE}
    ${CMAKE_CURRENT_LIST_DIR}/src/host/main_cli.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_usbsid.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/host/session_board.cpp
    ${US_NETDEVICE_SOURCES}
    ${US_DRIVER_DIR}/USBSID.cpp
    ${US_DRIVER_DIR}/USBSID_Manager.cpp
    ${US_SOFTSID_SOURCES}
    ${US_RTMIDI_SOURCES}
  )
  target_link_libraries(${EXECUTABLE} PRIVATE usbsidcore ${US_LINK_LIBS})
  target_include_directories(${EXECUTABLE} PRIVATE
    ${US_DRIVER_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/lib/miniaudio
    ${US_RESIDFP_INCLUDE_DIRS}
    ${US_FM_INCLUDE_DIRS}
    ${US_RTMIDI_INCLUDE_DIR}
    )
  target_compile_definitions(${EXECUTABLE} PRIVATE US_SOFTSID=${US_HAVE_RESIDFP})
  target_compile_definitions(${EXECUTABLE} PRIVATE US_HAVE_NETDEVICE=${US_HAVE_NETDEVICE})
  target_compile_definitions(${EXECUTABLE} PRIVATE US_HAVE_RTMIDI=${US_HAVE_RTMIDI})
  if(US_HAVE_RTMIDI)
    if(WIN32)
      target_compile_definitions(${EXECUTABLE} PRIVATE WINDOWS_COMPILE)
    else()
      target_compile_definitions(${EXECUTABLE} PRIVATE UNIX_COMPILE)
    endif()
    if(APPLE)
      target_compile_definitions(${EXECUTABLE} PRIVATE __MACOSX_CORE__)
      target_link_libraries(${EXECUTABLE} PRIVATE
        "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
    elseif(WIN32)
      target_compile_definitions(${EXECUTABLE} PRIVATE __WINDOWS_MM__)
      target_link_libraries(${EXECUTABLE} PRIVATE winmm)
    else()
      target_compile_definitions(${EXECUTABLE} PRIVATE __LINUX_ALSA__)
    endif()
  endif()
  target_compile_definitions(${EXECUTABLE} PRIVATE US_PLAYER_VERSION="${US_PLAYER_VERSION}")
  ### std::thread (sid_netdevice.cpp sender thread) needs this on every platform
  find_package(Threads REQUIRED)
  target_link_libraries(${EXECUTABLE} PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(${EXECUTABLE} PRIVATE ws2_32)
  endif()
  ### MSYS2/MinGW: link libstdc++, libgcc, winpthread and libusb (libusb-1.0.a) statically
  ### usplayer.exe then starts without the MSYS2 DLLs (a missing DLL exits 0xC0000135 before main)
  if(MINGW)
    target_link_options(${EXECUTABLE} PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  ### miniaudio needs these on Linux and they are harmless elsewhere
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND US_HAVE_RESIDFP)
    target_link_libraries(${EXECUTABLE} PRIVATE ${CMAKE_DL_LIBS} m)
  endif()
  ### the driver is third party and predates these warning settings
  target_compile_options(${EXECUTABLE} PRIVATE -Wall -fno-rtti)
  ### Only the file name is usplayer (usplayer.exe), the target stays ${EXECUTABLE}
  ### A custom -DEXECUTABLE (ymfm build) keeps its name
  if(EXECUTABLE STREQUAL PROJECT_NAME)
    set_target_properties(${EXECUTABLE} PROPERTIES OUTPUT_NAME usplayer)
  endif()
endif()

### Browser player: one wasm module with the flat C ABI in src/host/web_api.cpp, driven by the page in web/
if(${WEB} EQUAL 1)
  add_executable(${EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/src/host/web_api.cpp)
  target_link_libraries(${EXECUTABLE} PRIVATE usbsidcore)
  target_compile_options(${EXECUTABLE} PRIVATE ${US_COMPILE_OPTS})

  set(US_WEB_EXPORTS
    _malloc _free
    _usp_alloc _usp_free
    _usp_load_sidtune _usp_load_prg
    _usp_init_sidplayer _usp_start _usp_step _usp_stop
    _usp_next_subtune _usp_prev_subtune _usp_pause
    _usp_force_socket_two _usp_key_runstop _usp_type
    _usp_set_sid_config
    _usp_audio_configure
    _usp_audio_enabled
    _usp_audio_available
    _usp_audio_take
    _usp_audio_discard
    _usp_audio_scope _usp_audio_scope_voices _usp_audio_scope_take
    _usp_audio_set_stereo _usp_audio_channels
    _usp_audio_set_panning _usp_audio_pan
    _usp_audio_set_filter
    _usp_audio_clipped
    _usp_audio_fm_writes
    _usp_audio_set_sid_volume _usp_audio_sid_volume
    _usp_audio_set_fm_volume _usp_audio_fm_volume
    _usp_set_voice_mute
    _usp_voice_mute
    _usp_set_chip_mute
    _usp_chip_mute
    _usp_playtime_ms
    _usp_song_md5
    _usp_songlength_ms
    _usp_songlength_count
    _usp_is_playing _usp_is_paused _usp_is_prg _usp_is_pal
    _usp_clock_id _usp_clock_hz _usp_refresh_hz
    _usp_song _usp_songs _usp_frames _usp_sid_writes
    _usp_tune_name _usp_tune_author _usp_tune_released
    _usp_sid_count _usp_sid_addr _usp_sid_pan
    _usp_pan_layout _usp_pan_mode
    _usp_has_fm_opl _usp_has_embedded_songlengths _usp_embedded_songlength_ms
    _usp_benchmark
    _usp_irq_sources _usp_start_mode _usp_driver_address
    _usp_read_memory _usp_read_memory_block _usp_cia_latch _usp_sid_register
    _usp_audio_render _usp_audio_rendering
    _usp_audio_set_speed _usp_audio_speed
    _usbsid_web_ring_ptr _usbsid_web_ring_entries
    _usbsid_web_ring_head _usbsid_web_ring_tail _usbsid_web_ring_set_tail
    _usbsid_web_flush_count _usbsid_web_drop_count _usbsid_web_write_count
    _usbsid_web_reset_count _usbsid_web_get_clockrate
  )
  list(JOIN US_WEB_EXPORTS "','" US_WEB_EXPORTS_JOINED)

  set(US_WEB_LINK_OPTS
    "--no-entry"                  ### there is no main(), the page calls in
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=USBSIDPlayer"
    "-sENVIRONMENT=web,worker,node"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINVOKE_RUN=0"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','UTF8ToString']"
    "-sEXPORTED_FUNCTIONS=['${US_WEB_EXPORTS_JOINED}']"
  )

  set_target_properties(${EXECUTABLE} PROPERTIES SUFFIX ".js")
  target_link_options(${EXECUTABLE} PRIVATE ${US_WEB_LINK_OPTS})

  ### Same as an ES module for the worker
  ### A module worker cannot importScripts the classic script, build both (same objects, other wrapper)
  add_executable(${EXECUTABLE}_esm ${CMAKE_CURRENT_LIST_DIR}/src/host/web_api.cpp)
  target_link_libraries(${EXECUTABLE}_esm PRIVATE usbsidcore)
  target_compile_options(${EXECUTABLE}_esm PRIVATE ${US_COMPILE_OPTS})
  set_target_properties(${EXECUTABLE}_esm PROPERTIES
    OUTPUT_NAME "${EXECUTABLE}" SUFFIX ".mjs")
  target_link_options(${EXECUTABLE}_esm PRIVATE ${US_WEB_LINK_OPTS} "-sEXPORT_ES6=1")

  ### Both targets emit the same ${EXECUTABLE}.wasm, linking at once gives two writers on one file
  ### A parallel incremental rebuild can produce an empty (eight byte) module, order them
  ### web_smoke.mjs --esm checks the bytes
  add_dependencies(${EXECUTABLE}_esm ${EXECUTABLE})

  ### Put the artefacts beside the page that loads them
  ### web/ is self contained, demo.html and bench.html load ./usbsid.js
  ### The three copied files are generated, .gitignore drops them
  set(US_WEB_OUT ${CMAKE_CURRENT_LIST_DIR}/web)
  add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${US_WEB_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:${EXECUTABLE}> ${US_WEB_OUT}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE_DIR:${EXECUTABLE}>/${EXECUTABLE}.wasm ${US_WEB_OUT}/
    COMMENT "Copying ${EXECUTABLE}.js and ${EXECUTABLE}.wasm into web/")
  ### The ES module goes out under both names
  ### nginx has no mime.types entry for .mjs and serves application/octet-stream, import() refuses it
  ### The same bytes as .js are served as JavaScript everywhere, the module finds its wasm relative to its URL
  add_custom_command(TARGET ${EXECUTABLE}_esm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:${EXECUTABLE}_esm> ${US_WEB_OUT}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:${EXECUTABLE}_esm> ${US_WEB_OUT}/${EXECUTABLE}.esm.js
    COMMENT "Copying ${EXECUTABLE}.mjs into web/, and again as .esm.js")
endif()

### Tests
if(${DESKTOP} EQUAL 1 AND ${BUILD_TESTS} EQUAL 1)
  enable_testing()

  set(US_TEST_NAMES bus ram cpu mmu cia vic sid player embedded prg keyboard web roms session)
  list(APPEND US_TEST_NAMES netdevice asid)

  set(US_TEST_SOURCES "")
  foreach(t ${US_TEST_NAMES})
    list(APPEND US_TEST_SOURCES ${CMAKE_CURRENT_LIST_DIR}/tests/test_${t}.cpp)
  endforeach()

  ### One standalone executable per component, registered with CTest
  foreach(t ${US_TEST_NAMES})
    add_executable(test_${t} ${CMAKE_CURRENT_LIST_DIR}/tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE usbsidcore)
    target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests)
    target_compile_options(test_${t} PRIVATE ${US_COMPILE_OPTS})
    target_compile_definitions(test_${t} PRIVATE
      US_TEST_STANDALONE=1
      US_ASSET_DIR="${CMAKE_CURRENT_LIST_DIR}/assets")
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()

  ### The network backend is CLI-only and not in usbsidcore
  target_sources(test_netdevice PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_netdevice.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(test_netdevice PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(test_netdevice PRIVATE ws2_32)
  endif()

  ### ASID backend is CLI-only and not in usbsidcore, the test supplies the RtMidi symbols (recording sink)
  if(US_HAVE_RTMIDI)
    foreach(t asid)
      target_sources(test_${t} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_asid.cpp)
      target_compile_definitions(test_${t} PRIVATE US_TEST_HAVE_ASID=1)
    endforeach()
  endif()

  ### Aggregate runner with subcommands
  add_executable(usplayer-test
    ${CMAKE_CURRENT_LIST_DIR}/tests/usbsid_test.cpp
    ${US_TEST_SOURCES}
  )
  target_link_libraries(usplayer-test PRIVATE usbsidcore)
  target_include_directories(usplayer-test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests)
  target_compile_options(usplayer-test PRIVATE ${US_COMPILE_OPTS})
  target_compile_definitions(usplayer-test PRIVATE
    US_ASSET_DIR="${CMAKE_CURRENT_LIST_DIR}/assets")
  target_sources(usplayer-test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_netdevice.cpp)
  if(US_HAVE_RTMIDI)
    target_sources(usplayer-test PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_asid.cpp)
    target_compile_definitions(usplayer-test PRIVATE US_TEST_HAVE_ASID=1)
  endif()
  target_link_libraries(usplayer-test PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(usplayer-test PRIVATE ws2_32)
  endif()
endif()
