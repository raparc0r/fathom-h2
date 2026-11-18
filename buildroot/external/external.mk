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

# musl's libc.so is in the initramfs (RAM for good): built for size, musl's
# own default without Buildroot's -O2 (which musl keeps only for its malloc
# and string code; memcpy and memset are assembly on MIPS either way).
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
