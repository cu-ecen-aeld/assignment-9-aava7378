##############################################################
#
# AESD-CHAR-DRIVER
#
##############################################################

AESD_CHAR_DRIVER_VERSION = 1.0
AESD_CHAR_DRIVER_SITE = $(BR2_EXTERNAL_AAVA7378_PATH)/package/aesd-char-driver/src
AESD_CHAR_DRIVER_SITE_METHOD = local

define AESD_CHAR_DRIVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/aesdchar-start-stop $(TARGET_DIR)/etc/init.d/S98aesdchar
endef

$(eval $(kernel-module))
$(eval $(generic-package))
