####
# USBSID-Player: a cycle exact C64 SID player for USBSID-Pico,
# command line playback, websites and for embedding on RP2040/RP2350.
#
# usbsid-player.cmake
# Include this in the firmware build to build the player into it
#
# Adds no target and changes nothing global. Sets three variables and one custom target
#
#   USPLAYER_SOURCES       source files to add to the firmware target
#   USPLAYER_INCLUDE_DIRS  include directories those sources need
#   USPLAYER_DEFINITIONS   definitions they expect
#   RSIDDriver             target that assembles psiddrv.a65 into psiddrv.h
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

get_filename_component(USPLAYER_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

message(STATUS "USBSID-Player from ${USPLAYER_ROOT}")

### Everything that goes into the firmware
### Not the list in CMakeLists.txt, keep both in sync (firmware_hooks.cpp missing in this list gave undefined reference to us_reset_sid_registers)
### Not in this list: main_cli.cpp, web_api.cpp, sid_usbsid.cpp (libusb), sid_web.cpp, pacing.cpp (desktop pacer), songlengths.cpp (no filesystem)
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
  ${USPLAYER_ROOT}/src/vic
)
### src/util (logging.h, md5.h, profile.h) is NOT added, all references use "util/md5.h" via src
### A bare src/util include dir collides with other same-named headers (BTstack md5.h)

### Cynthcart (MC68B50 ACIA on IO1, $de00-$deff).
### Embedded only, see src/cart/cynthcart_embedded.cpp.
set(USPLAYER_CYNTHCART_SOURCES
  ${USPLAYER_ROOT}/src/cart/MC68B50.cpp
  ${USPLAYER_ROOT}/src/cart/cynthcart_embedded.cpp
)
set(USPLAYER_CYNTHCART_INCLUDE_DIRS
  ${USPLAYER_ROOT}/src/cart
)

### EMBEDDED picks the RAM placement attributes in types.h and the firmware side of the SID backend
### DESKTOP must be 0 and not undefined, the sources test it with #if
set(USPLAYER_DEFINITIONS
  EMBEDDED=1
  DESKTOP=0
)

### reloc65.c is third party C from psid64, own warning flags
set_source_files_properties(${USPLAYER_ROOT}/src/driver/reloc65.c
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-parameter;-Wno-error")

### The psid driver: xa65 assembles the 6502 source, bin2h turns the o65 into a header
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

### Warn about low optimisation
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_BUILD_TYPE STREQUAL "")
  message(WARNING
    "USBSID-Player wants -O3. At lower optimisation the emulation does not "
    "keep up with the SID bus and playback stutters.")
endif()
