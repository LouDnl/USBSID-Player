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
# Copyright (c) 2025 LouD
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

### Project magic sprinkles
set(PROJECT_NAME usbsid)

# enable or disable desktop build
set(DESKTOP 1 CACHE STRING "DESKTOP")
# enable or disable embedded build
set(EMBEDDED 0 CACHE STRING "EMBEDDED")
# enable or disable radare2 debugging support
set(RADARE2 1 CACHE STRING "RADARE2")

if(${DESKTOP} EQUAL 1 AND ${EMBEDDED} EQUAL 1)
  message(FATAL_ERROR "DESKTOP and EMBEDDED are mutually exclusive! Exiting...")
endif()

if(${DESKTOP} EQUAL 0 AND ${EMBEDDED} EQUAL 0)
  message(FATAL_ERROR "Please set either DESKTOP or EMBEDDED to 1! Exiting...")
endif()

### Compiler flags and options
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
  -DDESKTOP=${DESKTOP}
  -DEMBEDDED=${EMBEDDED}
)

# Windows additionals
# if (WIN32)
# set(WIN32_SRC
#   ${CMAKE_CURRENT_LIST_DIR}/src/termiwin/termiWin.c
# )
# set(WIN32_INC
#   ${CMAKE_CURRENT_LIST_DIR}/src/termiwin
# )
# endif (WIN32)


# Run the project command
project(${PROJECT_NAME} C CXX ASM)

if (DESKTOP)
  # the `pkg_check_modules` function is created with this call
  find_package(PkgConfig REQUIRED)

  # these calls create special `PkgConfig::<MODULE>` variables
  pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

  ### Libraries to link
  if (UNIX)
    pkg_check_modules(udev REQUIRED IMPORTED_TARGET libudev)
    # pkg_check_modules(asound REQUIRED IMPORTED_TARGET alsa)
    # pkg_check_modules(jack REQUIRED IMPORTED_TARGET jack)
    # pkg_check_modules(pthread REQUIRED IMPORTED_TARGET libpthread)
    set(TARGET_LL
      PkgConfig::libusb
      PkgConfig::udev
      # PkgConfig::asound
      # PkgConfig::pthread
    )
  endif (UNIX)
  if (WIN32)
    set(TARGET_LL
      PkgConfig::libusb
    )
  endif (WIN32)

endif (DESKTOP)

### Source files to compile
set(SOURCEFILES
  ${CMAKE_CURRENT_LIST_DIR}/src/main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/usplayer.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/emulation.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6510_cpu.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6526_cia.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6560_6561_vic.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos6581_8580_sid.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mos906114_pla.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/mmu.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c64/timer.cpp
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
  # ${WIN32_INC}
  /usr/local/lib
  /usr/local/include
  /usr/lib
  /usr/include
)

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

if (DESKTOP)
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
endif (DESKTOP)

### Compile time
add_executable(${PROJECT_NAME} ${SOURCEFILES})

if (UNIX)
target_compile_definitions(${PROJECT_NAME} PRIVATE UNIX_COMPILE)
# target_compile_definitions(${PROJECT_NAME} PRIVATE __LINUX_ALSA__)
# target_compile_definitions(${PROJECT_NAME} PRIVATE __RTMIDI_DEBUG__)
endif (UNIX)

# if (WIN32)
# target_compile_definitions(${PROJECT_NAME} PRIVATE TERMIWIN_DONOTREDEFINE=1)
# target_compile_definitions(${PROJECT_NAME} PRIVATE WINDOWS_COMPILE)
# endif (WIN32)

target_include_directories(${PROJECT_NAME} ${TARGET_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${TARGET_LL})
target_sources(${PROJECT_NAME} PUBLIC ${SOURCEFILES})
target_compile_options(${PROJECT_NAME} ${COMPILE_OPTS})
