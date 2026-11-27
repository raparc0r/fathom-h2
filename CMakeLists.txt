# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Host build (macOS/Linux): core libraries, desktop simulator, tools and tests.
# Device builds go through Buildroot (buildroot/) inside the Docker image (docker/).
cmake_minimum_required(VERSION 3.20)
project(fathom C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(FATHOM_BUILD_SIM "Build the desktop simulator (needs SDL2)" ON)
set(FATHOM_WARNINGS -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes)

# --- core libraries (also compiled into the device player) ---
set(FATHOM_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(cmake/fathom_sources.cmake)             # (the app's files, listed once: the device's build reads it too)
add_library(fathom_core STATIC
  ${FATHOM_SRC_CORE} ${FATHOM_SRC_LIBRARY_BASE} ${FATHOM_SRC_SHUFFLE} ${FATHOM_SRC_UI_BASE})
target_include_directories(fathom_core PUBLIC app)
target_compile_options(fathom_core PRIVATE ${FATHOM_WARNINGS})
target_link_libraries(fathom_core PUBLIC m)

# --- player libraries (library database, audio) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(PLAYER_DEPS REQUIRED IMPORTED_TARGET sqlite3 flac libmpg123)
add_library(fathom_player STATIC
  ${FATHOM_SRC_NET} ${FATHOM_SRC_LIBRARY} ${FATHOM_SRC_DECODE} ${FATHOM_SRC_ENGINE} ${FATHOM_SRC_PLAYER})
target_include_directories(fathom_player PUBLIC app third_party)
# zlib: the updater's unzip (on the H2 it comes with FFmpeg's)
find_package(ZLIB REQUIRED)
target_link_libraries(fathom_player PUBLIC fathom_core PkgConfig::PLAYER_DEPS ZLIB::ZLIB)
target_compile_options(fathom_player PRIVATE ${FATHOM_WARNINGS})
set_source_files_properties(app/library/stb_impl.c PROPERTIES COMPILE_OPTIONS "-w")
# Scaled JPEG decoding for cover art (always on the H2; on the desktop when available)
pkg_check_modules(JPEG IMPORTED_TARGET libjpeg)
if(JPEG_FOUND)
  target_compile_definitions(fathom_player PRIVATE FATHOM_HAVE_LIBJPEG)
  target_link_libraries(fathom_player PUBLIC PkgConfig::JPEG)
endif()
# The other formats (ALAC, AAC, WAV, AIFF, WavPack, APE, Opus...) through FFmpeg
# (always on the H2; on the desktop when available)
pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
if(FFMPEG_FOUND)
  target_sources(fathom_player PRIVATE ${FATHOM_SRC_FFMPEG})
  target_compile_definitions(fathom_player PRIVATE FATHOM_HAVE_FFMPEG)
  target_link_libraries(fathom_player PUBLIC PkgConfig::FFMPEG)
endif()
# Ogg Vorbis (app/audio/vorbis.c): Tremor as on the H2 if it's here, else
# libvorbisfile (floating point, fine on a computer); without either, not played
pkg_check_modules(TREMOR IMPORTED_TARGET vorbisidec)
if(TREMOR_FOUND)
  target_compile_definitions(fathom_player PRIVATE FATHOM_HAVE_VORBIS FATHOM_VORBIS_TREMOR)
  target_link_libraries(fathom_player PUBLIC PkgConfig::TREMOR)
else()
  pkg_check_modules(VORBISFILE IMPORTED_TARGET vorbisfile)
  if(VORBISFILE_FOUND)
    target_compile_definitions(fathom_player PRIVATE FATHOM_HAVE_VORBIS)
    target_link_libraries(fathom_player PUBLIC PkgConfig::VORBISFILE)
  endif()
endif()

add_executable(fathom-scan tools/fathom-scan/main.c)
target_link_libraries(fathom-scan PRIVATE fathom_player)
target_compile_options(fathom-scan PRIVATE ${FATHOM_WARNINGS})

add_executable(play-test tools/play-test/main.c)
target_link_libraries(play-test PRIVATE fathom_player)
target_compile_options(play-test PRIVATE ${FATHOM_WARNINGS})

# rg-bench: levelling's loudness measurement, timed, against 0.8.3's (tools/rg-bench)
add_executable(rg-bench tools/rg-bench/main.c tools/rg-bench/loudness_v1.c tools/rg-bench/loudness_ref.c)
target_link_libraries(rg-bench PRIVATE fathom_player)
target_compile_options(rg-bench PRIVATE ${FATHOM_WARNINGS})

# --- tools ---
add_library(fathom_simlib STATIC tools/shuffle-sim/simlib.c)
target_link_libraries(fathom_simlib PUBLIC fathom_core m)
target_compile_options(fathom_simlib PRIVATE ${FATHOM_WARNINGS})

add_executable(shuffle-sim tools/shuffle-sim/main.c)
target_link_libraries(shuffle-sim PRIVATE fathom_simlib)
target_compile_options(shuffle-sim PRIVATE ${FATHOM_WARNINGS})

# make-bitperfect: FLAC test files with known samples (tools/bitperfect)
add_executable(make-bitperfect tools/bitperfect/make_signals.c)
target_link_libraries(make-bitperfect PRIVATE PkgConfig::PLAYER_DEPS)
target_compile_options(make-bitperfect PRIVATE ${FATHOM_WARNINGS})

add_executable(theme-check tools/theme-check/main.c)
target_link_libraries(theme-check PRIVATE fathom_core)
target_compile_options(theme-check PRIVATE ${FATHOM_WARNINGS})

# --- tests ---
enable_testing()
# One test: the program test_<name> from tests/test_<name>.c (MAIN: from another file) and
# SOURCES, with the warnings on (NO_WARNINGS: without them), and CTest's test <name>, which
# runs it (CWD: in the source folder, for a test that reads assets/ or tests/ by a relative
# path). tools/h2-tests.sh builds every target named test_* for the H2 and runs it there,
# in a folder with the same assets/ and tests/ beside it.
function(fathom_test name)
  cmake_parse_arguments(T "CWD;INCLUDES_BEFORE;NO_WARNINGS" "MAIN" "SOURCES;LIBS;INCLUDES;DEFS" ${ARGN})
  if(T_UNPARSED_ARGUMENTS OR T_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "fathom_test(${name}): ${T_UNPARSED_ARGUMENTS} ${T_KEYWORDS_MISSING_VALUES}?")
  endif()
  if(NOT T_MAIN)
    set(T_MAIN tests/test_${name}.c)
  endif()
  add_executable(test_${name} ${T_MAIN} ${T_SOURCES})
  if(T_INCLUDES_BEFORE)   # (ahead of every other folder, the libraries' too)
    target_include_directories(test_${name} BEFORE PRIVATE ${T_INCLUDES})
  else()
    target_include_directories(test_${name} PRIVATE ${T_INCLUDES})
  endif()
  target_link_libraries(test_${name} PRIVATE ${T_LIBS})
  target_compile_definitions(test_${name} PRIVATE ${T_DEFS})
  if(NOT T_NO_WARNINGS)
    target_compile_options(test_${name} PRIVATE ${FATHOM_WARNINGS})
  endif()
  if(T_CWD)
    add_test(NAME ${name} COMMAND test_${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  else()
    add_test(NAME ${name} COMMAND test_${name})
  endif()
endfunction()
# A test that is a shell script, tests/test_<name>.sh, run in the source folder
function(fathom_sh_test name)
  add_test(NAME ${name} COMMAND sh tests/test_${name}.sh WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

fathom_test(shuffle LIBS fathom_simlib)
fathom_test(playlist SOURCES app/library/playlist.c INCLUDES app)
fathom_test(loudness SOURCES tools/rg-bench/loudness_v1.c tools/rg-bench/loudness_ref.c
  INCLUDES tools/rg-bench LIBS fathom_player)
fathom_test(btconv LIBS fathom_player)
fathom_test(podcast LIBS fathom_player INCLUDES tests CWD)
fathom_test(http LIBS fathom_player pthread INCLUDES tests)
if(FFMPEG_FOUND)
  target_compile_definitions(test_http PRIVATE FATHOM_HAVE_FFMPEG)
endif()
fathom_test(podsync LIBS fathom_player INCLUDES tests CWD)
fathom_test(coverfind LIBS fathom_player INCLUDES tests CWD)
fathom_test(crossfeed LIBS fathom_player INCLUDES tests CWD)
# Stereo width: the arithmetic, and through the engine
fathom_test(width LIBS fathom_player INCLUDES tests CWD)
fathom_test(tempo LIBS fathom_player m)
fathom_test(theme LIBS fathom_core CWD)
fathom_test(coverflow LIBS fathom_core)
fathom_test(glyphpack LIBS fathom_core)
fathom_test(scripts LIBS fathom_player pthread)
fathom_test(packfetch LIBS fathom_player)
fathom_test(eq LIBS fathom_player)
fathom_test(gain LIBS fathom_player)
fathom_test(radio LIBS fathom_player CWD)
fathom_test(rfmeta LIBS fathom_player)
# profile-library: the calls behind what's slow on a big library, timed (tools/profile-library)
add_executable(profile-library tools/profile-library/main.c)
target_link_libraries(profile-library PRIVATE fathom_player)
target_compile_options(profile-library PRIVATE ${FATHOM_WARNINGS})

fathom_test(player LIBS fathom_player)
fathom_test(engine LIBS fathom_player)
fathom_test(groups LIBS fathom_player)
fathom_test(scan LIBS fathom_player pthread)
fathom_test(reader LIBS fathom_player pthread CWD)
fathom_test(tags LIBS fathom_player)
fathom_test(sort LIBS fathom_player)
fathom_test(cue LIBS fathom_player)
# Lyrics: an LRC's lines and times, the tags' lyrics, where they're kept, the lookup online
fathom_test(lyrics LIBS fathom_player pthread INCLUDES tests)
# The FAT32 reader of the quick check (app/library/fatwalk.c) on card images made by the test
fathom_test(fatwalk LIBS fathom_player INCLUDES tests)
fathom_test(works LIBS fathom_player)
fathom_test(streamlist LIBS fathom_player)
fathom_test(dsd LIBS fathom_player)
fathom_test(mp3 LIBS fathom_player)
# Ogg Vorbis, on files made with libvorbisenc (when there's a Vorbis library and the encoder)
pkg_check_modules(VORBISENC IMPORTED_TARGET vorbisenc vorbis ogg)
if((TREMOR_FOUND OR VORBISFILE_FOUND) AND VORBISENC_FOUND)
  fathom_test(vorbis LIBS fathom_player PkgConfig::VORBISENC m)
endif()
fathom_test(json LIBS fathom_player)
fathom_test(radiodir LIBS fathom_player CWD)
fathom_test(radioicon LIBS fathom_player pthread INCLUDES tests CWD)
# What a server sends for a station's icon (an .ico, a page, the directory's answer), damaged
# at random, the same way every time; longer, and under the sanitizers: see the file's head
fathom_test(fuzz_radioicon MAIN tests/fuzz_radioicon.c LIBS fathom_player)
fathom_test(update LIBS fathom_player CWD)
if(JPEG_FOUND)
  fathom_test(art LIBS fathom_player)
endif()
# h2boot's fallback to the previous kernel: its own code, on a folder for a card
fathom_test(bootpick SOURCES bootloader/fathom/bootpick.c DEFS BOOTPICK_HOST)
fathom_test(bootlight SOURCES bootloader/fathom/bootlight.c DEFS BOOTLIGHT_HOST INCLUDES bootloader/fathom)
fathom_test(pcmring SOURCES app/audio/pcmring.c INCLUDES app)
fathom_test(batlog LIBS fathom_core)
fathom_test(wifitxt LIBS fathom_core)
fathom_test(fsafe LIBS fathom_core)
fathom_test(settings LIBS fathom_core)
fathom_test(lang LIBS fathom_core)
fathom_test(dnscache SOURCES app/net/dnscache.c INCLUDES app LIBS pthread)
# The player's shared certificate bundle (app/net/tls_wrap.c): only where FFmpeg and mbedTLS
# are installed and the linker has --wrap (GNU ld or lld on Linux, not macOS's ld)
pkg_check_modules(MBEDTLS QUIET IMPORTED_TARGET mbedtls mbedx509 mbedcrypto)
if(MBEDTLS_FOUND AND FFMPEG_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  fathom_test(tls_wrap SOURCES app/net/tls_wrap.c app/audio/ff_util.c INCLUDES app
    LIBS PkgConfig::FFMPEG PkgConfig::MBEDTLS pthread NO_WARNINGS CWD)
  target_link_options(test_tls_wrap PRIVATE
    -Wl,--wrap=mbedtls_x509_crt_parse_file -Wl,--wrap=mbedtls_x509_crt_free)
endif()
# The clock and HTTPS (app/net/netclock.c): the Date header and the year check; where
# mbedTLS is installed, its HTTPS request too, against a server of its own (tests/tls)
fathom_test(netclock SOURCES app/net/netclock.c app/core/datetime.c INCLUDES app tests CWD)
if(MBEDTLS_FOUND)
  target_compile_definitions(test_netclock PRIVATE FATHOM_HAVE_MBEDTLS)
  target_link_libraries(test_netclock PRIVATE PkgConfig::MBEDTLS pthread)
endif()
# Setting the clock by hand: a date and time a field at a time (app/core/datetime.c)
fathom_test(datetime LIBS fathom_core)
# Time zones for it (app/core/tz.c), against the C library's reading of the same rules
fathom_test(tz LIBS fathom_core)
# The udhcpc hook that sets the clock by NTP, trying again while it gets no answer
fathom_sh_test(fathom_ntp)
# The start-up scripts, against a root made for the test: the wait for the card and the
# boot report (S01bootlog); the USB network and the player's start (S40usbnet, S90fathom)
fathom_sh_test(bootlog)
fathom_sh_test(startup)
# USB drive mode: the card lent, its end seen (ejected, unplugged), the card back
fathom_sh_test(fathom_usbdisk)
# fathom-ssh: the password's hash, SSH as the settings say, two calls at once
fathom_sh_test(fathom_ssh)
# device/shell.c on the host: commands run for the workers (output, status, deadline, no
# fork), and what the Wi-Fi worker reads of wpa_supplicant's answers
fathom_test(shell LIBS pthread INCLUDES app)     # (device/shell.c: core/clock.h)
# fathom-wifi and fathom-bt against stand-ins for wpa_cli, udhcpc and bluetoothctl
fathom_sh_test(fathom_wifi)
fathom_sh_test(fathom_bt)
# HiBy's update files, for Uninstall Fathom: read, check, mark the bootloader, undo
fathom_test(hibyupt SOURCES app/sys/hibyupt.c app/sys/md5.c app/sys/uninstall.c INCLUDES app)
# The bootloader's copy of the Bluetooth and Wi-Fi firmware out of the stock system
fathom_test(fwcopy SOURCES bootloader/fathom/fwcopy.c INCLUDES bootloader/fathom)
add_executable(tags-compare tools/tags-compare/main.c)   # the one-pass FLAC reader vs libFLAC's
target_link_libraries(tags-compare PRIVATE fathom_player)
target_compile_options(tags-compare PRIVATE ${FATHOM_WARNINGS})
add_executable(fwcopy tools/fwcopy/main.c bootloader/fathom/fwcopy.c)   # (on a flash backup)
target_include_directories(fwcopy PRIVATE bootloader/fathom)
target_compile_options(fwcopy PRIVATE ${FATHOM_WARNINGS})
file(GLOB FATHOM_THEMES ${CMAKE_SOURCE_DIR}/assets/themes/*.theme)
add_test(NAME themes COMMAND theme-check --strict ${FATHOM_THEMES})
add_test(NAME shuffle_sim_smoke COMMAND shuffle-sim --synthetic 3000 --trials 3)

# --- desktop simulator ---
if(FATHOM_BUILD_SIM)
  add_subdirectory(sim)
  # device/input.c on the host (its Linux headers faked in tests/fake; it needs LVGL's for ui.h)
  fathom_test(input INCLUDES tests/fake app INCLUDES_BEFORE LIBS lvgl)
  # A theme's own files (app/ui/assets.c, fonts.c): what a made-up theme may cost at start-up
  fathom_test(assets SOURCES app/ui/assets.c app/ui/fonts.c INCLUDES tests LIBS fathom_player lvgl
    DEFS FATHOM_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()
