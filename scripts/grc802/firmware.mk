# make -f scripts/grc802/firmware.mk DESTDIR=/path/to/image/root [ANDROID=1]
GRC802_FIRMWARE_DIR ?= /andr36oid/work/reports/clone-issue-3/sv6160-rk915/firmware
GRC802_SCRIPT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
.PHONY: install
install:
	@test -n "$(DESTDIR)" || { echo "Set DESTDIR to the image staging root"; exit 1; }
	python3 "$(GRC802_SCRIPT_DIR)/install-firmware.py" "$(DESTDIR)" --source "$(GRC802_FIRMWARE_DIR)" $(if $(filter 1,$(ANDROID)),--android)
