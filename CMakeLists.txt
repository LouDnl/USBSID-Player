####
# USBSID-Player aims to be a command line SID file player that is also
# suited for embedding where both implementations target use
# with USBSID-Pico. USBSID-Pico is a RPi Pico/PicoW (RP2040) &
# Pico2/Pico2W (RP2350) based board for interfacing one or two
# MOS SID chips and/or hardware SID emulators over (WEB)USB with
# your computer, phone or ASID supporting player
#
# CMakeLists.txt
# This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
# File author: LouD
#
# Copyright (c) 2025 ~ 2026 LouD
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 2.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
####

### Cmake minimum version
cmake_minimum_required(VERSION 3.17)

### Cmake compiler standard versions
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

### CMake stuff for ZED
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

### Project magic sprinkles
set(PROJECT_NAME usbsid)
set(EXECUTABLE ${PROJECT_NAME} CACHE STRING "EXECUTABLE")

# enable or disable desktop build
set(DESKTOP 1 CACHE STRING "DESKTOP")
# enable or disable embedded build
set(EMBEDDED 0 CACHE STRING "EMBEDDED")
# enable or disable the (re)build of psiddrv.h
set(BUILDPSIDDRV 0 CACHE STRING "BUILDPSIDDRV")
# enable or disable residfp for desktop only!
set(USE_RESIDFP 0 CACHE STRING "USE_RESIDFP")
# enable or disable radare2 debugging support
set(RADARE2 1 CACHE STRING "RADARE2")
# enable or disable web (Emscripten/WASM) build.
# WEB is a sub-variant of DESKTOP: it reuses the portable DESKTOP code paths
# (posix sleep, stdout logging, malloc'd RAM) but swaps the libusb USBSID driver
# for a JS-side transport (see src/web/). Enabling WEB forces DESKTOP=1.
set(WEB 0 CACHE STRING "WEB")

if(${WEB} EQUAL 1)
  set(DESKTOP 1)
  set(EMBEDDED 0)
  set(RADARE2 0)
endif()

if(${DESKTOP} EQUAL 1 AND ${EMBEDDED} EQUAL 1)
  message(FATAL_ERROR "DESKTOP and EMBEDDED are mutually exclusive! Exiting...")
endif()

if(${EMBEDDED} EQUAL 1 AND ${USE_RESIDFP} EQUAL 1)
  message(FATAL_ERROR "USE_RESIDFP cannot be used with EMBEDDED! Exiting...")
endif()

if(${DESKTOP} EQUAL 1 AND ${WEB} EQUAL 1 AND ${USE_RESIDFP} EQUAL 1)
  message(FATAL_ERROR "USE_RESIDFP cannot be used with WEB! Exiting...")
endif()

if(${DESKTOP} EQUAL 0 AND ${EMBEDDED} EQUAL 0)
  message(FATAL_ERROR "Please set either DESKTOP or EMBEDDED to 1! Exiting...")
endif()

### Compiler flags and options
if (WEB)
  ### Lean set for the Emscripten/clang wasm backend. -save-temps / -fverbose-asm
  ### break the wasm backend (it reassembles the .s), and -Wno-maybe-uninitialized
  ### is a gcc-only flag. -O2 keeps SID timing sane (player needs high opt).
  set(COMPILE_OPTS PRIVATE
    -O2
    -Wno-format
    -Wno-unused-function
  )
else (WEB)
set(COMPILE_OPTS PRIVATE
  -g3
  -Og
  # -Wall
  # -Wunused
  # -Werror
  -fno-omit-frame-pointer
  -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
  -Wno-unused-function # extra check to verify no unused lingering code is present
  -Wno-maybe-uninitialized
  -save-temps
  -fverbose-asm
)
endif (WEB)

set(COMPILE_OPTS ${COMPILE_OPTS}
  -DDESKTOP=${DESKTOP}
  -DUSE_RESIDFP=${USE_RESIDFP}
  )
if (WEB)
  set(COMPILE_OPTS ${COMPILE_OPTS}
  -DWEB=${WEB}
  )
endif (WEB)
if (EMBEDDED)
  set(COMPILE_OPTS ${COMPILE_OPTS}
  -DEMBEDDED=${EMBEDDED}
  )
  add_compile_definitions(ONBOARD_SIDPLAYER=1)
endif (EMBEDDED)

# Run the project command
project(${PROJECT_NAME} C CXX ASM)

# EMSCRIPTEN is defined by the Emscripten toolchain during project(), so this
# guard must live after project().
if(${WEB} EQUAL 1 AND NOT EMSCRIPTEN)
  message(FATAL_ERROR "WEB=1 requires the Emscripten toolchain. Configure with 'emcmake cmake ...'. Exiting...")
endif()

if (${DESKTOP} EQUAL 1 AND ${USE_RESIDFP} EQUAL 1 AND ${WEB} EQUAL 0)
  # The autotools build produces libtool objects under lib/libresidfp/src/.libs/.
  # Link the whole archive: the residfp.o wrapper alone is missing SID,
  # WaveformGenerator, EnvelopeGenerator, State, resamplers, etc.
  set(RESIDFP_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/libresidfp/src/.libs/libresidfp.a)

  add_custom_command(
    OUTPUT ${RESIDFP_LIB}
    COMMAND $(MAKE) all
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib/libresidfp/
  )

  add_custom_target(
    BUILDRESID ALL
    DEPENDS ${RESIDFP_LIB}
  )

  # add_custom_target(BUILDRESID ALL
  #   COMMAND $(MAKE) all
  #   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib/libresidfp/
  # )

  # add_library(RESIDFP STATIC IMPORTED)
  # set_target_properties(RESIDFP
  #   IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/lib/libresidfp/src/residfp/libresidfp_la-residfp.o # Make sure to use absolute path here
  #   INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/lib/libresidfp/src/residfp/
  #   INTERFACE_COMPILE_DEFINITIONS "USING_RESIDFP;RESIDFP_STATIC"
  # )
  # add_dependencies(RESIDFP BUILDRESID) # So that anyone linking against RESIDFP causes BUILDRESID to build first

  # target_link_libraries(${EXECUTABLE} PRIVATE RESIDFP)
endif()

if (DESKTOP AND NOT WEB)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

  set(TARGET_LL PkgConfig::libusb)

  ### Linux ONLY (Exclude macOS/Darwin)
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(udev REQUIRED IMPORTED_TARGET libudev)
    list(APPEND TARGET_LL PkgConfig::udev)
  endif ()

  ### macOS ONLY
  if (APPLE)
    # libusb on macOS often requires these native frameworks
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    list(APPEND TARGET_LL ${IOKIT_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  endif ()

  if (WIN32)
    # PkgConfig::libusb is already in TARGET_LL
  endif ()

endif (DESKTOP AND NOT WEB)

### Source files to compile
set(SOURCEFILES
  ${CMAKE_CURRENT_LIST_DIR}/src/usplayer.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/vsidpsid.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/microsid.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/prgrunner.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/emulation.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6510_cpu.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6526_cia.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6560_6561_vic.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6581_8580_sid.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos906114_pla.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mmu.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/util/timer.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/util/wrappers.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/psid/sidfile.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/psiddrv/psid.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/psiddrv/reloc65.c
  #${WIN32_SRC}
)
### Header folders to include
set(TARGET_INCLUDE_DIRS PRIVATE
  .
  src
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${CMAKE_CURRENT_LIST_DIR}/src/c64
  ${CMAKE_CURRENT_LIST_DIR}/src/util
  ${CMAKE_CURRENT_LIST_DIR}/src/roms
  ${CMAKE_CURRENT_LIST_DIR}/src/psid
  ${CMAKE_CURRENT_LIST_DIR}/src/psiddrv
)

if (DESKTOP AND NOT WEB)
  if (USE_RESIDFP)
  #   set(SOURCEFILES
  #     ${SOURCEFILES}
  #     ${CMAKE_CURRENT_LIST_DIR}/lib/libresidfp
  #   )
    set(TARGET_INCLUDE_DIRS
      ${TARGET_INCLUDE_DIRS}
      ${CMAKE_CURRENT_LIST_DIR}/lib/libresidfp/src/residfp/
    )
    # list(APPEND TARGET_LL ${CMAKE_CURRENT_SOURCE_DIR}/lib/libresidfp/src/residfp/libresidfp_la-residfp.o)
    # target_link_libraries(APPEND TARGET_LL ${CMAKE_CURRENT_SOURCE_DIR}/lib/libresidfp/src/residfp/libresidfp_la-residfp.o)
  endif (USE_RESIDFP)
  set(TARGET_INCLUDE_DIRS ${TARGET_INCLUDE_DIRS}
    # ${WIN32_INC}
    /usr/local/lib
    /usr/local/include
    /usr/lib
    /usr/include
  )
endif (DESKTOP AND NOT WEB)

## Workaround for github actions failing
## Needs download and compilation of XA for this part to work
if (BUILDPSIDDRV)
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_custom_command(
    # OUTPUT psiddrv.h psidextdrv.h
    OUTPUT psiddrv.h
    COMMAND $(MAKE) all
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/psiddrv/
  )
  add_custom_target(
    RSIDDriver ALL
    # DEPENDS psiddrv.h psidextdrv.h
    DEPENDS psiddrv.h
  )
  endif ()
endif (BUILDPSIDDRV)

if (DESKTOP AND NOT WEB)
  set(SOURCEFILES
    ${SOURCEFILES}
    ${CMAKE_CURRENT_LIST_DIR}/lib/driver/src/USBSID.cpp
    # ${CMAKE_CURRENT_LIST_DIR}/src/midi/RtMidi.cpp
    # ${CMAKE_CURRENT_LIST_DIR}/src/midi/asid.cpp
    )
    set(TARGET_INCLUDE_DIRS
    ${TARGET_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/lib/driver/src
    # ${CMAKE_CURRENT_LIST_DIR}/lib/midi
  )
endif (DESKTOP AND NOT WEB)

if (WEB)
  ### Web build: JS-side transport replaces the libusb driver.
  ### src/web/stub provides an API-compatible USBSID.h (no libusb).
  set(SOURCEFILES
    ${SOURCEFILES}
    ${CMAKE_CURRENT_LIST_DIR}/src/web/web_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/web/web_player.cpp
    )
  set(TARGET_INCLUDE_DIRS
    ${TARGET_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/src/web/stub
  )
endif (WEB)

### Compile time
add_executable(${EXECUTABLE} ${SOURCEFILES})

if (${DESKTOP} EQUAL 1 AND ${USE_RESIDFP} EQUAL 1 AND ${WEB} EQUAL 0)
  add_dependencies(${EXECUTABLE} BUILDRESID)
  target_link_libraries(${EXECUTABLE} ${RESIDFP_LIB})
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
target_compile_definitions(${EXECUTABLE} PRIVATE UNIX_COMPILE)
# target_compile_definitions(${PROJECT_NAME} PRIVATE __LINUX_ALSA__)
# target_compile_definitions(${PROJECT_NAME} PRIVATE __RTMIDI_DEBUG__)
endif ()

# if (WIN32)
# target_compile_definitions(${PROJECT_NAME} PRIVATE WINDOWS_COMPILE)
# target_compile_definitions(${PROJECT_NAME} PRIVATE TERMIWIN_DONOTREDEFINE=1)
# endif (WIN32)

target_include_directories(${EXECUTABLE} ${TARGET_INCLUDE_DIRS})
target_link_libraries(${EXECUTABLE} ${TARGET_LL})
target_sources(${EXECUTABLE} PUBLIC ${SOURCEFILES})
target_compile_options(${EXECUTABLE} ${COMPILE_OPTS})

if (WEB)
  ### Emscripten output + exports. main() is not auto-run (INVOKE_RUN=0);
  ### JS drives playback via the exported control functions (P3).
  set_target_properties(${EXECUTABLE} PROPERTIES SUFFIX ".js")
  target_link_options(${EXECUTABLE} PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=USBSIDPlayer"
    "-sENVIRONMENT=web,worker,node"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINVOKE_RUN=0"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8']"
    "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_usp_alloc','_usp_free','_usp_load_sidtune','_usp_load_prg','_usp_init_sidplayer','_usp_start','_usp_step','_usp_step_instr','_usp_refresh_hz','_usp_is_pal','_usp_stop','_usp_next_subtune','_usp_prev_subtune','_usp_pause','_usp_is_playing','_usp_force_socket_two','_usbsid_web_ring_ptr','_usbsid_web_ring_entries','_usbsid_web_ring_head','_usbsid_web_ring_tail','_usbsid_web_ring_set_tail','_usbsid_web_flush_count','_usbsid_web_drop_count','_usbsid_web_write_count','_usbsid_web_get_clockrate']"
  )
endif (WEB)

if (NOT WEB)
### Copy build output to main directory
add_custom_command(TARGET
  ${EXECUTABLE}
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${EXECUTABLE}> ${CMAKE_CURRENT_LIST_DIR})
### Remove build output from build directory
add_custom_command(TARGET
  ${EXECUTABLE}
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm $<TARGET_FILE:${EXECUTABLE}>)
endif (NOT WEB)
