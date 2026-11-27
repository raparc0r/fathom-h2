# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# The app's source files, listed once. The Mac's build (CMakeLists.txt: two
# libraries, the simulator, the tests) and the device's (device/CMakeLists.txt:
# the player and its tools, each file compiled into the program) each wrote
# them out and had to be kept in step by hand: a new file goes here.
#
# Set FATHOM_ROOT to the repository's folder before including this.
#
# The order within each list is the order the device's player has always been
# linked in (with link-time optimisation and unused sections dropped, the
# order of its files sets how its code is laid out): add at the end of a list.

set(_r ${FATHOM_ROOT}/app)

set(FATHOM_SRC_CORE
  ${_r}/core/rng.c ${_r}/core/settings.c ${_r}/core/fsafe.c ${_r}/core/lang.c ${_r}/core/batlog.c
  ${_r}/core/wifitxt.c ${_r}/core/datetime.c ${_r}/core/tz.c ${_r}/core/intern.c)

# What of the library needs neither SQLite nor a decoder (the Mac's fathom_core has it)
set(FATHOM_SRC_LIBRARY_BASE
  ${_r}/library/keys.c ${_r}/library/scripts.c)

set(FATHOM_SRC_LIBRARY
  ${_r}/library/tags.c ${_r}/library/cue.c
  ${_r}/library/library.c ${_r}/library/library_browse.c ${_r}/library/library_level.c
  ${_r}/library/library_pixels.c ${_r}/library/library_playlists.c ${_r}/library/library_plays.c
  ${_r}/library/library_search.c ${_r}/library/library_trying.c ${_r}/library/library_upkeep.c
  ${_r}/library/reader.c ${_r}/library/artdec.c ${_r}/library/fatwalk.c ${_r}/library/playlist.c
  ${_r}/library/stb_impl.c)

set(FATHOM_SRC_SHUFFLE
  ${_r}/shuffle/shuffle.c)

# Opening and decoding a file or a stream. With FFmpeg (always on the H2),
# FATHOM_SRC_FFMPEG goes with it.
set(FATHOM_SRC_DECODE
  ${_r}/audio/decoder.c ${_r}/audio/dsd.c ${_r}/audio/vorbis.c ${_r}/audio/streamlist.c)
set(FATHOM_SRC_FFMPEG
  ${_r}/audio/ff_util.c)

set(FATHOM_SRC_NET
  ${_r}/net/http.c ${_r}/net/json.c ${_r}/net/radiodir.c ${_r}/net/radioicon.c ${_r}/net/podcast.c
  ${_r}/net/podsync.c ${_r}/net/coverfind.c ${_r}/net/netclock.c ${_r}/net/sha256.c ${_r}/net/unzip.c
  ${_r}/net/update.c ${_r}/net/packfetch.c)

set(FATHOM_SRC_ENGINE
  ${_r}/audio/engine.c ${_r}/audio/loudness.c ${_r}/audio/btconv.c ${_r}/audio/eq.c ${_r}/audio/tempo.c
  ${_r}/audio/crossfeed.c ${_r}/audio/out_null.c ${_r}/audio/out_clock.c)

set(FATHOM_SRC_PLAYER
  ${_r}/player/player.c ${_r}/player/radio.c ${_r}/player/rfmeta.c)

# The interface: what needs no LVGL, and what does
set(FATHOM_SRC_UI_BASE
  ${_r}/ui/theme.c ${_r}/ui/coverflow.c ${_r}/ui/glyphpack.c)
set(FATHOM_SRC_UI
  ${_r}/ui/fonts.c ${_r}/ui/assets.c ${_r}/ui/ui.c ${_r}/ui/ui_graphs.c)

# What a tool that only plays or measures a file needs beside the decoders
set(FATHOM_SRC_DECODE_TOOL
  ${FATHOM_SRC_DECODE} ${FATHOM_SRC_FFMPEG}
  ${_r}/library/cue.c ${_r}/net/http.c ${_r}/core/fsafe.c ${_r}/net/json.c)

unset(_r)
