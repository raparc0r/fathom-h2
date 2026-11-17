################################################################################
#
# libfreeaptx
#
################################################################################

LIBFREEAPTX_VERSION = 0.2.2
LIBFREEAPTX_SITE = $(call github,iamthehorker,libfreeaptx,$(LIBFREEAPTX_VERSION))
LIBFREEAPTX_LICENSE = LGPL-2.1-or-later
LIBFREEAPTX_LICENSE_FILES = COPYING
LIBFREEAPTX_INSTALL_STAGING = YES

LIBFREEAPTX_SO = libfreeaptx.so.$(LIBFREEAPTX_VERSION)

# just the library (not the command line tools)
define LIBFREEAPTX_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) PREFIX=/usr \
		$(LIBFREEAPTX_SO) libfreeaptx.so.0 libfreeaptx.so
endef

define LIBFREEAPTX_INSTALL_LIB
	$(INSTALL) -D -m 0755 $(@D)/$(LIBFREEAPTX_SO) $(1)/usr/lib/$(LIBFREEAPTX_SO)
	ln -sf $(LIBFREEAPTX_SO) $(1)/usr/lib/libfreeaptx.so.0
	ln -sf libfreeaptx.so.0 $(1)/usr/lib/libfreeaptx.so
endef

define LIBFREEAPTX_INSTALL_STAGING_CMDS
	$(call LIBFREEAPTX_INSTALL_LIB,$(STAGING_DIR))
	$(INSTALL) -D -m 0644 $(@D)/freeaptx.h $(STAGING_DIR)/usr/include/freeaptx.h
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_FATHOM_PATH)/package/libfreeaptx/libfreeaptx.pc \
		$(STAGING_DIR)/usr/lib/pkgconfig/libfreeaptx.pc
	$(SED) 's/@VERSION@/$(LIBFREEAPTX_VERSION)/' $(STAGING_DIR)/usr/lib/pkgconfig/libfreeaptx.pc
endef

define LIBFREEAPTX_INSTALL_TARGET_CMDS
	$(call LIBFREEAPTX_INSTALL_LIB,$(TARGET_DIR))
endef

$(eval $(generic-package))
