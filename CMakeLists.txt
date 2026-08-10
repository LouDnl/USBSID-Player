####
# USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
# line playback and for embedding on RP2350 (Pico2).
#
# CMakeLists.txt
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

cmake_minimum_required(VERSION 3.17)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

### Executable name stays `usbsid`, same as player-repo (PROPOSAL 9.1.1)
set(PROJECT_NAME usbsid)
set(EXECUTABLE ${PROJECT_NAME} CACHE STRING "EXECUTABLE")

### Target selection, mutually exclusive
set(DESKTOP 1 CACHE STRING "Build the desktop command line player")
set(EMBEDDED 0 CACHE STRING "Build for the RP2350 firmware")
### The browser player. A sub variant of DESKTOP: the emulation is the same
### code, only the SID backend differs (a heap ring the page drains, instead of
### libusb). Configure it with `emcmake cmake -S . -B temp/build_web -DWEB=1`.
set(WEB 0 CACHE STRING "Build the WebAssembly player")
### Tests, desktop only
set(BUILD_TESTS 1 CACHE STRING "Build the test suite")
### Per cycle counters saying which chip is costing the time. Off by default:
### they are plain increments on the hottest path there is. See
### src/util/profile.h and temp/tools/dbg_cost.cpp.
set(PROFILE 0 CACHE STRING "Build with the per cycle profiling counters")
### Warnings as errors
set(STRICT 1 CACHE STRING "Treat warnings as errors")

if(${WEB} EQUAL 1)
  set(DESKTOP 1)
  set(EMBEDDED 0)
  set(BUILD_TESTS 0) ### the suite is native, it does not cross compile
endif()

if(${DESKTOP} EQUAL 1 AND ${EMBEDDED} EQUAL 1)
  message(FATAL_ERROR "DESKTOP and EMBEDDED are mutually exclusive! Exiting...")
endif()
if(${DESKTOP} EQUAL 0 AND ${EMBEDDED} EQUAL 0)
  message(FATAL_ERROR "Please set either DESKTOP or EMBEDDED to 1! Exiting...")
endif()

### Default to an optimised build. -O3 is a hard requirement for the embedded
### player, and the desktop build wants it too so both run the same code paths.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g")
set(CMAKE_C_FLAGS_RELEASE "-O3 -g")
### Debug info in a wasm module is DWARF that has to be downloaded, and the
### page has no debugger attached to it in normal use. Build it without.
if(${WEB} EQUAL 1)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3")
  set(CMAKE_C_FLAGS_RELEASE "-O3")
endif()

project(${PROJECT_NAME} C CXX ASM)

### EMSCRIPTEN only exists once the toolchain file has run, which is during
### project(), so this cannot be checked any earlier.
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
)

### Core library. Everything except the frontends lives here so the tests, the
### CLI and the embedded API all link identical objects.
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
  ${CMAKE_CURRENT_LIST_DIR}/src/player/pacing.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/player/player.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/sid/mos6581_8580.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/vic/mos6569.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/mem/roms/rom_data.cpp
)

### Which SID backend the C API is compiled against. The embedded one binds to
### the firmware's bus functions through weak symbols, and an undefined weak
### function under wasm-ld is not reliably null, so the web build leaves it out
### altogether rather than linking it and trusting a null check. The desktop
### build takes both, because the test suite exercises the web ring natively.
if(${WEB} EQUAL 1)
  list(APPEND US_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_web.cpp)
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

add_library(usbsidcore STATIC ${US_CORE_SOURCES})
target_include_directories(usbsidcore PUBLIC ${US_CORE_INCLUDE_DIRS})
### -fno-rtti is a C++ flag, and reloc65.c is third party C that was never
### written to today's warning settings, so it gets its own set.
target_compile_options(usbsidcore PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:${US_COMPILE_OPTS}>)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/driver/reloc65.c
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-parameter")
target_compile_definitions(usbsidcore PUBLIC ${US_COMPILE_DEFS})

### The desktop player. The USBSID driver is the copy in lib/driver.
if(${DESKTOP} EQUAL 1 AND ${WEB} EQUAL 0)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)
  set(US_LINK_LIBS PkgConfig::libusb)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(udev REQUIRED IMPORTED_TARGET libudev)
    list(APPEND US_LINK_LIBS PkgConfig::udev)
  endif()

  ### Where the driver's sources are depends on how lib/driver got there.
  ###
  ### It is a submodule of USBSID-Pico-driver, and that repository keeps its
  ### sources in src/ with java/, webusb/ and the rest beside it. So a real
  ### submodule checkout, which is what CI does, gives lib/driver/src/. A
  ### working copy that has the four files dropped in flat, which is what this
  ### tree had and why it built here and not on a runner, gives lib/driver/.
  ### Take whichever is actually present rather than assuming.
  set(US_DRIVER_ROOT ${CMAKE_CURRENT_LIST_DIR}/lib/driver)
  if(EXISTS ${US_DRIVER_ROOT}/src/USBSID.cpp)
    set(US_DRIVER_DIR ${US_DRIVER_ROOT}/src)
  elseif(EXISTS ${US_DRIVER_ROOT}/USBSID.cpp)
    set(US_DRIVER_DIR ${US_DRIVER_ROOT})
  else()
    message(FATAL_ERROR
      "No USBSID driver under ${US_DRIVER_ROOT}.\n"
      "  Looked for src/USBSID.cpp and USBSID.cpp.\n"
      "  The submodule is probably not checked out: run\n"
      "    git submodule update --init --recursive")
  endif()
  message(STATUS "USBSID driver sources: ${US_DRIVER_DIR}")

  add_executable(${EXECUTABLE}
    ${CMAKE_CURRENT_LIST_DIR}/src/host/main_cli.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sid/sid_usbsid.cpp
    ${US_DRIVER_DIR}/USBSID.cpp
  )
  target_link_libraries(${EXECUTABLE} PRIVATE usbsidcore ${US_LINK_LIBS})
  target_include_directories(${EXECUTABLE} PRIVATE ${US_DRIVER_DIR})
  ### the driver is third party and predates these warning settings
  target_compile_options(${EXECUTABLE} PRIVATE -Wall -fno-rtti)
  target_compile_definitions(${EXECUTABLE} PRIVATE UNIX_COMPILE)
endif()

### The browser player. No libusb, no CLI, no threads: one wasm module and the
### flat C ABI in src/host/web_api.cpp. The page in web/ drives it.
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
    _usp_is_playing _usp_is_paused _usp_is_prg _usp_is_pal
    _usp_clock_id _usp_clock_hz _usp_refresh_hz
    _usp_song _usp_songs _usp_frames _usp_sid_writes
    _usp_tune_name _usp_tune_author _usp_tune_released
    _usp_benchmark
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

  ### The same thing again as an ES module, for the worker.
  ###
  ### A module worker cannot importScripts, and the classic output above is a
  ### script rather than a module, so the worker cannot load it. Building both
  ### is cheaper than the alternatives: the page keeps the classic script it
  ### already loads, and the worker gets something it can `import`. They are the
  ### same objects, only the wrapper differs.
  add_executable(${EXECUTABLE}_esm ${CMAKE_CURRENT_LIST_DIR}/src/host/web_api.cpp)
  target_link_libraries(${EXECUTABLE}_esm PRIVATE usbsidcore)
  target_compile_options(${EXECUTABLE}_esm PRIVATE ${US_COMPILE_OPTS})
  set_target_properties(${EXECUTABLE}_esm PROPERTIES
    OUTPUT_NAME "${EXECUTABLE}" SUFFIX ".mjs")
  target_link_options(${EXECUTABLE}_esm PRIVATE ${US_WEB_LINK_OPTS} "-sEXPORT_ES6=1")

  ### The two targets emit the *same* ${EXECUTABLE}.wasm beside their wrapper,
  ### so linking them at once is two writers on one file. A parallel
  ### incremental rebuild caught it: the wasm came out eight bytes long, an
  ### empty module, while everything still linked and reported success.
  ### Ordering them costs nothing, the link takes seconds, and the bytes are
  ### the same either way, which is what `web_smoke.mjs --esm` checks.
  add_dependencies(${EXECUTABLE}_esm ${EXECUTABLE})

  ### Put the artefacts next to the page that loads them.
  ###
  ### web/ is then self contained: it can be served, zipped or copied to
  ### another site's directory without dragging a build tree behind it, and
  ### demo.html and bench.html load `./usbsid.js` rather than reaching up into
  ### a build directory that may or may not be where they last looked.
  ### The three copied files are generated, so .gitignore drops them.
  set(US_WEB_OUT ${CMAKE_CURRENT_LIST_DIR}/web)
  add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${US_WEB_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:${EXECUTABLE}> ${US_WEB_OUT}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE_DIR:${EXECUTABLE}>/${EXECUTABLE}.wasm ${US_WEB_OUT}/
    COMMENT "Copying ${EXECUTABLE}.js and ${EXECUTABLE}.wasm into web/")
  add_custom_command(TARGET ${EXECUTABLE}_esm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:${EXECUTABLE}_esm> ${US_WEB_OUT}/
    COMMENT "Copying ${EXECUTABLE}.mjs into web/")
endif()

### Tests
if(${DESKTOP} EQUAL 1 AND ${BUILD_TESTS} EQUAL 1)
  enable_testing()

  set(US_TEST_NAMES bus ram cpu mmu cia vic sid player embedded prg keyboard web)

  set(US_TEST_SOURCES "")
  foreach(t ${US_TEST_NAMES})
    list(APPEND US_TEST_SOURCES ${CMAKE_CURRENT_LIST_DIR}/tests/test_${t}.cpp)
  endforeach()

  ### One standalone executable per component, registered with CTest so a
  ### failing step is named precisely
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

  ### Aggregate runner with subcommands
  add_executable(usbsid-test
    ${CMAKE_CURRENT_LIST_DIR}/tests/usbsid_test.cpp
    ${US_TEST_SOURCES}
  )
  target_link_libraries(usbsid-test PRIVATE usbsidcore)
  target_include_directories(usbsid-test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests)
  target_compile_options(usbsid-test PRIVATE ${US_COMPILE_OPTS})
  target_compile_definitions(usbsid-test PRIVATE
    US_ASSET_DIR="${CMAKE_CURRENT_LIST_DIR}/assets")
endif()
