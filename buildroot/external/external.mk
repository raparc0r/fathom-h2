# SPDX-License-Identifier: GPL-3.0-or-later
include $(sort $(wildcard $(BR2_EXTERNAL_FATHOM_PATH)/package/*/*.mk))

# aptX for bluez-alsa through libfreeaptx (LGPL), not Buildroot's libopenaptx,
# whose 0.2.1 adds terms of its own to the GPL. Buildroot's bluez-alsa.mk knows
# only libopenaptx: its configure options are expanded when configure runs (so
# these come last and win), and the dependency goes on its configure step here.
# aptX HD stays off: it's 24-bit, and the player sends the link 16-bit.
# FFmpeg's https through mbedTLS (Buildroot's ffmpeg.mk knows only GnuTLS and
# OpenSSL): the option is in FFMPEG_EXTRACONF; the dependency goes here.
ifeq ($(BR2_PACKAGE_FFMPEG)$(BR2_PACKAGE_MBEDTLS),yy)
$(FFMPEG_TARGET_CONFIGURE): | mbedtls
endif

ifeq ($(BR2_PACKAGE_BLUEZ_ALSA)$(BR2_PACKAGE_LIBFREEAPTX),yy)
BLUEZ_ALSA_CONF_OPTS += --enable-aptx --disable-aptx-hd --with-libfreeaptx
$(BLUEZ_ALSA_TARGET_CONFIGURE): | libfreeaptx
endif

# The kernel's boot logo is Fathom's (assets/logo.png, converted to the
# kernel's 224-colour PPM). fbcon shows it while the system starts; the kernel
# command line keeps messages off the screen (post-image.sh).
define FATHOM_LINUX_LOGO
	python3 $(BR2_EXTERNAL_FATHOM_PATH)/../../tools/logo/png2ppm.py \
		$(BR2_EXTERNAL_FATHOM_PATH)/../../assets/logo.png \
		$(LINUX_DIR)/drivers/video/logo/logo_linux_clut224.ppm
endef
LINUX_PRE_BUILD_HOOKS += FATHOM_LINUX_LOGO

# musl's libc.so is in the initramfs (RAM for good): built for size. (With no
# -O flag musl would pick its own default, -O2 with -O3 for malloc and the
# string functions: 47 KB more. On MIPS memcpy, memset and the rest are C,
# already unrolled by hand: -O3 gives them the same code within a few
# instructions, so -Os costs them nothing.)
# (Changing this needs musl-dirclean.)
MUSL_EXTRA_CFLAGS += -Os

# SQLite and mbedTLS for size: about 1.7 MB of the player's code, nearly all
# cold (the player runs a few dozen kinds of query; TLS mostly sits in its
# record layer), and the player's code is page cache that competes with
# everything else in 24.5 MB. -Os takes 340 KB off the player; on a computer a
# scan and browsing were 0-6 % slower and AES-GCM faster, while ChaCha20 stays
# at -O2 (package-patches/mbedtls: at -Os it ran at half the speed). SQLite's
# flags are expanded when configure runs, so this, coming last, wins over
# Buildroot's -O2. mbedTLS's CMakeLists sets its own Release flags (-O2, after
# ours); its MinSizeRel is CMake's "-Os -DNDEBUG" (Release's NDEBUG too).
# lighttpd's mbedTLS (the network drive) is this build too.
# (Changing these needs sqlite-dirclean / mbedtls-dirclean.)
SQLITE_CFLAGS += -Os
MBEDTLS_CONF_OPTS += -DCMAKE_BUILD_TYPE=MinSizeRel

# The player's libraries, built as code for its static binary: where only the
# player links them (post-build.sh deletes their shared copies), they're built
# static only, as
#  - code for an executable (-mno-shared -mplt): GCC's MIPS default is code fit
#    for a shared library, every call and global reached through the GOT; direct
#    calls and absolute addresses are smaller and need fewer loads (the player's
#    own code is built the same way, device/CMakeLists.txt);
#  - one function per section, so the player's --gc-sections leaves out what it
#    never calls (FLAC's encoder, most of SQLite's API, ...).
# Code like this can't go into a shared library or a position-independent
# program (the toolchain's default), hence static only and -no-pie for the
# programs these packages build (configure's link tests, tools that aren't
# installed). SQLite (lighttpd's WebDAV), mpg123 and Opus (bluealsa) have
# shared users on the card: they keep their shared libraries and get only the
# sections, except that SQLite's static library is compiled again for the
# player (below). mbedTLS: a package patch does the same for its static
# libraries.
# (Changing these needs <package>-dirclean.)
FATHOM_LIB_CFLAGS = -mno-shared -mplt -ffunction-sections -fdata-sections
FATHOM_LIB_ENV = CFLAGS="$(TARGET_CFLAGS) $(FATHOM_LIB_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS) -no-pie"
# (Ogg FLAC goes through FFmpeg, so libFLAC needs no Ogg)
FLAC_CONF_OPTS += --disable-shared --disable-ogg
FLAC_CONF_ENV += $(FATHOM_LIB_ENV)
LIBOGG_CONF_OPTS += --disable-shared
LIBOGG_CONF_ENV += $(FATHOM_LIB_ENV)
TREMOR_CONF_OPTS += --disable-shared
TREMOR_CONF_ENV += $(FATHOM_LIB_ENV)
OPUS_CONF_OPTS += --disable-extra-programs
OPUS_CFLAGS += -ffunction-sections -fdata-sections
MPG123_CONF_ENV += CFLAGS="$(TARGET_CFLAGS) -ffunction-sections -fdata-sections"
JPEG_TURBO_CONF_OPTS += -DENABLE_SHARED=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) $(FATHOM_LIB_CFLAGS)" \
	-DCMAKE_EXE_LINKER_FLAGS="$(TARGET_LDFLAGS) -no-pie"
SQLITE_CFLAGS += -ffunction-sections -fdata-sections
FFMPEG_CONF_OPTS += --disable-shared --disable-pic --extra-ldflags=-no-pie
FFMPEG_CFLAGS += $(FATHOM_LIB_CFLAGS)

# FFmpeg for size (-Os, instead of its own -O3), except its decoders and what
# they call per frame, which stay at -O3 as before: the demuxers, protocols,
# option parsing and setup around them run a little per packet or once per
# track, the decoders all the time (on another computer, -Os made the AAC,
# MP3 and WavPack decoders 5-35 % slower). The list names objects in the build
# tree; a name that's wrong just builds that file for size. --enable-small
# also leaves out FFmpeg's long names and descriptions (35 KB) and picks
# smaller, slower CRC, AES and MD5 code: the Ogg demuxer's page checks and
# HLS's AES-128 run over a radio stream's few KB/s, and the decoders check
# CRCs only on headers (MP3's when present, FLAC's).
FFMPEG_CONF_OPTS += --enable-small --optflags=-Os
FATHOM_FFMPEG_HOT = \
	libavcodec/aacdec_fixed.o libavcodec/aacsbr_fixed.o libavcodec/aacps_fixed.o \
	libavcodec/aacpsdsp_fixed.o libavcodec/sbrdsp_fixed.o libavcodec/aacps_common.o \
	libavcodec/mpegaudiodec_fixed.o libavcodec/mpegaudiodsp.o \
	libavcodec/mpegaudiodsp_fixed.o libavcodec/dct32_fixed.o \
	libavcodec/alac.o libavcodec/alacdsp.o libavcodec/wavpack.o libavcodec/dsd.o \
	libavcodec/apedec.o libavcodec/lossless_audiodsp.o libavcodec/bswapdsp.o \
	libavcodec/tta.o libavcodec/ttadsp.o libavcodec/flacdec.o libavcodec/flacdsp.o \
	libavcodec/pcm.o libavcodec/libopusdec.o \
	libavutil/tx_int32.o libavutil/fixed_dsp.o libavutil/crc.o
define FATHOM_FFMPEG_HOT_O3
	echo '$(FATHOM_FFMPEG_HOT): CFLAGS += -O3' >> $(@D)/ffbuild/config.mak
endef
FFMPEG_POST_CONFIGURE_HOOKS += FATHOM_FFMPEG_HOT_O3

# The player's SQLite: the amalgamation compiled again after the package's
# own build, as code for an executable, one function per section, and without
# what the player's SQL never asks for: JSON, date and math functions,
# extensions, UTF-16, shared cache, tracing, progress callbacks and the
# deprecated or introspection APIs (only options that leave the amalgamation's
# parser alone). No memory statistics either: nothing reads them, and they
# cost a lock around every allocation. It replaces the staged libsqlite3.a,
# which only the player and its tools link; lighttpd's shared library is
# SQLite as built by the package. 250 KB of the player's code less.
FATHOM_SQLITE_PLAYER_OPTS = -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_JSON \
	-DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_DATETIME_FUNCS -DSQLITE_OMIT_UTF16 \
	-DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_PROGRESS_CALLBACK \
	-DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_DECLTYPE \
	-DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_DESERIALIZE \
	-DSQLITE_OMIT_COMPILEOPTION_DIAGS -DSQLITE_OMIT_INTROSPECTION_PRAGMAS \
	-DSQLITE_DEFAULT_MEMSTATUS=0
define FATHOM_SQLITE_PLAYER_LIB
	$(TARGET_CC) -c $(@D)/sqlite3.c -o $(@D)/sqlite3-player.o \
		$(TARGET_CFLAGS) -Os $(FATHOM_LIB_CFLAGS) $(FATHOM_SQLITE_PLAYER_OPTS)
	rm -f $(STAGING_DIR)/usr/lib/libsqlite3.a
	$(TARGET_AR) rcs $(STAGING_DIR)/usr/lib/libsqlite3.a $(@D)/sqlite3-player.o
endef
SQLITE_POST_INSTALL_STAGING_HOOKS += FATHOM_SQLITE_PLAYER_LIB

# mpg123: only the library (no programs), without what the player never asks
# of it: 8-bit and floating-point output, resampling and downsampling, the
# equalizer, ICY (radio goes through FFmpeg), frame analysis and console
# messages (it runs quiet). bluealsa's MP3 decoding (feeding a stream, 16-bit
# out) needs none of them either.
MPG123_CONF_OPTS += --disable-components --enable-libmpg123 \
	--disable-8bit --disable-real --disable-ntom --disable-downsample \
	--disable-equalizer --disable-icy --disable-moreinfo --disable-messages \
	--disable-feature_report

# busybox, half of the RAM disk (unreclaimable RAM): code for an executable, as
# for the player's libraries above, and so not position-independent. 102 KB of
# code less (678 -> 576 KB); busybox's own code is no longer at a random
# address (its libraries, stack and heap still are). musl's dynamic linker
# handles the PLT and copy relocations this needs. (busybox already builds
# itself -Oz, after Buildroot's -O2.) (Changing this needs busybox-dirclean.)
BUSYBOX_CFLAGS += -mno-shared -mplt
BUSYBOX_LDFLAGS += -no-pie
