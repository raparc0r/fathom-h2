################################################################################
#
# fathom-system: small helpers from the repository's device/ folder, outside
# the player's CMake build:
#   fathom-usbrole      the USB-C port's role (S45usbrole), in the root
#                       filesystem; linked with the C library there, not
#                       statically: the initramfs is RAM, and libc is in it anyway
#   fathom-nocache.so   LD_PRELOADed into lighttpd and the SFTP server
#                       (device/nocache.c), installed to the card with the
#                       network drive (post-image.sh: fathom/share)
# Rebuild after editing them: fathom-system-rebuild.
#
################################################################################

FATHOM_SYSTEM_VERSION = local
FATHOM_SYSTEM_SITE = $(BR2_EXTERNAL_FATHOM_PATH)/../../device
FATHOM_SYSTEM_SITE_METHOD = local
FATHOM_SYSTEM_LICENSE = GPL-3.0-or-later
FATHOM_SYSTEM_INSTALL_TARGET = YES
FATHOM_SYSTEM_INSTALL_IMAGES = YES

define FATHOM_SYSTEM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -Wall -Wextra $(TARGET_LDFLAGS) \
		-o $(@D)/fathom-usbrole $(@D)/usbrole.c
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -Wall -Wextra -shared -fPIC $(TARGET_LDFLAGS) \
		-o $(@D)/fathom-nocache.so $(@D)/nocache.c
endef

define FATHOM_SYSTEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fathom-usbrole $(TARGET_DIR)/usr/sbin/fathom-usbrole
endef

define FATHOM_SYSTEM_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fathom-nocache.so $(BINARIES_DIR)/fathom-nocache.so
	$(TARGET_STRIP) $(BINARIES_DIR)/fathom-nocache.so
endef

$(eval $(generic-package))
