################################################################################
#
# fathom-player: built straight from the repository's device/ folder (no copy),
# installed into the SD card image by post-image.sh. fathom-usbrole, from the
# same tree, goes into the root filesystem.
#
################################################################################

FATHOM_PLAYER_VERSION = local
FATHOM_PLAYER_SITE = $(BR2_EXTERNAL_FATHOM_PATH)/package/fathom-player/src
FATHOM_PLAYER_SITE_METHOD = local
FATHOM_PLAYER_LICENSE = GPL-3.0-or-later
FATHOM_PLAYER_DEPENDENCIES = sqlite flac mpg123 jpeg ffmpeg opus tremor host-pkgconf $(BR2_CMAKE_HOST_DEPENDENCY)
ifeq ($(BR2_PACKAGE_DBUS),y)
FATHOM_PLAYER_DEPENDENCIES += dbus   # fathom-avrcp
endif
FATHOM_PLAYER_INSTALL_TARGET = YES
FATHOM_PLAYER_INSTALL_IMAGES = YES

FATHOM_PLAYER_TREE = $(realpath $(BR2_EXTERNAL_FATHOM_PATH)/../../device)

define FATHOM_PLAYER_BUILD_CMDS
	$(BR2_CMAKE) -S $(FATHOM_PLAYER_TREE) -B $(@D)/build \
		-DCMAKE_TOOLCHAIN_FILE=$(HOST_DIR)/share/buildroot/toolchainfile.cmake \
		-DCMAKE_BUILD_TYPE=Release \
		-DPKG_CONFIG_EXECUTABLE=$(HOST_DIR)/bin/pkg-config \
		-DFETCHCONTENT_BASE_DIR=$(BR2_DL_DIR)/fathom-fetch
	$(BR2_CMAKE) --build $(@D)/build -j$(PARALLEL_JOBS)
endef

# fathom-usbrole goes into the root filesystem (S45usbrole starts it)
define FATHOM_PLAYER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/fathom-usbrole $(TARGET_DIR)/usr/sbin/fathom-usbrole
	$(TARGET_STRIP) $(TARGET_DIR)/usr/sbin/fathom-usbrole
endef

define FATHOM_PLAYER_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/fathom-player $(BINARIES_DIR)/fathom-player
	$(TARGET_STRIP) $(BINARIES_DIR)/fathom-player
	if [ -f $(@D)/build/fathom-avrcp ]; then \
		$(INSTALL) -D -m 0755 $(@D)/build/fathom-avrcp $(BINARIES_DIR)/fathom-avrcp && \
		$(TARGET_STRIP) $(BINARIES_DIR)/fathom-avrcp; fi
endef

$(eval $(generic-package))
