################################################################################
#
# cam2fb — UVC YUYV preview on /dev/fb0 (RGB565)
#
################################################################################

CAM2FB_VERSION = 1.0
CAM2FB_SITE = $(CAM2FB_PKGDIR)
CAM2FB_SITE_METHOD = local
CAM2FB_LICENSE = MIT

define CAM2FB_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -O2 -Wall -Wextra \
		-o $(@D)/cam2fb $(CAM2FB_PKGDIR)/cam2fb.c
endef

define CAM2FB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cam2fb $(TARGET_DIR)/usr/bin/cam2fb
endef

$(eval $(generic-package))
