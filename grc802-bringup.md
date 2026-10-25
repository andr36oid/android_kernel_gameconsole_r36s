# GRC802-MB V1.1 bring-up

## Phase 1

The only hardware source is `/andr36oid/work/reports/clone-issue-3/stock.dts`.
The generic dArkOS tree is not used. The new board includes the R36S Android
base for the SoC, power management and VOPB to DSI pipeline used by R46H.

Build and verify without rebuilding kernel host tools:

```
scripts/grc802/build-dtb.sh /tmp/ultra-x
```

Output: `/tmp/ultra-x/rk3326-r36s-clone-grc802.dtb`.
The script preprocesses with cpp, builds with the supplied prebuilt dtc,
decompiles, compiles stock separately and compares the resulting properties.
Verification passed: 798 initialization bytes, 8 exit bytes, all 13 timing
properties, panel delays/format/lanes/size/compatible, reset GPIO, all 18
key GPIOs and 3 rocker GPIOs, four ADC channels and Android density 304.
Warnings are inherited graph/unit-address issues, not build failures.

Panel power is stock GPIO2_A4, reset is GPIO3_D3, backlight PWM1 is 25000 ns.
Stock regulator voltages, battery table, charger values, speaker enable,
multicodecs card, headphone pin and UART5 debugger replace base board values.
The multicodecs driver is enabled in lineageos_r36s_defconfig. Its 4.19
implementation uses codec jack detection; the stock ADC headset-button
properties are retained but this version does not implement those controls.
Stock charge LED properties are not copied because the 4.19 battery driver
does not consume them and they overlap the radio pins.

The ADC drivers were inspected: odroidgo3 uses an analog multiplexer;
odroidgo2 and adc-joystick each read two direct channels. Two adc-joystick
instances use channels 0/1 and 2/3. The existing driver gained optional
`linux,abs-codes` (two distinct ABS codes) and `label` properties so the right
stick reports ABS_RX/ABS_RY while existing boards keep ABS_X/ABS_Y. These
are separate Linux input devices; Android input-device grouping and game
mapping must be checked on the console.

Buttons use the andr36oid codes from the base tree. The first 14 entries
follow the established EmuELEC positional order, with stock's changed R2/L2
pins. Extra entries are FN (GPIO3_D2), mode (GPIO2_B2), L3 (GPIO2_B5), R3
(GPIO2_B6). As requested, rockers GPIO2_B3/B7/C0 are buttons, provisionally
L3/FN/R3. Stock DTS alone does not identify those extra button functions;
physical labels, polarity and stick orientation still need evtest validation.
No multiplexer GPIOs are driven. Stock's volume ADC ladder on channel 2
conflicts with its right-X channel assignment; both are preserved, and that
stock ambiguity must be resolved on hardware if volume events follow X.
Stock also assigns ADC1 to headset detection, which this multicodecs driver
does not read.

Phase 1 was completed and verified before radio changes. The ARM64
adc-joystick and multicodecs objects compiled successfully with the board
configuration using aarch64-linux-gnu-gcc 13. No console is connected, so
screen operation, input events, charging and audio are not runtime verified.
The actual worktree is Linux 4.19.325 (Makefile), rather than the 4.19.183
specified in the report. All API and build checks target this worktree.

## Phase 2

See seekwave-assessment.md for the backport and its limits. The board's
SDIO node is on ff380000, with pwrseq on GPIO0_A2, stock WLAN host wake on
GPIO0_A1, 4-bit bus and 50 MHz maximum. The second-card-slot properties are
removed. UART1 uses stock transfer/CTS pins. Bluetooth platform data retains
stock reset/wake/host-wake/RTS pins and stays disabled, matching stock.

The compiled module is at:
`/tmp/ultra-x/kernel/drivers/net/wireless/seekwave/sv6160/sv6160.ko`.
Build it and the affected board/MMC objects with:

```
scripts/grc802/build-module.sh /tmp/ultra-x/kernel
```

The script uses the prepared reference Module.symvers (override SYMVERS)
and this worktree's config/headers. It is a module link check, not a full
Image build or a runtime load test. Copying this module alone onto an older
kernel does not install the host fixes. Rebuild and ship the matching kernel
Image when assembling the device image.

Firmware installation, with binaries kept outside Git:

```
make -f scripts/grc802/firmware.mk DESTDIR=/tmp/ultra-x/rootfs
make -f scripts/grc802/firmware.mk DESTDIR=/tmp/ultra-x/android-rootfs ANDROID=1
```

For the Android product build, include this AFTER the existing common device
product file:

```
include kernel/gameconsole/r36s/scripts/grc802/android-product.mk
```

GRC802_FIRMWARE_DIR defaults to the report's firmware directory; override it
on another build host. The fragment supplies both /vendor/firmware files,
replaces the SDIO HAL identification list and adds the sv6160 module list.
The current device init already aliases /lib/firmware to /vendor/firmware.
Install sv6160.ko into /vendor/lib/modules using the device's existing module
packaging. Do not load the older rk915 module for these SDIO IDs at the same
time. The product fragment should be selected only for this experimental
GRC802 build, since replacing the shared HAL list changes all RK915 entries.

The device source tree is read-only in this session, so that include and the
full image build remain pending. A staged rootfs and an Android vendor layout
were populated and verified in /tmp; the existing production image was not
modified. The final DTB is verified but no device has booted it here.

On the console after installing the matching kernel, DTB, module and firmware:

```
insmod /vendor/lib/modules/sv6160.ko
dmesg | grep -E 'sv6160|mmc|Firmware'
ls /sys/bus/sdio/devices
ip link show wlan0
```

Expected evidence to collect, not results observed here: an SDIO function-1
probe, logs naming both firmware files and their exact sizes, firmware-ready
state, and mac80211 registration. Retain complete boot logs and verify the
IRQ line. Check scan and association separately; neither is established by
successful upload or by wlan0 existing. Bluetooth is not operational from
this work alone. Do not set the mac_program module parameter during bring-up;
it is the upstream optional eFuse programming path and is unnecessary here.

A ready-to-apply product integration patch is included at
scripts/grc802/android-device.patch. Its applicability was checked against
the current device/gameconsole/common checkout with `git apply --check`.
It conditionally includes the fragment when GRC802_EXPERIMENTAL_RADIO=true;
apply it in that repository and enable the flag only for the GRC802 build.
It remains unapplied because that repository is read-only here.
