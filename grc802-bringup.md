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
