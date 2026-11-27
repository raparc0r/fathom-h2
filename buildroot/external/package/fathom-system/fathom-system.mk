################################################################################
#
# fathom-system: small helpers from the repository's device/ folder, outside
# the player's CMake build:
#   fathom-usbrole      the USB-C port's role (S45usbrole), in the root
#                       filesystem; linked with the C library there, not
#                       statically: the initramfs is RAM, and libc is in it anyway
#   fathom-cardout      the screen while the microSD card is out
#                       (fathom-sdcard), in the root filesystem: the card's
#                       programs can't be run then
#   fathom-nocache.so   LD_PRELOADed into lighttpd and the SFTP server
#                       (device/nocache.c), installed to the card with the
#                       network drive (post-image.sh: fathom/share)
# Rebuild after editing them: fathom-system-rebuild. (They use a few of the
# app's header-only helpers, core/clock.h: read from the source tree, -I
# below, since only device/ is copied to the build folder.)
#
################################################################################

FATHOM_SYSTEM_VERSION = local
FATHOM_SYSTEM_SITE = $(BR2_EXTERNAL_FATHOM_PATH)/../../device
FATHOM_SYSTEM_SITE_METHOD = local
FATHOM_SYSTEM_APP = $(BR2_EXTERNAL_FATHOM_PATH)/../../app
FATHOM_SYSTEM_LICENSE = GPL-3.0-or-later
FATHOM_SYSTEM_INSTALL_TARGET = YES
FATHOM_SYSTEM_INSTALL_IMAGES = YES

define FATHOM_SYSTEM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -Wall -Wextra -I$(FATHOM_SYSTEM_APP) $(TARGET_LDFLAGS) \
		-o $(@D)/fathom-usbrole $(@D)/usbrole.c
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -Wall -Wextra -I$(FATHOM_SYSTEM_APP) $(TARGET_LDFLAGS) \
		-o $(@D)/fathom-cardout $(@D)/cardout.c
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -Wall -Wextra -shared -fPIC $(TARGET_LDFLAGS) \
		-o $(@D)/fathom-nocache.so $(@D)/nocache.c
endef

define FATHOM_SYSTEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fathom-usbrole $(TARGET_DIR)/usr/sbin/fathom-usbrole
	$(INSTALL) -D -m 0755 $(@D)/fathom-cardout $(TARGET_DIR)/usr/sbin/fathom-cardout
endef

define FATHOM_SYSTEM_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fathom-nocache.so $(BINARIES_DIR)/fathom-nocache.so
	$(TARGET_STRIP) $(BINARIES_DIR)/fathom-nocache.so
endef

$(eval $(generic-package))
