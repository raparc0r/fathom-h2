# SPDX-License-Identifier: GPL-3.0-or-later
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
add_library(fathom_core STATIC
  app/core/rng.c
  app/core/settings.c
  app/core/intern.c
  app/core/lang.c
  app/library/keys.c
  app/shuffle/shuffle.c
  app/ui/theme.c)
target_include_directories(fathom_core PUBLIC app)
target_compile_options(fathom_core PRIVATE ${FATHOM_WARNINGS})
target_link_libraries(fathom_core PUBLIC m)

# --- player libraries (library database, audio) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(PLAYER_DEPS REQUIRED IMPORTED_TARGET sqlite3 flac libmpg123)
add_library(fathom_player STATIC
  app/net/http.c
  app/net/json.c
  app/net/radiodir.c
  app/net/sha256.c
  app/net/unzip.c
  app/net/update.c
  app/library/tags.c
  app/library/cue.c
  app/library/library.c
  app/library/fatwalk.c
  app/library/playlist.c
  app/library/stb_impl.c
  app/audio/decoder.c
  app/audio/dsd.c
  app/audio/vorbis.c
  app/audio/streamlist.c
  app/audio/engine.c
  app/audio/loudness.c
  app/audio/btconv.c
  app/audio/eq.c
  app/audio/out_null.c
  app/audio/out_clock.c
  app/player/player.c
  app/player/radio.c
  app/player/rfmeta.c)
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
  target_sources(fathom_player PRIVATE app/audio/ff_util.c)
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
add_executable(test_shuffle tests/test_shuffle.c)
target_link_libraries(test_shuffle PRIVATE fathom_simlib)
target_compile_options(test_shuffle PRIVATE ${FATHOM_WARNINGS})
add_test(NAME shuffle COMMAND test_shuffle)
add_executable(test_playlist tests/test_playlist.c app/library/playlist.c)
target_include_directories(test_playlist PRIVATE app)
target_compile_options(test_playlist PRIVATE ${FATHOM_WARNINGS})
add_test(NAME playlist COMMAND test_playlist)
add_executable(test_loudness tests/test_loudness.c)
target_link_libraries(test_loudness PRIVATE fathom_player)
target_compile_options(test_loudness PRIVATE ${FATHOM_WARNINGS})
add_test(NAME loudness COMMAND test_loudness)
add_executable(test_btconv tests/test_btconv.c)
target_link_libraries(test_btconv PRIVATE fathom_player)
target_compile_options(test_btconv PRIVATE ${FATHOM_WARNINGS})
add_test(NAME btconv COMMAND test_btconv)

add_executable(test_eq tests/test_eq.c)
target_link_libraries(test_eq PRIVATE fathom_player)
target_compile_options(test_eq PRIVATE ${FATHOM_WARNINGS})
add_test(NAME eq COMMAND test_eq)

add_executable(test_gain tests/test_gain.c)
target_link_libraries(test_gain PRIVATE fathom_player)
target_compile_options(test_gain PRIVATE ${FATHOM_WARNINGS})
add_test(NAME gain COMMAND test_gain)
add_executable(test_radio tests/test_radio.c)
target_link_libraries(test_radio PRIVATE fathom_player)
target_compile_options(test_radio PRIVATE ${FATHOM_WARNINGS})
add_test(NAME radio COMMAND test_radio WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(test_rfmeta tests/test_rfmeta.c)
target_link_libraries(test_rfmeta PRIVATE fathom_player)
target_compile_options(test_rfmeta PRIVATE ${FATHOM_WARNINGS})
add_test(NAME rfmeta COMMAND test_rfmeta)
add_executable(test_player tests/test_player.c)
target_link_libraries(test_player PRIVATE fathom_player)
target_compile_options(test_player PRIVATE ${FATHOM_WARNINGS})
add_test(NAME player COMMAND test_player)
add_executable(test_groups tests/test_groups.c)
target_link_libraries(test_groups PRIVATE fathom_player)
target_compile_options(test_groups PRIVATE ${FATHOM_WARNINGS})
add_test(NAME groups COMMAND test_groups)
add_executable(test_sort tests/test_sort.c)
target_link_libraries(test_sort PRIVATE fathom_player)
target_compile_options(test_sort PRIVATE ${FATHOM_WARNINGS})
add_test(NAME sort COMMAND test_sort)
add_executable(test_cue tests/test_cue.c)
target_link_libraries(test_cue PRIVATE fathom_player)
target_compile_options(test_cue PRIVATE ${FATHOM_WARNINGS})
add_test(NAME cue COMMAND test_cue)
add_executable(test_works tests/test_works.c)
target_link_libraries(test_works PRIVATE fathom_player)
target_compile_options(test_works PRIVATE ${FATHOM_WARNINGS})
add_test(NAME works COMMAND test_works)
add_executable(test_streamlist tests/test_streamlist.c)
target_link_libraries(test_streamlist PRIVATE fathom_player)
target_compile_options(test_streamlist PRIVATE ${FATHOM_WARNINGS})
add_test(NAME streamlist COMMAND test_streamlist)
add_executable(test_dsd tests/test_dsd.c)
target_link_libraries(test_dsd PRIVATE fathom_player)
target_compile_options(test_dsd PRIVATE ${FATHOM_WARNINGS})
add_test(NAME dsd COMMAND test_dsd)
# Ogg Vorbis, on files made with libvorbisenc (when there's a Vorbis library and the encoder)
pkg_check_modules(VORBISENC IMPORTED_TARGET vorbisenc vorbis ogg)
if((TREMOR_FOUND OR VORBISFILE_FOUND) AND VORBISENC_FOUND)
  add_executable(test_vorbis tests/test_vorbis.c)
  target_link_libraries(test_vorbis PRIVATE fathom_player PkgConfig::VORBISENC m)
  target_compile_options(test_vorbis PRIVATE ${FATHOM_WARNINGS})
  add_test(NAME vorbis COMMAND test_vorbis)
endif()
add_executable(test_json tests/test_json.c)
target_link_libraries(test_json PRIVATE fathom_player)
target_compile_options(test_json PRIVATE ${FATHOM_WARNINGS})
add_test(NAME json COMMAND test_json)
add_executable(test_radiodir tests/test_radiodir.c)
target_link_libraries(test_radiodir PRIVATE fathom_player)
target_compile_options(test_radiodir PRIVATE ${FATHOM_WARNINGS})
add_test(NAME radiodir COMMAND test_radiodir WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(test_update tests/test_update.c)
target_link_libraries(test_update PRIVATE fathom_player)
target_compile_options(test_update PRIVATE ${FATHOM_WARNINGS})
add_test(NAME update COMMAND test_update WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(test_lang tests/test_lang.c)
target_link_libraries(test_lang PRIVATE fathom_core)
target_compile_options(test_lang PRIVATE ${FATHOM_WARNINGS})
add_test(NAME lang COMMAND test_lang)
# The player's shared certificate bundle (app/net/tls_wrap.c): only where FFmpeg and mbedTLS
# are installed and the linker has --wrap (GNU ld or lld on Linux, not macOS's ld)
pkg_check_modules(MBEDTLS QUIET IMPORTED_TARGET mbedtls mbedx509 mbedcrypto)
if(MBEDTLS_FOUND AND FFMPEG_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_tls_wrap tests/test_tls_wrap.c app/net/tls_wrap.c app/audio/ff_util.c)
  target_include_directories(test_tls_wrap PRIVATE app)
  target_link_libraries(test_tls_wrap PRIVATE PkgConfig::FFMPEG PkgConfig::MBEDTLS pthread)
  target_link_options(test_tls_wrap PRIVATE
    -Wl,--wrap=mbedtls_x509_crt_parse_file -Wl,--wrap=mbedtls_x509_crt_free)
  add_test(NAME tls_wrap COMMAND test_tls_wrap WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
# The bootloader's copy of the Bluetooth and Wi-Fi firmware out of the stock system
add_executable(test_fwcopy tests/test_fwcopy.c bootloader/fathom/fwcopy.c)
target_include_directories(test_fwcopy PRIVATE bootloader/fathom)
target_compile_options(test_fwcopy PRIVATE ${FATHOM_WARNINGS})
add_test(NAME fwcopy COMMAND test_fwcopy)
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
endif()
