# Seekwave SV6160 backport assessment

## Scope and baseline, before importing the driver

Source: `/andr36oid/work/reports/clone-issue-3/sv6160-rk915/`, local
clean-room mac80211 driver targeting Linux 6.18+. Read README,
SDIO_ARCHITECTURE, VENDOR-DRIVER-FINDINGS, the host patch, overlay and conf.
This worktree identifies itself as Linux 4.19.325, not the report's
4.19.183. Headers and existing implementations here are the compilation
baseline. Phase 1 was built and verified first; see grc802-bringup.md.

This is an experimental transport/firmware bring-up port. Upstream reports
SDIO data exchange and firmware boot, with scanning still being developed.
Association, payload traffic and WPA are not proven by that status. Building
here cannot establish any of those runtime capabilities. No target console
is attached. Bluetooth is not implemented by this Wi-Fi driver.

## API comparison and complete shim plan for this source snapshot

| Source use | Worktree 4.19 interface | Required adaptation |
| --- | --- | --- |
| linux/unaligned.h | asm/unaligned.h | Change include, keep get_unaligned_le32/le64 helpers |
| ops.stop(hw, suspend) | stop(hw) | Remove suspend argument |
| ops.config(hw, radio_idx, changed) | config(hw, changed) | Remove radio index |
| bss_info_changed(..., u64 changed) | u32 changed | Use u32, all used BSS_CHANGED bits fit |
| vif->cfg.assoc, vif->cfg.aid | bss_conf.assoc, bss_conf.aid | Read info->assoc and info->aid |
| info->chanreq.oper.chan | info->chandef.chan | Use chandef, guard missing channel |
| conf_tx(..., link_id, queue, params) | conf_tx(..., queue, params) | Remove link ID |
| ieee80211_beacon_get(hw, vif, link_id) | beacon_get(hw, vif) | Remove link ID |
| ieee80211_handle_wake_tx_queue | Not present | Omit wake_tx_queue, use existing legacy .tx path |
| ieee80211_emulate_add/remove/change_chanctx | Not present | Omit chanctx ops, use single-channel .config |
| timer_container_of | from_timer | Use from_timer(sc, t, bcn_timer) |
| timer_delete_sync | del_timer_sync | Use del_timer_sync |
| DEFINE_SIMPLE_DEV_PM_OPS, pm_ptr | SIMPLE_DEV_PM_OPS, direct pointer | Use old macro and &pm_ops |

No cfg80211_ops are registered directly. cfg80211_scan_request, scan_info,
ieee80211_scan_completed(hw, &info), hw_scan/cancel_hw_scan, scan SSID/channel
arrays, regulatory_hint, remain-on-channel, RX status fields, TX status,
queue helpers, key flags/ciphers, AMPDU callbacks and band/HT capability
fields used here are available. No global cfg80211/mac80211 backport or MLO
emulation is needed. Advertising two interfaces contradicts the source's
single sc->vif; restrict advertised concurrency to one interface.

SDIO claim/release host/IRQ, enable/disable func, CMD52 byte access, CMD53
memcpy/readsb, set_block_size, lenient function-0 quirk, PM flags and SDIO
of_node assignment are present. mmc_host parent resources and devm_ioremap
are available. Runtime PM entry points are unchanged. timer_setup, mod_timer,
INIT_WORK/INIT_DELAYED_WORK, schedule/cancel[_delayed]_work[_sync], completion,
jiffies conversion, skb queues, debugfs and devm IRQ functions are available.
No workqueue scheduling shim is needed; teardown ordering needs repair.

Kconfig will depend on MAC80211, MMC and ARM64 or COMPILE_TEST and select
FW_LOADER. The upstream forced ARM64 crypto selections and soft dependency
are unnecessary for the transport and break the advertised COMPILE_TEST
case; mac80211 provides software crypto according to normal kernel config.

## SDIO host compatibility

The tree ALREADY has MMC_CAP2_WIFI_RK915 (bit 28), `supports-rk915`, disabled
DW MMC low-power clock gating, relaxed RK915 CIS handling, a 512-byte block
size override and the SDIO resume power quirk. Do not duplicate the upstream
RK912 bit 29. Stock uses `supports-rk912`; alias it to the existing RK915
capability. Port the CMD52 previous-data wait into the command-selection
chain so CCCR_ABORT retains STOP without waiting for the data it must abort.
The preexisting implementation incorrectly also waits for abort commands.
Guard the relaxed CIS parser's buf[12]/buf[13] access with size >= 14.
These changes must remain limited to explicitly opted-in hosts.

## Runtime defects to address while importing

The source requests IRQs before initializing rx_work, completions and queues;
firmware can signal immediately. Initialize core state before registering any
IRQ. Track SDIO IRQ ownership, disable/free both IRQ sources before freeing
hw state, cancel every timer/work item on probe errors and removal, then
unregister/free mac80211. devm IRQ release after ieee80211_free_hw is too late.
GPIO IRQ disable depth before firmware readiness also needs balanced handling.
SDIO function timeout must be corrected before sdio_enable_func, not after.
These are load/unload correctness fixes, not claims of functioning RF.

## Board and firmware plan

Stock SDIO controller: ff380000 (&sdio), 4 bits, 50 MHz. Remove inherited
second-card-slot/CD and removable-SD supplies. Enable non-removable SDIO and
use GPIO0_A2 for active-high chip enable, represented as an active-low pwrseq
reset. Stock WLAN host wake is GPIO0_A1. The disabled seekwcn_boot node also
lists a different host wake, GPIO0_A5; preserve this discrepancy in notes and
use the enabled wlan-platdata pin A1 first. Do not copy XF40H overlay A5
silently. Avoid two active drivers owning the same power GPIO.

Stock Bluetooth describes UART1 RTS GPIO1_C3, reset GPIO0_A0, wake GPIO0_A1
and host wake GPIO0_A7. Its bluetooth-platdata node is disabled in stock; UART1 is enabled with
transfer and CTS pins. Retain these states until an appropriate BT driver,
firmware and Android HAL are supplied. A UART definition cannot make Bluetooth work.

Driver firmware requests are exactly `ssv6160-fw.bin` (48056 bytes) and
`ssv6160-patch.bin` (16716 bytes); patch can be overridden by module parameter.
The image must contain them in /lib/firmware. Keep binary firmware out of
history by providing an image-staging rule/script that copies and verifies
the report files. Android device files and existing image outputs are outside
this session's writable roots, so direct modification there is unavailable.
A checked-in kernel-side staging hook and a staged writable rootfs will be
provided with exact integration instructions.

## Additional references inspected

- https://github.com/ethersloth/c20e-rk3562-linux : modern RK3562 integration,
  useful for packaging only. The specifically named wifi-only preparation
  script could not be retrieved through the web tool during assessment.
- https://github.com/WLTB-Gino/swt6621s : vendor USB full-MAC driver, separate
  BSP/core modules and different firmware names. It is not a drop-in SDIO
  mac80211 backport or a source of this board's pins.
- https://wiki.neardi.net/docs/neardi-wifi/wifi-adapt/adaptation/ : vendor
  integrations separate bus, WLAN and BT drivers and require matching
  transport firmware. This supports treating BT as a separate unfinished port.
- https://gitlab.com/rockchip_android14_radxa_rkr6/rk/hardware/seekwave :
  retrieval failed. No uninspected Android HAL support is claimed.

## Commit storage limitation

The worktree's Git metadata resolves under /andr36oid/src/.repo, read-only in
this session. Commits are preserved on night/ultra-x in
/tmp/ultra-x/commits.git with author itskenny0 <0@kenny.cat>; a bundle will
allow importing them into the real branch. The worktree's real branch ref
cannot be advanced from this session.

## Additional gaps confirmed by the first compile

- module_sdio_driver is absent: use module_driver with SDIO register/unregister.
- cancel_remain_on_channel takes only hw in 4.19: remove vif argument.
- set_rts_threshold has no radio index: remove it.
- IEEE80211_AMPDU_TX_START_IMMEDIATE is absent: signal readiness with
  ieee80211_start_tx_ba_cb_irqsafe and return 0.
- Include linux/mmc/card.h explicitly for mmc_card fields and quirks.
- Remove the upstream extra RX log argument and unused read_next label.

The one-interface restriction is implemented by omitting iface_combinations,
which defaults to one interface. 4.19 rejects explicit max_interfaces=1.
The upstream hard-coded NZ regulatory hint is removed; normal cfg80211
regulatory handling applies. Firmware boot now precedes core initialization
and both IRQ requests, so no uninitialized worker can be scheduled during
upload. Core initialization succeeds before any IRQ source is registered.

The ARM64 config enables VMAP_STACK. The imported driver's firmware start
command and other commands can be stack-backed, which is invalid for MMC
DMA. The common write helper now copies to an aligned, zero-padded kmalloc
buffer before CMD53. This also avoids reading past an unaligned command's
allocation. Firmware boot now fails explicitly when the required patch is
missing, and does not accept an uninitialized firmware-state value after a
failed read. Direct DW MMC MMIO mapping is limited to the PX30 platform host.

## Final validation and remaining work

Source revision: 3160b3242fe3155eaf6a5458943275b57c3dcd1f.

Completed in this worktree:

- CONFIG_SV6160=m through lineageos_r36s_defconfig, wired through parent
  Kconfig and Makefiles. Existing RK915 is retained for other boards; the
  GRC802 Android module list selects only sv6160, not rfkill-rk or rk915.
- ARM64 module compiled and linked without warnings, including MODPOST,
  with aarch64-linux-gnu-gcc 13.3.0 and this worktree's generated headers.
  The prepared reference KERNEL_OBJ/Module.symvers supplies kernel exports;
  a full kernel Image was not rebuilt. `LOCALVERSION=` matches the reference
  release 4.19.325-cip136-rk3326-andr36oid-itskenny0. MODVERSIONS is off.
  Reproduction: `scripts/grc802/build-module.sh /tmp/ultra-x/kernel`.
- ADC joystick, multicodecs, DW MMC, MMC host-property parser and CIS parser
  objects compiled for ARM64. Source and resulting module have all three
  supported SDIO aliases: 3030:3030, 0296:5348 and 0296:5347.
- Phase 2 DTB rebuilt with cpp/prebuilt dtc and decompiled. Panel/input
  comparisons still pass; verification also compares stock SDIO frequency,
  width, power/wake pins, BT pins and enabled UART1 against the output.
- Firmware staged to /tmp/ultra-x/rootfs/lib/firmware, and separately into
  /tmp/ultra-x/android-rootfs/vendor/firmware with /lib/firmware pointing
  to /vendor/firmware. Sizes and report digests were checked before copying
  and after installation. No firmware binary or DTB is committed.

Artifacts:

- /tmp/ultra-x/rk3326-r36s-clone-grc802.dtb
- /tmp/ultra-x/kernel/drivers/net/wireless/seekwave/sv6160/sv6160.ko
- /tmp/ultra-x/final-module-build.log
- /tmp/ultra-x/phase2-verify.log

The host is x86_64, has no adb tool and exposes no target USB/serial device.
Module loading on the console, SDIO enumeration, firmware upload, scan,
association, data traffic, suspend/resume and unload are NOT runtime tested.
Do not describe Wi-Fi as working. Upstream itself has not established full
scan/association/data support, and reports sustained-RX SDIO stalls in the
related vendor implementation. Bluetooth remains disabled at the platform
node, with stock UART1 enabled; its transport/firmware/HAL port remains undone.

Stock's enabled wlan binding uses A1 for host wake, while the disabled boot
binding uses A5. This port uses A1 and retains the alternate disabled node.
The disabled wlan-platdata node preserves stock pins but cannot compete with
mmc-pwrseq for A2 power control. Verify the actual IRQ line on the console.
The retained stock BT wake A1 is shared with Wi-Fi; enabling BT blindly is
not an implementation of combo-chip arbitration.

Android device repository changes and image assembly could not be applied
outside the writable worktree. The concrete packaging fragment is
scripts/grc802/android-product.mk. Its include, module installation and target
checks are described in grc802-bringup.md. Firmware staging is verified;
a final flashable Android image containing these changes is NOT built.

The optional scripts/grc802/android-device.patch passes `git apply --check`
against the current common device repository. It adds an opt-in product
include guarded by GRC802_EXPERIMENTAL_RADIO=true. The packaging fragment
was also evaluated with GNU make: it replaces exactly the SDIO HAL list,
retains other copy rules, and resolves all four added source files. This is
a prepared integration patch, not an applied Android product change.
