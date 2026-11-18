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
