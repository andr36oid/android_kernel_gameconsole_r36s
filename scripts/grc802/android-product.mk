# Include AFTER device/gameconsole/common/device.mk for a GRC802 image.
# Firmware remains external to Git. Override this path in another build environment.
GRC802_FIRMWARE_DIR ?= /andr36oid/work/reports/clone-issue-3/sv6160-rk915/firmware
GRC802_INTEGRATION_PATH := $(call my-dir)

# Replace the existing RK915 HAL lookup with the experimental SV6160 module.
PRODUCT_COPY_FILES := $(filter-out %:$(TARGET_COPY_OUT_VENDOR)/etc/wifi_sdio_id_list.txt,$(PRODUCT_COPY_FILES))
PRODUCT_COPY_FILES += \
    $(GRC802_FIRMWARE_DIR)/ssv6160-fw.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ssv6160-fw.bin \
    $(GRC802_FIRMWARE_DIR)/ssv6160-patch.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ssv6160-patch.bin \
    $(GRC802_INTEGRATION_PATH)/wifi_sdio_id_list.txt:$(TARGET_COPY_OUT_VENDOR)/etc/wifi_sdio_id_list.txt \
    $(GRC802_INTEGRATION_PATH)/modprobe.sv6160:$(TARGET_COPY_OUT_VENDOR)/etc/modprobe.d/sv6160
