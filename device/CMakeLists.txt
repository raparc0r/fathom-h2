# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Fathom player for the H2, cross-compiled by Buildroot (package fathom-player):
# one static binary that runs from the SD card. LVGL is fetched at the same
# pinned version as the simulator.
cmake_minimum_required(VERSION 3.20)
project(fathom_device C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

include(FetchContent)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE FILEPATH "LVGL config" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG        v9.2.2
  GIT_SHALLOW    TRUE
  # our fixes (patches/lvgl)
  PATCH_COMMAND sh ${ROOT}/patches/lvgl/apply.sh)
FetchContent_MakeAvailable(lvgl)
# Each function in its own section, so the link keeps only those used (--gc-sections)
target_compile_options(lvgl PRIVATE -ffunction-sections -fdata-sections)

find_package(PkgConfig REQUIRED)
# vorbisidec: Tremor, the integer Vorbis decoder (app/audio/vorbis.c)
pkg_check_modules(DEPS REQUIRED sqlite3 flac libmpg123 libjpeg libavformat libavcodec libavutil vorbisidec)
# FFmpeg's https is mbedTLS (radio); its pkg-config names only -lmbedtls, and a
# static link needs the other two after it
pkg_check_modules(MBEDTLS QUIET mbedtls mbedx509 mbedcrypto)
if(MBEDTLS_FOUND)
  set(DEPS_STATIC_LDFLAGS ${DEPS_STATIC_LDFLAGS} -lmbedtls -lmbedx509 -lmbedcrypto)
endif()

# The board: our own kernel's DRM display and the PB5 output switch.
set(FATHOM_BOARD_SOURCES disp_drm.c audio_route.c)

add_executable(fathom-player
  main.c input.c uac_relay.c bt.c wifi.c share.c ssh.c tlscert.c radiometa.c out_router.c ff_trim.c updater.c ${ROOT}/app/audio/out_bt.c ${FATHOM_BOARD_SOURCES}
  ${ROOT}/app/core/rng.c
  ${ROOT}/app/core/settings.c
  ${ROOT}/app/core/lang.c
  ${ROOT}/app/core/intern.c
  ${ROOT}/app/library/keys.c
  ${ROOT}/app/library/tags.c
  ${ROOT}/app/library/cue.c
  ${ROOT}/app/library/library.c
  ${ROOT}/app/library/fatwalk.c
  ${ROOT}/app/library/playlist.c
  ${ROOT}/app/library/stb_impl.c
  ${ROOT}/app/shuffle/shuffle.c
  ${ROOT}/app/audio/decoder.c
  ${ROOT}/app/audio/dsd.c
  ${ROOT}/app/audio/vorbis.c
  ${ROOT}/app/audio/streamlist.c
  ${ROOT}/app/audio/ff_util.c
  ${ROOT}/app/net/http.c
  ${ROOT}/app/net/json.c
  ${ROOT}/app/net/radiodir.c
  ${ROOT}/app/net/sha256.c
  ${ROOT}/app/net/unzip.c
  ${ROOT}/app/net/update.c
  ${ROOT}/app/audio/engine.c
  ${ROOT}/app/audio/loudness.c
  ${ROOT}/app/audio/btconv.c ${ROOT}/app/audio/eq.c
  ${ROOT}/app/audio/out_null.c
  ${ROOT}/app/audio/out_clock.c
  ${ROOT}/app/audio/out_alsa.c
  ${ROOT}/app/audio/mixer_alsa.c
  ${ROOT}/app/player/player.c
  ${ROOT}/app/player/radio.c
  ${ROOT}/app/player/rfmeta.c
  ${ROOT}/app/ui/theme.c
  ${ROOT}/app/ui/fonts.c
  ${ROOT}/app/ui/assets.c
  ${ROOT}/app/ui/ui.c)
target_include_directories(fathom-player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${ROOT}/app ${ROOT}/third_party ${DEPS_STATIC_INCLUDE_DIRS})
# 32 MiB of RAM: JPEG covers decode pre-scaled (libjpeg); PNGs are capped; small SQLite cache.
# The version shown in Settings > About: git describe of the tree being built.
execute_process(COMMAND git -C ${ROOT} describe --always --dirty
                OUTPUT_VARIABLE FATHOM_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT FATHOM_GIT)
  set(FATHOM_GIT "unknown")
endif()
target_compile_definitions(fathom-player PRIVATE
  "FATHOM_VERSION=\"${FATHOM_GIT}\""
  FATHOM_HAVE_LIBJPEG
  FATHOM_HAVE_FFMPEG
  FATHOM_HAVE_VORBIS FATHOM_VORBIS_TREMOR
  $<$<BOOL:${MBEDTLS_FOUND}>:FATHOM_HAVE_MBEDTLS>
  FATHOM_ART_MAX_PIXELS=1200000LL
  FATHOM_JPEG_MAX_MEMORY=4194304L
  FATHOM_ART_MAX_FILE=4194304L
  "FATHOM_SQLITE_CACHE=\"-512\"")
target_compile_options(fathom-player PRIVATE -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)
set_source_files_properties(${ROOT}/app/library/stb_impl.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_options(fathom-player PRIVATE -static -Wl,--gc-sections)
# The certificate bundle parsed once for all of FFmpeg's HTTPS connections
# (app/net/tls_wrap.c: FFmpeg's calls to mbedTLS are redirected to it)
if(MBEDTLS_FOUND)
  target_sources(fathom-player PRIVATE ${ROOT}/app/net/tls_wrap.c)
  target_link_options(fathom-player PRIVATE
    -Wl,--wrap=mbedtls_x509_crt_parse_file -Wl,--wrap=mbedtls_x509_crt_free)
endif()
# z: the updater's unzip (app/net/unzip.c), the zlib FFmpeg already links
target_link_libraries(fathom-player PRIVATE lvgl ${DEPS_STATIC_LDFLAGS} z pthread m)
install(TARGETS fathom-player RUNTIME DESTINATION bin)

# Bit-perfect checks (tools/bitperfect): plays files through the player's engine
# and ALSA output, recording what reaches the card. Not installed; copied by hand.
add_executable(fathom-bitperfect
  ${ROOT}/tools/bitperfect/bp_play.c
  ${ROOT}/app/audio/decoder.c
  ${ROOT}/app/library/cue.c
  ${ROOT}/app/audio/dsd.c
  ${ROOT}/app/audio/vorbis.c
  ${ROOT}/app/audio/streamlist.c
  ${ROOT}/app/audio/ff_util.c
  ${ROOT}/app/net/http.c
  ${ROOT}/app/net/json.c
  ${ROOT}/app/audio/engine.c
  ${ROOT}/app/audio/eq.c
  ${ROOT}/app/audio/out_alsa.c
  ${ROOT}/app/audio/mixer_alsa.c)
target_include_directories(fathom-bitperfect PRIVATE ${ROOT}/app ${DEPS_STATIC_INCLUDE_DIRS})
target_compile_options(fathom-bitperfect PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(fathom-bitperfect PRIVATE FATHOM_HAVE_FFMPEG FATHOM_HAVE_VORBIS FATHOM_VORBIS_TREMOR)
target_link_options(fathom-bitperfect PRIVATE -static)
target_link_libraries(fathom-bitperfect PRIVATE ${DEPS_STATIC_LDFLAGS} pthread m)

# Library scan without the UI (tools/fathom-scan): tags, formats and cover art of a
# folder into a database. Not installed; copied by hand.
add_executable(fathom-scan
  ${ROOT}/tools/fathom-scan/main.c
  ${ROOT}/app/core/intern.c
  ${ROOT}/app/core/rng.c
  ${ROOT}/app/library/keys.c
  ${ROOT}/app/library/tags.c
  ${ROOT}/app/library/cue.c
  ${ROOT}/app/library/library.c
  ${ROOT}/app/library/fatwalk.c
  ${ROOT}/app/library/playlist.c
  ${ROOT}/app/library/stb_impl.c
  ${ROOT}/app/shuffle/shuffle.c
  ${ROOT}/app/audio/decoder.c
  ${ROOT}/app/audio/dsd.c
  ${ROOT}/app/audio/vorbis.c
  ${ROOT}/app/audio/streamlist.c
  ${ROOT}/app/audio/ff_util.c
  ${ROOT}/app/audio/loudness.c)
target_include_directories(fathom-scan PRIVATE ${ROOT}/app ${ROOT}/third_party ${DEPS_STATIC_INCLUDE_DIRS})
target_compile_definitions(fathom-scan PRIVATE FATHOM_HAVE_FFMPEG FATHOM_HAVE_LIBJPEG FATHOM_HAVE_VORBIS FATHOM_VORBIS_TREMOR)
target_link_options(fathom-scan PRIVATE -static)
target_link_libraries(fathom-scan PRIVATE ${DEPS_STATIC_LDFLAGS} pthread m)

# fatwalk check (tools/fatcheck): lists a folder by reading the FAT directly and
# through the kernel, and compares. Not installed; copied by hand.
add_executable(fathom-fatcheck ${ROOT}/tools/fatcheck/fatcheck.c ${ROOT}/app/library/fatwalk.c)
target_include_directories(fathom-fatcheck PRIVATE ${ROOT}/app)
target_compile_options(fathom-fatcheck PRIVATE -Wall -Wextra)
target_link_options(fathom-fatcheck PRIVATE -static)

# Bluetooth check (tools/bluetooth): the chip's version and address, and the
# devices it hears nearby, over raw HCI (no BlueZ). Not installed; copied by hand.
add_executable(fathom-hcitest ${ROOT}/tools/bluetooth/hcitest.c)
target_compile_options(fathom-hcitest PRIVATE -Wall -Wextra)
target_link_options(fathom-hcitest PRIVATE -static)

# The player as an AVRCP target for Bluetooth headphones (avrcp.c),
# dynamically linked with the card's libdbus (fathom/bt), started by fathom-bt.
pkg_check_modules(DBUS QUIET dbus-1)
if(DBUS_FOUND)
  add_executable(fathom-avrcp avrcp.c)
  target_compile_options(fathom-avrcp PRIVATE -Wall -Wextra ${DBUS_CFLAGS})
  target_link_libraries(fathom-avrcp PRIVATE ${DBUS_LDFLAGS})
endif()

# USB-C port role (TCS1421 CFG pins): sink, or dual role to find USB DACs.
# Installed into the root filesystem, so it runs without the SD card.
add_executable(fathom-usbrole usbrole.c)
target_compile_options(fathom-usbrole PRIVATE -Wall -Wextra)
target_link_options(fathom-usbrole PRIVATE -static)
