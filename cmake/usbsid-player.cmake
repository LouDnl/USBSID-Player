####
# USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
# line playback and for embedding on RP2350 (Pico2).
#
# cmake/usbsid-player.cmake
# What the firmware needs to include to build the player into itself.
#
# The firmware build owns the toolchain, the optimisation flags and the target,
# so this file adds no target of its own and changes nothing global. It sets
# three variables and defines one custom target:
#
#   USPLAYER_SOURCES       every source file to add to the firmware target
#   USPLAYER_INCLUDE_DIRS  every include directory those sources need
#   USPLAYER_DEFINITIONS   the definitions they expect
#   RSIDDriver             a target that assembles psiddrv.a65 into psiddrv.h
#
# Use it from repo/CMakeLists.txt like this:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/lib/usbsid-player/cmake/usbsid-player.cmake)
#   set(SOURCEFILES ${SOURCEFILES} ${USPLAYER_SOURCES})
#   set(TARGET_INCLUDE_DIRS ${TARGET_INCLUDE_DIRS} ${USPLAYER_INCLUDE_DIRS})
#   add_compile_definitions(${USPLAYER_DEFINITIONS})
#   ...
#   add_dependencies(${BUILD} RSIDDriver)
#
# See docs/EMBEDDED.md for the whole patch and for what the firmware has to
# call at runtime.
#
# This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
# File author: LouD
#
# Copyright (c) 2026 LouD
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

get_filename_component(USPLAYER_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

message(STATUS "USBSID-Player from ${USPLAYER_ROOT}")

### Everything that goes into the firmware. The desktop only files, the CLI
### frontend and the USB driver backend, are deliberately not here.
### This list is the firmware's, and it is **not** the one in CMakeLists.txt.
###
### Two lists that have to agree about anything the firmware needs, and nothing
### tells you when they stop agreeing except a link error on a machine with an
### ARM toolchain. `firmware_hooks.cpp` was added to the other one and not to
### this one, and the first anyone knew was
###   undefined reference to `us_reset_sid_registers'
###
### What belongs here: everything the player needs on the device. What does not:
### `main_cli.cpp`, `web_api.cpp`, `sid_usbsid.cpp` (libusb), `sid_web.cpp`,
### `pacing.cpp` (the desktop pacer) and `songlengths.cpp` (no filesystem).
### Adding a file to the core build means asking which of those two it is.
set(USPLAYER_SOURCES
  ${USPLAYER_ROOT}/src/api/usplayer.cpp
  ${USPLAYER_ROOT}/src/core/bus.cpp
  ${USPLAYER_ROOT}/src/core/machine.cpp
  ${USPLAYER_ROOT}/src/cia/mos6526.cpp
  ${USPLAYER_ROOT}/src/cpu/mos6510.cpp
  ${USPLAYER_ROOT}/src/driver/psiddrv_install.cpp
  ${USPLAYER_ROOT}/src/driver/reloc65.c
  ${USPLAYER_ROOT}/src/file/prgfile.cpp
  ${USPLAYER_ROOT}/src/file/sidfile.cpp
  ${USPLAYER_ROOT}/src/io/keyboard.cpp
  ${USPLAYER_ROOT}/src/util/logging.cpp
  ${USPLAYER_ROOT}/src/mem/mmu.cpp
  ${USPLAYER_ROOT}/src/mem/mos906114_pla.cpp
  ${USPLAYER_ROOT}/src/mem/ram.cpp
  ${USPLAYER_ROOT}/src/mem/roms/rom_data.cpp
  ${USPLAYER_ROOT}/src/player/player.cpp
  ${USPLAYER_ROOT}/src/sid/mos6581_8580.cpp
  ${USPLAYER_ROOT}/src/sid/firmware_hooks.cpp
  ${USPLAYER_ROOT}/src/sid/sid_embedded.cpp
  ${USPLAYER_ROOT}/src/sid/sid_voice3.cpp
  ${USPLAYER_ROOT}/src/vic/mos6569.cpp
)

set(USPLAYER_INCLUDE_DIRS
  ${USPLAYER_ROOT}/src
  ${USPLAYER_ROOT}/src/api
  ${USPLAYER_ROOT}/src/core
  ${USPLAYER_ROOT}/src/cia
  ${USPLAYER_ROOT}/src/cpu
  ${USPLAYER_ROOT}/src/driver
  ${USPLAYER_ROOT}/src/file
  ${USPLAYER_ROOT}/src/io
  ${USPLAYER_ROOT}/src/mem
  ${USPLAYER_ROOT}/src/mem/roms
  ${USPLAYER_ROOT}/src/player
  ${USPLAYER_ROOT}/src/sid
  ${USPLAYER_ROOT}/src/util
  ${USPLAYER_ROOT}/src/vic
)

### EMBEDDED picks the RAM placement attributes in types.h and the firmware
### side of the SID backend. DESKTOP has to be defined as 0 rather than left
### undefined, because the sources test it with #if.
set(USPLAYER_DEFINITIONS
  EMBEDDED=1
  DESKTOP=0
)

### reloc65.c is third party C from psid64 and predates the firmware's warning
### settings. It gets its own, so -Werror elsewhere stays on.
set_source_files_properties(${USPLAYER_ROOT}/src/driver/reloc65.c
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-parameter;-Wno-error")

### The psid driver: xa65 assembles the 6502 source, bin2h turns the o65 into a
### header. Same two tools the firmware already needs for player-repo.
add_custom_command(
  OUTPUT ${USPLAYER_ROOT}/src/driver/psiddrv.h
  COMMAND $(MAKE) all
  # the o65 must not be left where a linker could find it
  COMMAND ${CMAKE_COMMAND} -E rm -f ${USPLAYER_ROOT}/src/driver/psiddrv.o65
  WORKING_DIRECTORY ${USPLAYER_ROOT}/src/driver/
  COMMENT "Assembling the psid driver"
)
add_custom_target(RSIDDriver ALL
  DEPENDS ${USPLAYER_ROOT}/src/driver/psiddrv.h
)

### A warning worth having in the build log rather than in a support thread.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_BUILD_TYPE STREQUAL "")
  message(WARNING
    "USBSID-Player wants -O3. At lower optimisation the emulation does not "
    "keep up with the SID bus and playback stutters.")
endif()
