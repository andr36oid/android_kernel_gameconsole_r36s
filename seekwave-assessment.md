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
and host wake GPIO0_A7. Its bluetooth-platdata node and UART are disabled in
stock; retain disabled status until an appropriate BT driver, firmware and
Android HAL are supplied. A UART definition cannot make Bluetooth work.

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
