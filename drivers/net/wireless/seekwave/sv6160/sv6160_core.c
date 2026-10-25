// SPDX-License-Identifier: GPL-2.0
/*
 * Seekwave SV6160 / Rockchip RK915 WiFi Driver - Core
 * Copyright (c) 2025 Joel Wirāmu Pauling <aenertia@aenertia.net>
 *
 * SDIO mailbox/FIFO protocol + RPU firmware command protocol.
 * Based on stolen/rk915 vendor BSP architecture.
 */
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/etherdevice.h>
#include <linux/moduleparam.h>
#include <linux/ieee80211.h>
#include <linux/mmc/sdio.h>
#include <asm/unaligned.h>
#include <net/ieee80211_radiotap.h>
#include "sv6160.h"

/* Module Parameters */
static bool mod_coalescing = true;
module_param_named(coalescing, mod_coalescing, bool, 0644);
MODULE_PARM_DESC(coalescing, "Enable IRQ coalescing (batch processing)");

static int mod_rx_bundle = 16;
module_param_named(rx_bundle, mod_rx_bundle, int, 0644);
MODULE_PARM_DESC(rx_bundle, "Max RX packets to process per interrupt (default 16)");

static char *mod_mac_program;
module_param_named(mac_program, mod_mac_program, charp, 0444);
MODULE_PARM_DESC(mac_program, "Program eFuse MAC if blank (Format: xx:xx:xx:xx:xx:xx)");

static char *mod_patch_name = SV6160_PATCH_NAME;
module_param_named(patch, mod_patch_name, charp, 0444);
MODULE_PARM_DESC(patch, "Firmware patch file name (default: ssv6160-patch.bin)");

static int mod_sdio_blksize = SV6160_SDIO_BLOCK_SIZE;
module_param_named(sdio_blksize, mod_sdio_blksize, int, 0444);
MODULE_PARM_DESC(sdio_blksize, "SDIO block size (default 512, try 64/128 for read issues)");

static int mod_scan_single_ch;
module_param_named(scan_single_ch, mod_scan_single_ch, int, 0644);
MODULE_PARM_DESC(scan_single_ch, "Override scan to single channel (0=use all, 1-14=channel)");

static bool mod_scan_passive;
module_param_named(scan_passive, mod_scan_passive, bool, 0644);
MODULE_PARM_DESC(scan_passive, "Force all scan channels to passive mode");

int sv6160_get_sdio_blksize(void) { return mod_sdio_blksize; }

/* Forward declarations */
static void sv6160_idle_work_fn(struct work_struct *work);
static inline void sv6160_mark_active(struct sv6160_softc *sc);

/* Trigger RX worker from polling context.
 * Does NOT reset stale guard - init polling must not read stale
 * RECV_LEN (causes -110 that permanently kills DW MMC bus).
 * Callers that KNOW new data is available (scan poll after firmware
 * TX path is enabled) should reset last_recv_len=0 before calling. */
static void sv6160_trigger_rx(struct sv6160_softc *sc)
{
	if (READ_ONCE(sc->removing))
		return;
	sc->gpio_irq_pending = true;
	schedule_work(&sc->rx_work);
}

/* 2.4 GHz Channel Table (channels 1-14) */
static const struct ieee80211_channel sv6160_2ghz_channels[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2484, .hw_value = 14 },
};

/* 802.11b/g rates */
static const struct ieee80211_rate sv6160_2ghz_rates[] = {
	{ .bitrate = 10,  .hw_value = 0,  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 20,  .hw_value = 1,  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55,  .hw_value = 2,  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value = 3,  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60,  .hw_value = 4  },
	{ .bitrate = 90,  .hw_value = 5  },
	{ .bitrate = 120, .hw_value = 6  },
	{ .bitrate = 180, .hw_value = 7  },
	{ .bitrate = 240, .hw_value = 8  },
	{ .bitrate = 360, .hw_value = 9  },
	{ .bitrate = 480, .hw_value = 10 },
	{ .bitrate = 540, .hw_value = 11 },
};

/* ===================================================================
 * SDIO I/O - Mailbox Register Protocol
 *
 * The vendor driver polls register 35 (REQ_INT_STA) until it reads 0
 * after each register write.  This handshake ensures the chip has
 * processed the write before the host issues the next command.
 * =================================================================== */

/* Wait for write completion - must be called with host claimed */
#define SDIO_REG_WRITE_COMP  35
#define SDIO_WRITE_COMP_TIMEOUT_MS 500

static void sv6160_sdio_write_comp(struct sdio_func *func)
{
	int ret, tries = 5000;  /* 500ms at 100us per poll - matches stolen BSP */
	u8 val;

	do {
		val = sdio_readb(func, SDIO_REG_WRITE_COMP, &ret);
		if (ret || val == 0)
			return;
		udelay(100);
	} while (--tries > 0);
}

int sv6160_sdio_writeb(struct sv6160_softc *sc, u8 addr, u8 val)
{
	int ret;

	sdio_claim_host(sc->func);
	sdio_writeb(sc->func, val, addr, &ret);
	if (!ret)
		sv6160_sdio_write_comp(sc->func);
	sdio_release_host(sc->func);

	if (ret)
		dev_err(sc->dev, "sdio_writeb(0x%02x, 0x%02x) failed: %d\n",
			addr, val, ret);
	return ret;
}

int sv6160_sdio_readb(struct sv6160_softc *sc, u8 addr, u8 *val)
{
	int ret;

	sdio_claim_host(sc->func);
	*val = sdio_readb(sc->func, addr, &ret);
	sdio_release_host(sc->func);

	if (ret)
		dev_err(sc->dev, "sdio_readb(0x%02x) failed: %d\n", addr, ret);
	return ret;
}

int sv6160_sdio_write_data(struct sv6160_softc *sc, u32 addr,
			   void *buf, size_t len)
{
	int ret;
	void *dma_buf;
	size_t aligned = ALIGN(len, 4);

	/* Commands can live on the vmapped ARM64 stack. MMC needs a DMA-safe
	 * buffer, and padding must not read beyond the caller's allocation. */
	if (!len || aligned < len)
		return -EINVAL;
	dma_buf = kzalloc(aligned, GFP_KERNEL);
	if (!dma_buf)
		return -ENOMEM;
	memcpy(dma_buf, buf, len);

	sdio_claim_host(sc->func);
	ret = sdio_memcpy_toio(sc->func, addr, dma_buf, aligned);
	sdio_release_host(sc->func);
	kfree(dma_buf);

	if (ret)
		dev_err(sc->dev, "sdio_write_data(0x%05x, %zu) failed: %d\n",
			addr, len, ret);
	return ret;
}

int sv6160_sdio_read_data(struct sv6160_softc *sc, u32 addr,
			  void *buf, size_t len)
{
	int ret;

	sdio_claim_host(sc->func);
	ret = sdio_memcpy_fromio(sc->func, buf, addr, ALIGN(len, 4));
	sdio_release_host(sc->func);

	if (ret)
		dev_err(sc->dev, "sdio_read_data(0x%05x, %zu) failed: %d\n",
			addr, len, ret);
	return ret;
}

/* ===================================================================
 * RPU Command Protocol
 *
 * Commands are sent as packed structs to SDIO address 0.
 * The firmware processes them and returns events via the same FIFO.
 * =================================================================== */

/* PKT_INFO values written to reg 17 before CMD53 writes */
#define PKT_INFO_CMD    0x01
#define PKT_INFO_DATA   0x10
#define PKT_INFO_PATCH  0x11

int sv6160_rpu_send_cmd(struct sv6160_softc *sc, void *cmd, size_t len,
			u32 id)
{
	struct rpu_msg_hdr *hdr = cmd;
	int ret;

	/* Drain any in-flight rx_work before sending a new command.
	 * Without this, a concurrent rx_worker may attempt CMD53 reads
	 * while we're doing a CMD53 write, causing bus collisions that
	 * result in -110 timeout and permanent DW MMC bus wedge.
	 * Skip if called from within rx_work context (e.g., mark_active
	 * → ps_config → send_cmd) to avoid deadlock. */
	if (!current_work() || current_work() != &sc->rx_work)
		cancel_work_sync(&sc->rx_work);

	memset(hdr->hal_data, 0, HAL_PRIV_DATA_SIZE);
	hdr->descriptor_id = cpu_to_le32(0x0000ffff);
	hdr->payload_length = cpu_to_le32(0);
	hdr->id = cpu_to_le32(id);
	hdr->length = cpu_to_le32(len);
	hdr->more_cmd_data = cpu_to_le32(0);

	sc->recv_len_failed = false;

	ret = sv6160_sdio_write_data(sc, SDIO_DATA_ADDR, cmd, len);

	/* Do NOT reset last_recv_len here.  With CCCR 0x16=0x80 GPIO
	 * mode enabled, the firmware asserts GPIO when it has a response
	 * ready.  The GPIO handler signals gpio_irq_pending which lets
	 * rx_worker proceed.  Resetting the stale guard here allows
	 * polling-triggered reads to pass on stale RECV_LEN before the
	 * firmware has prepared the response, causing -110. */
	return ret;
}

/* Send RPU_CMD_RESET to initialize the LMAC */
int sv6160_rpu_reset(struct sv6160_softc *sc)
{
	struct rpu_cmd_reset cmd;
	int ret;

	/*
	 * RF calibration parameters from gamely BSP (proc_init.c).
	 * 94 bytes: TX power tables (66 bytes) + TX power offsets (14 bytes)
	 * + PHY sensitivity thresholds per channel (14 bytes).
	 * Without these, LMAC calibration leaves the radio unable to receive.
	 */
	static const u8 rf_params_base[94] = {
		/* TX power calibration tables (bytes 0-65) */
		0x00, 0x20, 0x40, 0x00, 0x24, 0x02, 0x10, 0x02,
		0x04, 0x03, 0x04, 0x04, 0x04, 0x05, 0x04, 0x06,
		0x04, 0x07, 0x04, 0x08, 0x04, 0x09, 0x04, 0x0A,
		0x04, 0x0B, 0x04, 0x0C, 0x04, 0x0C, 0xF8, 0x0B,
		0x0C, 0x04,
		0xFF, 0xFA, 0xF2, 0xEC, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		/* TX power offsets (bytes 66-79), 14 x 0x08 */
		0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
		0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
		/* PHY sensitivity thresholds per channel (bytes 80-93)
		 * Original: 14 x 0x8C (-70 dBm)
		 * Overwritten below with default_phy_threshold=180 (0xB4) */
		0x8C, 0x8C, 0x8C, 0x8C, 0x8C, 0x8C, 0x8C, 0x8C,
		0x8C, 0x8C, 0x8C, 0x8C, 0x8C, 0x8C,
	};
	int i;

	memset(&cmd, 0, sizeof(cmd));
	/* LMAC_DO_CALIB (0x10) + LOAD_FACTORY_CAL (0x40) are required for the
	 * firmware to configure GPIO interrupt assertion.
	 * LMAC_NO_SLEEP (0x20) prevents firmware sleep during RX processing
	 * (gamely lpw_no_sleep=1). */
	cmd.type = cpu_to_le32(LMAC_ENABLE | LOAD_FACTORY_CAL |
			       LMAC_DO_CALIB | LMAC_NO_SLEEP);
	/* Gamely BSP sets LMAC_DO_CALIB in BOTH type AND lmac_mode.
	 * The lmac_mode calibration flag may enable the RF amplifier
	 * during reset - without it, RF stays disabled after boot. */
	cmd.lmac_mode = cpu_to_le32(LMAC_MODE_NORMAL | LMAC_DO_CALIB);
	cmd.reserv[0] = cpu_to_le32(0x01b50052);  /* magic from vendor */
	cmd.reserv[1] = cpu_to_le32(0x017901b5);  /* magic from vendor */
	cmd.num_spatial_streams = 1;

	/* Populate RF params - gamely fills all 369 bytes with 0xFF first,
	 * then overlays calibration data from rk915_rf_para.txt.
	 * Without the 0xFF fill, the firmware reads 0x00 for uncalibrated
	 * parameters which may prevent the radio from operating. */
	memset(cmd.rf_params, 0xFF, SV6160_RF_PARAMS_SIZE);
	memcpy(cmd.rf_params, rf_params_base, sizeof(rf_params_base));

	/* Apply default_phy_threshold=180 (0xB4, approx -90 dBm) to
	 * bytes 80-93 - matches gamely set_default_phy_thresh(rf_params, 94) */
	for (i = 80; i < 94; i++)
		cmd.rf_params[i] = 180;

	reinit_completion(&sc->reset_done);

	ret = sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), FW_PKT_TYPE_INIT);
	if (ret)
		return ret;

	/* Wait for RPU_EVENT_RESET_COMPLETE.
	 * Poll the RX worker manually since SDIO interrupts may not
	 * be firing during early init. */
	{
		int polls = 50;

		while (polls-- > 0) {
			sv6160_trigger_rx(sc);
			if (wait_for_completion_timeout(&sc->reset_done,
						       msecs_to_jiffies(100)))
				break;
			/* Do NOT reset last_recv_len - firmware keeps stale
			 * RECV_LEN after clear+notify. Resetting causes -110
			 * on the next poll. Responses with different sizes
			 * get through the stale guard naturally. */
		}
		if (polls <= 0) {
			dev_err(sc->dev, "RPU reset timeout\n");
			return -ETIMEDOUT;
		}
	}

	sc->is_rpu_ready = true;
	dev_info(sc->dev, "RPU LMAC initialized\n");
	return 0;
}

/* Add a virtual interface (STA/AP) */
int sv6160_rpu_add_vif(struct sv6160_softc *sc, const u8 *mac_addr, int mode)
{
	struct rpu_cmd_vif_ctrl cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.if_ctrl = cpu_to_le32(VIF_ADD);
	cmd.if_index = cpu_to_le32(0);
	cmd.mode = cpu_to_le32(mode);
	memcpy(cmd.mac_addr, mac_addr, 6);

	sc->vif_index = 0;
	/* RPU_CMD_VIF_CTRL (15) → firmware tier-2 LMAC processor (verified) */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_VIF_CTRL);
}

/* Remove virtual interface */
int sv6160_rpu_remove_vif(struct sv6160_softc *sc)
{
	struct rpu_cmd_vif_ctrl cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.if_ctrl = cpu_to_le32(VIF_REMOVE);
	cmd.if_index = cpu_to_le32(sc->vif_index);

	/* RPU_CMD_VIF_CTRL (15) → firmware tier-2 LMAC processor (verified) */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_VIF_CTRL);
}

/* Send scan command */
int sv6160_rpu_scan(struct sv6160_softc *sc,
		    struct ieee80211_scan_request *req)
{
	struct rpu_cmd_scan *cmd;
	struct cfg80211_scan_request *sreq = &req->req;
	size_t cmd_size;
	int i, ret;

	cmd_size = sizeof(*cmd) + sreq->ie_len;
	cmd = kzalloc(cmd_size, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	cmd->if_index = cpu_to_le32(sc->vif_index);
	cmd->type = cpu_to_le32(SCAN_ACTIVE);
	cmd->n_channel = cpu_to_le32(min_t(int, sreq->n_channels,
					   SV6160_MAX_SCAN_CHANNELS));

	for (i = 0; i < le32_to_cpu(cmd->n_channel); i++) {
		cmd->channel_list[i] = sreq->channels[i]->hw_value;
		cmd->chan_max_power[i] = 20; /* dBm */
		if (sreq->channels[i]->flags & IEEE80211_CHAN_NO_IR)
			cmd->chan_flags[i] = SCAN_PASSIVE;
		else
			cmd->chan_flags[i] = SCAN_ACTIVE;
	}

	/* Debug: override scan parameters for testing */
	if (mod_scan_single_ch > 0 && mod_scan_single_ch <= 14) {
		cmd->n_channel = cpu_to_le32(1);
		cmd->channel_list[0] = mod_scan_single_ch;
		cmd->chan_max_power[0] = 20;
		cmd->chan_flags[0] = mod_scan_passive ? SCAN_PASSIVE : SCAN_ACTIVE;
		dev_info(sc->dev, "scan override: single ch %d %s\n",
			 mod_scan_single_ch, mod_scan_passive ? "passive" : "active");
	}
	if (mod_scan_passive) {
		int j;
		cmd->type = cpu_to_le32(SCAN_PASSIVE);
		for (j = 0; j < le32_to_cpu(cmd->n_channel); j++)
			cmd->chan_flags[j] = SCAN_PASSIVE;
	}

	/* Populate SSID list - firmware expects ssids[4] array.
	 * For wildcard scan, n_ssids=1 with ssid_len=0. */
	cmd->n_ssids = cpu_to_le32(max_t(int, sreq->n_ssids, 1));
	for (i = 0; i < min_t(int, sreq->n_ssids, SV6160_MAX_NUM_SSIDS); i++) {
		cmd->ssids[i].len = cpu_to_le32(sreq->ssids[i].ssid_len);
		if (sreq->ssids[i].ssid_len > 0)
			memcpy(cmd->ssids[i].ssid, sreq->ssids[i].ssid,
			       min_t(size_t, sreq->ssids[i].ssid_len,
				     SV6160_MAX_SSID_LEN));
	}

	cmd->extra_ies_len = cpu_to_le32(sreq->ie_len);
	if (sreq->ie_len)
		memcpy(cmd->extra_ies, sreq->ie, sreq->ie_len);

	/* Skip pre-scan channel programming - RPU_CMD_SCAN includes the
	 * channel list and the LMAC tier-2 processor handles channel
	 * switching internally. Pre-programming caused CH_PROG_DONE
	 * timeout (28-byte response blocked by stale guard). */

	sc->scan_request = req;
	sc->scan_in_progress = true;
	/* Use RPU_CMD_SCAN (1) - goes through tier-2 LMAC processor.
	 * Gamely uses this path (NOT pkt_type 0x16 which breaks GPIO).
	 * The LMAC processor handles scan AND WiFi RX frame delivery. */
	ret = sv6160_rpu_send_cmd(sc, cmd, cmd_size, RPU_CMD_SCAN);
	dev_info(sc->dev, "scan sent: %d ch, id=RPU_CMD_SCAN(%d), size=%zu, ret=%d\n",
		 le32_to_cpu(cmd->n_channel), RPU_CMD_SCAN, cmd_size, ret);
	if (ret) {
		sc->scan_request = NULL;
		sc->scan_in_progress = false;
	}

	kfree(cmd);
	return ret;
}

/* Abort scan -- reset firmware scan state machine.
 * The gamely BSP sends RPU_CMD_SCAN_ABORT (2) which goes through the tier-2
 * LMAC processor.  We instead send via FW_PKT_TYPE_SCAN (0x16) with
 * n_channel=0 to re-enter the scan handler directly -- this re-arms
 * scan_pending and the firmware scan loop exits immediately with no channels
 * to process, generating a SCAN_COMPLETE event.  Both approaches are valid. */
int sv6160_rpu_scan_abort(struct sv6160_softc *sc)
{
	struct rpu_cmd_scan cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.if_index = cpu_to_le32(sc->vif_index);
	cmd.n_channel = cpu_to_le32(0);

	dev_dbg(sc->dev, "scan abort: sending empty scan via pkt_type=0x%02x\n",
		FW_PKT_TYPE_SCAN);
	ret = sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), FW_PKT_TYPE_SCAN);
	if (ret)
		dev_warn(sc->dev, "scan abort: send failed (%d)\n", ret);

	return ret;
}

/* Program channel - with RF PLL pre-tuning via single-channel scan.
 *
 * RPU_CMD_CHANNEL (firmware fcn.0200154a) sends channel config to the
 * LMAC and sets internal state, but does NOT program the RF PLL register
 * at 0x42000034.  Only the scan handler (fcn.02007954) writes the PLL.
 *
 * Fix: send a 1-channel passive scan on the target channel FIRST to
 * program the PLL, then send RPU_CMD_CHANNEL to set firmware state.
 * The SCAN_COMPLETE event from the pretune scan is harmless - the
 * event handler checks sc->scan_request which is NULL for internal scans.
 */
int sv6160_rpu_channel(struct sv6160_softc *sc, u32 channel, u32 freq_band)
{
	struct rpu_cmd_channel cmd;
	struct rpu_cmd_scan pretune;
	int ret, polls;

	dev_info(sc->dev, "Channel set: ch=%u band=%u (pretune+set)\n",
		 channel, freq_band);

	/* Phase 1: Pre-tune RF PLL via single-channel passive scan */
	memset(&pretune, 0, sizeof(pretune));
	pretune.type = cpu_to_le32(SCAN_PASSIVE);
	pretune.n_channel = cpu_to_le32(1);
	pretune.channel_list[0] = channel;
	pretune.chan_max_power[0] = 20;
	pretune.chan_flags[0] = SCAN_PASSIVE;

	ret = sv6160_rpu_send_cmd(sc, &pretune, sizeof(pretune),
				  FW_PKT_TYPE_SCAN);
	if (ret)
		dev_warn(sc->dev, "PLL pretune scan ch=%u failed: %d\n",
			 channel, ret);

	/* Drain the SCAN_COMPLETE event from the pretune scan.
	 * PLL programming is fast (~1ms) but we need to consume
	 * the event to avoid stale RECV_LEN issues. */
	for (polls = 0; polls < 10; polls++) {
		sv6160_trigger_rx(sc);
		msleep(10);
	}

	/* Phase 2: Send RPU_CMD_CHANNEL to set firmware internal state.
	 * RPU_CMD_CHANNEL (23) → firmware tier-2 LMAC processor.
	 * Note: 23 == FW_PKT_TYPE_MGMT, but the MGMT fast-path is only
	 * taken when an internal flag at 0x100064E8 is set; otherwise
	 * falls through to the general LMAC processor (verified). */
	memset(&cmd, 0, sizeof(cmd));
	cmd.primary_ch_number = cpu_to_le32(channel);
	cmd.channel_number1 = cpu_to_le32(channel);
	cmd.channel_bw = cpu_to_le32(RPU_CHAN_WIDTH_20_NOHT);
	cmd.freq_band = cpu_to_le32(freq_band);

	reinit_completion(&sc->chan_prog_done);
	ret = sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_CHANNEL);
	if (ret)
		return ret;

	/* Wait for CH_PROG_DONE event */
	for (polls = 0; polls < 50; polls++) {
		sv6160_trigger_rx(sc);
		if (wait_for_completion_timeout(&sc->chan_prog_done,
						msecs_to_jiffies(200)))
			return 0;
	}
	dev_warn(sc->dev, "Channel programming timeout\n");
	return 0;
}

/* Update VIF configuration (BSSID, connect state, etc) */
int sv6160_rpu_vif_cfg(struct sv6160_softc *sc, const u8 *bssid,
		       u32 changed, u8 connect_state, u32 aid, u8 op_channel)
{
	struct rpu_cmd_vif_cfg cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.if_index = cpu_to_le32(sc->vif_index);
	cmd.changed_bitmap = cpu_to_le32(changed);

	if (changed & VIF_CFG_BSSID_CHANGED)
		memcpy(cmd.bssid, bssid, 6);

	if (changed & VIF_CFG_CONNECT_STATE) {
		cmd.connect_state = connect_state;
		cmd.aid = cpu_to_le32(aid);
		cmd.op_channel = op_channel;
	}

	if (changed & VIF_CFG_BCN_INT_CHANGED)
		cmd.beacon_interval = cpu_to_le32(sc->beacon_int);

	if (changed & VIF_CFG_SHORTSLOT_CHANGED)
		cmd.use_short_slot = cpu_to_le32(1);

	if (changed & VIF_CFG_BASICRATES_CHANGED)
		cmd.basic_rate_set = cpu_to_le32(0x153); /* default b/g rates */

	/* RPU_CMD_VIF_CFG (24) → firmware tier-2 LMAC processor (verified) */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_VIF_CFG);
}

/*
 * Send FW_PRIV_INIT - matches gamely BSP exactly.
 *
 * Gamely only sends ONE fw_priv_cmd during normal init: type=6
 * (FW_PRIV_INIT) with MAC byte + fw_skip_rx_pkt_submit=0.
 *
 * The fw_priv_cmd type field is an enum:
 *   1=DUMP_TXRX_COUNT, 2=DUMP_TXRX_BUF, 3=DUMP_REG_INFO,
 *   4=DUMP_FW_LOG, 5=DUMP_TXRX_QUEUE, 6=FW_PRIV_INIT,
 *   7=DUMP_FW_VERSION, 8=FW_SET_PARAMS
 *
 * Previous driver versions also sent type=3 (DUMP_REG_INFO - was
 * incorrectly called "SET_PHY_PARAMS"), type=6 again (duplicate),
 * and type=8 (FW_SET_PARAMS - production mode only). These extra
 * commands caused "rpu hang without ready" firmware errors.
 */
int sv6160_rpu_fw_priv_init(struct sv6160_softc *sc)
{
	/*
	 * Gamely fw_priv_cmd struct layout (from 0001-rk915.patch):
	 *   host_rpu_msg_hdr  hdr;        // 28 bytes
	 *   u32               type;       // 6 = FW_PRIV_INIT
	 *   fw_reg_info        reg_info;  // 16 bytes (unused for type=6)
	 *   int               production_test;
	 *   int               fw_skip_rx_pkt_submit;  // 0 = forward RX
	 *   fw_params          params;    // 8 bytes (unused for type=6)
	 *   int               sniffer;
	 *   u8                wlan_mac_addr;  // first byte of MAC only
	 *   u8                p2p_mac_addr;   // first byte of MAC only
	 */
	struct {
		struct rpu_msg_hdr hdr;
		__le32 type;
		__le32 reg_info[4]; /* fw_reg_info: reg, val, len, rw */
		__le32 production_test;
		__le32 fw_skip_rx_pkt_submit;
		__le32 params_mask;
		u8 params[6]; /* echo_mode..en_wifi_isr_thd */
		__le32 sniffer;
		u8 wlan_mac_addr;
		u8 p2p_mac_addr;
	} __packed cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = cpu_to_le32(6);  /* FW_PRIV_INIT */
	cmd.fw_skip_rx_pkt_submit = cpu_to_le32(0);
	cmd.wlan_mac_addr = sc->hw->wiphy->perm_addr[0];
	cmd.p2p_mac_addr = sc->hw->wiphy->perm_addr[0];

	reinit_completion(&sc->cmd_done);
	ret = sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd),
				  FW_PKT_TYPE_HOST_CMD);
	if (ret)
		return ret;

	{
		int polls = 30;

		while (polls-- > 0) {
			sv6160_trigger_rx(sc);
			if (wait_for_completion_timeout(&sc->cmd_done,
						       msecs_to_jiffies(100)))
				break;
		}
	}
	dev_info(sc->dev, "FW_PRIV_INIT type=6 sent (gamely-only init)\n");

	return 0;
}

/* Send NOP/ping to verify bidirectional firmware data path */
int sv6160_rpu_fw_priv_nop(struct sv6160_softc *sc)
{
	struct {
		struct rpu_msg_hdr hdr;
		__le32 sub_cmd;
	} __packed cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.sub_cmd = cpu_to_le32(FW_PRIV_NOP);

	reinit_completion(&sc->cmd_done);

	ret = sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), FW_PKT_TYPE_HOST_CMD);
	if (ret)
		return ret;

	if (!sc->is_started) {
		/* Init: poll manually */
		int polls = 10;

		while (polls-- > 0) {
			sv6160_trigger_rx(sc);
			if (wait_for_completion_timeout(&sc->cmd_done,
						       msecs_to_jiffies(100)))
				return 0;
			
		}
		dev_warn(sc->dev, "FW NOP timeout\n");
		return -ETIMEDOUT;
	}

	/* Post-init: poll with delayed stale guard reset */
	{
		int polls = 10;

		while (polls-- > 0) {
			sv6160_trigger_rx(sc);
			if (wait_for_completion_timeout(&sc->cmd_done,
						       msecs_to_jiffies(100)))
				return 0;
			msleep(100);
			/* send_cmd already resets last_recv_len */
		}
		dev_warn(sc->dev, "FW NOP timeout\n");
		return -ETIMEDOUT;
	}
	return 0;
}

/* Program PHY thresholds - one per 2.4GHz channel (14 total).
 * Gamely uses default_phy_threshold=180 for all channels.
 * RPU_CMD_UPD_PHY_THRESH (40) → tier-2 LMAC processor. */
int sv6160_rpu_phy_thresh(struct sv6160_softc *sc)
{
	struct {
		struct rpu_msg_hdr hdr;
		u8 tx_boost;
		u8 ssd_enable;
		u8 spare[2];
		u8 thresholds[14];
	} __packed cmd;
	int i;

	memset(&cmd, 0, sizeof(cmd));
	for (i = 0; i < 14; i++)
		cmd.thresholds[i] = 180;  /* gamely default */

	dev_info(sc->dev, "Setting PHY thresholds (180 all channels)\n");
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), 40);
}

/* Program TX power level */
int sv6160_rpu_tx_power(struct sv6160_softc *sc, u32 power_dbm)
{
	struct rpu_cmd_tx_power cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.tx_power = cpu_to_le32(power_dbm);
	cmd.band = cpu_to_le32(0); /* 2.4 GHz */
	cmd.if_index = cpu_to_le32(sc->vif_index);

	/* RPU_CMD_TX_POWER (10) → firmware tier-2 LMAC processor (verified) */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_TX_POWER);
}

/* Configure firmware power save mode */
int sv6160_rpu_ps_config(struct sv6160_softc *sc, int mode,
			 u32 idle_timeout, u32 listen_interval)
{
	struct rpu_cmd_ps cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.ps_mode = cpu_to_le32(mode);
	cmd.idle_timeout_ms = cpu_to_le32(idle_timeout);
	cmd.listen_interval = cpu_to_le32(listen_interval);

	/* RPU_CMD_PS (13) → firmware tier-2 LMAC processor (verified) */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_PS);
}

/* Send RPU_CMD_ROC_CTRL for remain-on-channel */
int sv6160_rpu_roc(struct sv6160_softc *sc, u32 ctrl, u32 channel,
		   u32 duration, u32 type)
{
	struct rpu_cmd_roc cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.roc_ctrl = cpu_to_le32(ctrl);
	cmd.roc_channel = cpu_to_le32(channel);
	cmd.roc_duration = cpu_to_le32(duration);
	cmd.roc_type = cpu_to_le32(type);

	/* RPU_CMD_ROC_CTRL (22) hits FW_PKT_TYPE_SCAN fast-path - the scan
	 * handler's RF channel setup is appropriate for remain-on-channel.
	 * This matches gamely BSP behavior (verified). */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_ROC_CTRL);
}

/* ===================================================================
 * RPU Event Processing
 *
 * Events arrive via the SDIO FIFO. The RX worker reads them
 * and dispatches based on the event ID in the header.
 * =================================================================== */

/*
 * Trim trailing padding from beacon/probe response IE region.
 *
 * The SV6160 firmware truncates large frames at 452 bytes (68 hdr + 384
 * payload), padding with zeros.  Trailing 00 00 bytes form a ghost SSID IE
 * (tag=0, len=0) that causes iwd to overwrite the real SSID with an empty
 * one.  Walk the IE chain and trim: (1) any incomplete trailing IE, and
 * (2) any trailing all-zero IEs that are clearly firmware padding.
 */
static void sv6160_trim_beacon_ies(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	unsigned int hdr_len;
	const u8 *ies;
	unsigned int ies_len, pos = 0, last_nonzero = 0;

	if (!ieee80211_is_beacon(hdr->frame_control) &&
	    !ieee80211_is_probe_resp(hdr->frame_control))
		return;

	/* 802.11 header + fixed fields: timestamp(8) + interval(2) + cap(2) */
	hdr_len = ieee80211_hdrlen(hdr->frame_control) + 12;
	if (skb->len <= hdr_len)
		return;

	ies = skb->data + hdr_len;
	ies_len = skb->len - hdr_len;

	/* Walk the IE chain.  Track the end of the last IE whose tag byte
	 * is nonzero - any trailing tag-0 IEs with zero-length data are
	 * firmware zero-padding, not real SSID IEs (the real SSID IE is
	 * always the first IE in a beacon). */
	while (pos + 1 < ies_len) {
		unsigned int ie_len = 2 + ies[pos + 1];

		if (pos + ie_len > ies_len)
			break;  /* truncated IE - stop here */

		if (ies[pos] != 0)
			last_nonzero = pos + ie_len;

		pos += ie_len;
	}

	/* If the only IEs are SSID (tag 0), keep them - don't trim a
	 * legitimate hidden-SSID-only beacon.  Otherwise trim trailing
	 * zero-tag IEs and any orphan bytes. */
	if (last_nonzero && last_nonzero < skb->len - hdr_len)
		skb_trim(skb, hdr_len + last_nonzero);
	else if (pos < ies_len)
		skb_trim(skb, hdr_len + pos);
}

void sv6160_rpu_rx_frame(struct sv6160_softc *sc, struct sk_buff *skb)
{
	struct rpu_rx_pkt rxinfo;
	struct rpu_msg_hdr *hdr;
	struct ieee80211_rx_status *rx_status;
	struct ieee80211_hdr *wh;
	struct ieee80211_supported_band *band;
	int i;

	if (skb->len < sizeof(struct rpu_rx_pkt)) {
		dev_warn(sc->dev, "rx_frame: too short %u < %zu\n",
			 skb->len, sizeof(struct rpu_rx_pkt));
		dev_kfree_skb(skb);
		return;
	}

	/* Copy RX info before stripping - we need it after skb_pull */
	memcpy(&rxinfo, skb->data, sizeof(struct rpu_rx_pkt));
	hdr = (struct rpu_msg_hdr *)skb->data;

	dev_info(sc->dev, "rx_frame: skb=%u pkt_len=%u ch=%u rssi=%u mic=%u qos=%u\n",
		 skb->len, le32_to_cpu(rxinfo.pkt_length), rxinfo.channel,
		 rxinfo.rssi, rxinfo.rx_pkt_status,
		 le32_to_cpu(hdr->more_cmd_data));

	/*
	 * QoS vs non-QoS frame stripping (from stolen rx.c):
	 * more_cmd_data != 0 means QoS frame - firmware overwrites the
	 * 2 reserved bytes with data to maintain 4-byte alignment of total
	 * length and 2-byte alignment of starting address for mac80211.
	 */
	if (le32_to_cpu(hdr->more_cmd_data) == 0) {
		/* Non-QoS: strip entire RX header */
		skb_pull(skb, sizeof(struct rpu_rx_pkt));
	} else {
		/* QoS: strip header minus 2 bytes, trim 2 from end */
		skb_pull(skb, sizeof(struct rpu_rx_pkt) - 2);
		skb_trim(skb, skb->len - 2);
	}

	dev_info(sc->dev, "rx_frame STEP1: stripped len=%u\n", skb->len);

	if (skb->len < 10) {
		dev_warn(sc->dev, "rx_frame: too short after strip: %u\n", skb->len);
		dev_kfree_skb(skb);
		return;
	}

	sv6160_trim_beacon_ies(skb);
	dev_info(sc->dev, "rx_frame STEP2: trimmed len=%u\n", skb->len);

	wh = (struct ieee80211_hdr *)skb->data;
	printk(KERN_ERR "sv6160: STEP2a wh=%px\n", wh);

	rx_status = IEEE80211_SKB_RXCB(skb);
	printk(KERN_ERR "sv6160: STEP2b rx_status=%px sizeof=%zu\n",
	       rx_status, sizeof(*rx_status));
	memset(rx_status, 0, sizeof(*rx_status));
	printk(KERN_ERR "sv6160: STEP2c memset done\n");

	/* Channel and frequency */
	rx_status->band = NL80211_BAND_2GHZ;
	rx_status->freq = ieee80211_channel_to_frequency(
		rxinfo.channel ? rxinfo.channel : 1, NL80211_BAND_2GHZ);
	printk(KERN_ERR "sv6160: STEP2d freq=%u\n", rx_status->freq);

	/*
	 * RSSI: firmware reports as unsigned char dBm value.
	 * Stolen assigns directly: rx_status.signal = rx_control_info.rssi
	 * For mac80211 SIGNAL_DBM, this should be a signed dBm value.
	 * Firmware likely stores as signed byte (e.g., -70 = 0xBA).
	 */
	rx_status->signal = (s8)rxinfo.rssi;
	printk(KERN_ERR "sv6160: STEP2e signal=%d\n", rx_status->signal);

	/* Decryption flags - only set when frame was actually encrypted.
	 * rx_pkt_status=0 means either "success" or "not encrypted".
	 * For beacons/probes (which are always unencrypted), don't
	 * set RX_FLAG_DECRYPTED - it confuses mac80211 and may cause
	 * scan results to be dropped. Only set for data frames. */
	if (rxinfo.rx_pkt_status == RX_MIC_FAILURE_TKIP) {
		rx_status->flag |= RX_FLAG_MMIC_ERROR;
	} else if (rxinfo.rx_pkt_status == RX_MIC_FAILURE_CCMP) {
		dev_kfree_skb(skb);
		return;
	}

	printk(KERN_ERR "sv6160: STEP2f mic=%u rate_flags=0x%02x rate=%u\n",
	       rxinfo.rx_pkt_status, rxinfo.rate_flags, rxinfo.rate_or_mcs);
	/* Rate index mapping */
	if (rxinfo.rate_flags & ENABLE_11N_FORMAT) {
		/* HT rate */
		rx_status->rate_idx = rxinfo.rate_or_mcs & 0x7f;
		rx_status->encoding = RX_ENC_HT;
	} else {
		/* Legacy rate - find matching bitrate */
		band = sc->hw->wiphy->bands[NL80211_BAND_2GHZ];
		if (band) {
			for (i = 0; i < band->n_bitrates; i++) {
				if (rxinfo.rate_or_mcs == band->bitrates[i].hw_value) {
					rx_status->rate_idx = i;
					break;
				}
			}
		}
	}

	printk(KERN_ERR "sv6160: STEP2g rate_idx=%u encoding=%u\n",
	       rx_status->rate_idx, rx_status->encoding);
	/* Beacon timestamp for TSF synchronization */
	if (ieee80211_is_beacon(wh->frame_control)) {
		rx_status->mactime = get_unaligned_le64(rxinfo.timestamp);
		rx_status->flag |= RX_FLAG_MACTIME_START;
	}

	rx_status->antenna = 0;

	sv6160_mark_active(sc);
	dev_info(sc->dev, "rx_frame STEP3: before deliver len=%u\n", skb->len);

	wh = (struct ieee80211_hdr *)skb->data;
	dev_info(sc->dev, "rx_frame DELIVER: fc=0x%04x len=%u freq=%u sig=%d\n",
		 le16_to_cpu(wh->frame_control),
		 skb->len, rxinfo.channel, (s8)rxinfo.rssi);
	ieee80211_rx_irqsafe(sc->hw, skb);
}

/*
 * Process RX_SERIAS (event 30) - aggregated batch of RX frames.
 * Format: cmd_rx_ctrl header (magic 0x3412ccff, pkt_count, pkt_len[])
 *         followed by concatenated individual RPU events.
 * Each event's length field upper 16 bits = next packet length.
 */
void sv6160_rpu_process_rx_batch(struct sv6160_softc *sc, struct sk_buff *skb)
{
	struct sv6160_rx_batch_hdr *batch;
	struct rpu_msg_hdr *hdr;
	u32 pkt_count;
	u8 *ptr;
	u32 offset;
	int i;

	if (skb->len < sizeof(struct sv6160_rx_batch_hdr)) {
		dev_warn(sc->dev, "RX_SERIAS: too short (%u bytes)\n", skb->len);
		dev_kfree_skb(skb);
		return;
	}

	batch = (struct sv6160_rx_batch_hdr *)skb->data;

	/* Verify magic in hal_data */
	if (get_unaligned_le32(batch->hdr.hal_data) != RX_SERIAS_MAGIC) {
		dev_warn(sc->dev, "RX_SERIAS: bad magic 0x%08x\n",
			 get_unaligned_le32(batch->hdr.hal_data));
		dev_kfree_skb(skb);
		return;
	}

	pkt_count = le32_to_cpu(batch->pkt_count);
	if (pkt_count == 0 || pkt_count > SV6160_MAX_AGG_PACKETS) {
		dev_warn(sc->dev, "RX_SERIAS: bad count %u\n", pkt_count);
		dev_kfree_skb(skb);
		return;
	}

	dev_info(sc->dev, "RX_SERIAS: %u packets, skb_len=%u\n",
		 pkt_count, skb->len);

	/* Skip past the batch header to the first event */
	offset = ALIGN(sizeof(struct sv6160_rx_batch_hdr), 4);
	ptr = skb->data + offset;

	for (i = 0; i < pkt_count; i++) {
		struct sk_buff *sub_skb;
		u32 pkt_len;

		if (offset >= skb->len)
			break;

		pkt_len = le16_to_cpu(batch->pkt_len[i]);
		if (pkt_len == 0 || offset + pkt_len > skb->len) {
			dev_info(sc->dev, "RX_SERIAS[%d]: stop - len=%u offset=%u total=%u\n",
				 i, pkt_len, offset, skb->len);
			break;
		}

		/* Fix length field: mask upper 16 bits (next pkt length) */
		hdr = (struct rpu_msg_hdr *)ptr;
		hdr->length = cpu_to_le32(le32_to_cpu(hdr->length) & 0x0000FFFF);

		dev_info(sc->dev, "RX_SERIAS[%d]: id=%u len=%u\n",
			 i, le32_to_cpu(hdr->id), pkt_len);

		/* Clone this sub-event into its own skb */
		sub_skb = dev_alloc_skb(pkt_len);
		if (!sub_skb)
			break;

		skb_put_data(sub_skb, ptr, pkt_len);
		sv6160_rpu_process_event(sc, sub_skb);

		offset += pkt_len;
		ptr += pkt_len;
	}

	dev_kfree_skb(skb);
}

static void sv6160_process_tx_done(struct sv6160_softc *sc,
				   struct sk_buff *skb)
{
	struct rpu_event_tx_done *tx_done;
	struct sk_buff *tx_skb;
	struct ieee80211_tx_info *info;
	u32 desc_id;
	int slot, i;
	u8 status, retries, rate;

	if (skb->len < sizeof(*tx_done)) {
		dev_dbg(sc->dev, "TX_DONE: short event (%u bytes)\n", skb->len);
		goto out;
	}

	tx_done = (struct rpu_event_tx_done *)skb->data;
	desc_id = le32_to_cpu(tx_done->descriptor_id);

	dev_dbg(sc->dev,
		"TX_DONE: desc_id=%u queue=%u pdout=%u status=[%u,%u,%u,%u] retries=[%u,%u,%u,%u] rate=[%u,%u,%u,%u]\n",
		 desc_id, tx_done->queue, tx_done->pdout_voltage,
		 tx_done->frm_status[0], tx_done->frm_status[1],
		 tx_done->frm_status[2], tx_done->frm_status[3],
		 tx_done->retries_num[0], tx_done->retries_num[1],
		 tx_done->retries_num[2], tx_done->retries_num[3],
		 tx_done->rate[0], tx_done->rate[1],
		 tx_done->rate[2], tx_done->rate[3]);

	for (i = 0; i < SV6160_MAX_TX_CMDS; i++) {
		status  = tx_done->frm_status[i];
		retries = tx_done->retries_num[i];
		rate    = tx_done->rate[i];

		/*
		 * For single-frame TX_DONE the descriptor_id field
		 * identifies the frame; for batched completions,
		 * each successive index maps to desc_id + i.
		 */
		slot = (desc_id + i) % SV6160_TX_PENDING_MAX;
		tx_skb = sc->tx_pending[slot];

		if (!tx_skb) {
			/* No pending frame in this slot - if status is
			 * also zero this is just padding, so stop.
			 */
			if (status == 0)
				break;
			dev_dbg(sc->dev,
				"TX_DONE: no pending skb for desc %u slot %d (status %u)\n",
				desc_id + i, slot, status);
			continue;
		}

		sc->tx_pending[slot] = NULL;

		info = IEEE80211_SKB_CB(tx_skb);
		ieee80211_tx_info_clear_status(info);

		if (status == TX_DONE_STAT_SUCCESS)
			info->flags |= IEEE80211_TX_STAT_ACK;

		/* Report rate and retry count to mac80211 */
		if (rate & 0x80) {
			/* MCS rate */
			info->status.rates[0].idx = rate & 0x7f;
			info->status.rates[0].flags = IEEE80211_TX_RC_MCS;
		} else if (rate > 0) {
			/* Legacy rate - find matching bitrate index */
			struct ieee80211_supported_band *band;

			band = sc->hw->wiphy->bands[info->band];
			info->status.rates[0].idx = 0;
			if (band) {
				int j;

				for (j = 0; j < band->n_bitrates; j++) {
					if (band->bitrates[j].hw_value == rate) {
						info->status.rates[0].idx = j;
						break;
					}
				}
			}
			info->status.rates[0].flags = 0;
		} else {
			/* Rate unknown - keep whatever mac80211 set */
			info->status.rates[0].idx = -1;
		}
		info->status.rates[0].count = retries + 1;

		/* Invalidate remaining rate entries */
		info->status.rates[1].idx = -1;
		info->status.rates[1].count = 0;

		dev_dbg(sc->dev,
			"TX_DONE[%d]: desc=%u status=%u retries=%u rate=0x%02x\n",
			i, desc_id + i, status, retries, rate);

		ieee80211_tx_status_irqsafe(sc->hw, tx_skb);

		if (atomic_dec_return(&sc->tx_outstanding) <
		    SV6160_TX_PENDING_MAX - 8)
			ieee80211_wake_queues(sc->hw);
	}

out:
	dev_kfree_skb(skb);
}

void sv6160_rpu_process_event(struct sv6160_softc *sc, struct sk_buff *skb)
{
	struct rpu_msg_hdr *hdr = (struct rpu_msg_hdr *)skb->data;
	u32 event = le32_to_cpu(hdr->id) & 0xffff;

	dev_info(sc->dev, "EVENT: id=%u len=%u skb_len=%u\n",
		 event, le32_to_cpu(hdr->length) & 0xffff, skb->len);

	switch (event) {
	case RPU_EVENT_RESET_COMPLETE:
		dev_info(sc->dev, "RPU: reset complete\n");
		complete(&sc->reset_done);
		dev_kfree_skb(skb);
		break;

	case RPU_EVENT_SCAN_COMPLETE:
		dev_info(sc->dev, "RPU: scan complete\n");
		cancel_delayed_work(&sc->scan_timeout);
		cancel_delayed_work(&sc->scan_poll);
		cancel_delayed_work(&sc->scan_work);
		sc->scan_in_progress = false;
		/* Re-enable GPIO IRQ + clear state after scan */
		if (sc->gpio_irq_disabled && sc->host_wake_irq > 0 &&
		    !READ_ONCE(sc->removing)) {
			int err;

			sdio_writeb(sc->func, SDIO_REG_INT_CLR_VAL,
				    SDIO_REG_INT_ADDR, &err);
			sdio_writeb(sc->func, 1, SDIO_REG_NOTIFY_ADDR, &err);
			sv6160_sdio_write_comp(sc->func);
			usleep_range(2000, 3000);
			sc->gpio_irq_disabled = false;
			enable_irq(sc->host_wake_irq);
		}
		if (sc->scan_request) {
			struct cfg80211_scan_info info = {
				.aborted = false,
			};

			ieee80211_scan_completed(sc->hw, &info);
			sc->scan_request = NULL;
		}
		dev_kfree_skb(skb);
		break;

	case RPU_EVENT_SCAN_ABORT_COMPLETE:
		dev_info(sc->dev, "RPU: scan abort complete\n");
		cancel_delayed_work(&sc->scan_timeout);
		cancel_delayed_work(&sc->scan_poll);
		cancel_delayed_work(&sc->scan_work);
		sc->scan_in_progress = false;
		/* Re-enable GPIO IRQ + clear state after scan abort */
		if (sc->gpio_irq_disabled && sc->host_wake_irq > 0 &&
		    !READ_ONCE(sc->removing)) {
			int err;

			sdio_writeb(sc->func, SDIO_REG_INT_CLR_VAL,
				    SDIO_REG_INT_ADDR, &err);
			sdio_writeb(sc->func, 1, SDIO_REG_NOTIFY_ADDR, &err);
			sv6160_sdio_write_comp(sc->func);
			usleep_range(2000, 3000);
			sc->gpio_irq_disabled = false;
			enable_irq(sc->host_wake_irq);
		}
		if (sc->scan_request) {
			struct cfg80211_scan_info info = {
				.aborted = true,
			};
			ieee80211_scan_completed(sc->hw, &info);
			sc->scan_request = NULL;
		}
		dev_kfree_skb(skb);
		break;

	case RPU_EVENT_RX:
		dev_info(sc->dev, "RPU_EVENT_RX: len=%u\n", skb->len);
		sv6160_rpu_rx_frame(sc, skb);
		/* skb consumed by mac80211 */
		break;

	case RPU_EVENT_RX_SERIAS:
		if (skb->len > 28) {
			/* Non-bare RX_SERIAS - could be WiFi frame or batch.
			 * Check for batch magic; if absent, try as single RX. */
			dev_info(sc->dev, "RX_SERIAS: len=%u, first 16 bytes:\n",
				 skb->len);
			print_hex_dump(KERN_INFO, "  rx_ser: ", DUMP_PREFIX_OFFSET,
				       16, 1, skb->data,
				       min_t(unsigned int, skb->len, 64), true);

			if (skb->len >= sizeof(struct sv6160_rx_batch_hdr) &&
			    get_unaligned_le32(skb->data) == RX_SERIAS_MAGIC) {
				sv6160_rpu_process_rx_batch(sc, skb);
			} else {
				/* Try as individual RX frame - the chain
				 * reading delivers frames with event id=30
				 * but no batch magic. Treat as RPU_EVENT_RX. */
				dev_info(sc->dev, "RX_SERIAS as single frame\n");
				sv6160_rpu_rx_frame(sc, skb);
			}
		} else {
			/* Bare header (28 bytes) - status notification */
			dev_kfree_skb(skb);
		}
		break;

	case RPU_EVENT_TX_DONE:
		sv6160_process_tx_done(sc, skb);
		break;

	case RPU_EVENT_HOST_DATA_TX_DONE: {
		/* Event 32: TX completion for pkt_type=0x2C (HOST_DATA).
		 * Struct layout differs from rpu_event_tx_done (id=1):
		 * descriptor_id at 0x08 is always 0, no per-frame status.
		 * Bulk-ack all pending TX frames as successful.
		 */
		int j;

		dev_dbg(sc->dev, "HOST_DATA_TX_DONE: len=%u\n", skb->len);

		for (j = 0; j < SV6160_TX_PENDING_MAX; j++) {
			struct sk_buff *tx_skb = sc->tx_pending[j];
			struct ieee80211_tx_info *txi;

			if (!tx_skb)
				continue;

			sc->tx_pending[j] = NULL;
			txi = IEEE80211_SKB_CB(tx_skb);
			ieee80211_tx_info_clear_status(txi);
			txi->flags |= IEEE80211_TX_STAT_ACK;
			txi->status.rates[0].idx = -1;
			txi->status.rates[0].count = 0;
			ieee80211_tx_status_irqsafe(sc->hw, tx_skb);

			if (atomic_dec_return(&sc->tx_outstanding) <
			    SV6160_TX_PENDING_MAX - 8)
				ieee80211_wake_queues(sc->hw);
		}
		dev_kfree_skb(skb);
		break;
	}

	case RPU_EVENT_CH_PROG_DONE:
		dev_info(sc->dev, "RPU: channel programming done\n");
		complete(&sc->chan_prog_done);
		dev_kfree_skb(skb);
		break;

	case RPU_EVENT_CALIBRATION_DONE:
	case RPU_EVENT_COMMAND_PROC_DONE:
	case RPU_EVENT_FW_PRIV_CMD_DONE:
		dev_dbg(sc->dev, "RPU: command done (event %u, len=%u)\n",
			event, skb->len);
		complete(&sc->cmd_done);
		dev_kfree_skb(skb);
		break;

	case RPU_EVENT_DISCONNECTED:
		dev_info(sc->dev, "RPU: disconnected\n");
		if (sc->vif) {
			wiphy_info(sc->hw->wiphy,
				   "disconnect event, vif type=%d addr=%pM\n",
				   sc->vif->type, sc->vif->addr);
			ieee80211_connection_loss(sc->vif);
		}
		dev_kfree_skb(skb);
		break;

	case RPU_EVENT_ROC_STATUS:
		dev_info(sc->dev, "RPU: ROC status event\n");
		if (sc->roc_in_progress) {
			cancel_delayed_work(&sc->roc_timeout);
			sc->roc_in_progress = false;
			ieee80211_remain_on_channel_expired(sc->hw);
		}
		dev_kfree_skb(skb);
		break;

	case RPU_EVENT_FW_ERROR:
		dev_err(sc->dev, "RPU: firmware error! (len=%u)\n", skb->len);
		print_hex_dump(KERN_ERR, "  fw_err: ", DUMP_PREFIX_OFFSET,
			       16, 1, skb->data,
			       min_t(unsigned int, skb->len, 128), true);
		dev_kfree_skb(skb);
		break;

	default:
		dev_info(sc->dev, "RPU: unknown event %u (len=%u)\n",
			 event, le32_to_cpu(hdr->length));
		dev_kfree_skb(skb);
		break;
	}
}

/* ===================================================================
 * Firmware State Machine & Boot
 * =================================================================== */

static int sv6160_poll_fw_state(struct sv6160_softc *sc,
				enum sv6160_fw_state target, int retries)
{
	u8 state;
	int ret;

	while (retries-- > 0) {
		ret = sv6160_sdio_readb(sc, SDIO_REG_FW_STATE, &state);
		if (ret)
			return ret;
		if (state == target)
			return 0;
		if (state == FW_STATE_RESET_COMPLETE)
			return -EIO;
		msleep(SV6160_FW_POLL_DELAY_MS);
	}
	dev_err(sc->dev, "FW state timeout: wanted %d, got %d\n",
		target, state);
	return -ETIMEDOUT;
}

int sv6160_boot_firmware(struct sv6160_softc *sc)
{
	const struct firmware *fw, *patch;
	struct sv6160_start_cmd start_cmd;
	size_t offset, chunk;
	int ret;

	ret = request_firmware(&fw, SV6160_FW_NAME, sc->dev);
	if (ret) {
		dev_err(sc->dev, "Failed to load %s: %d\n", SV6160_FW_NAME, ret);
		return ret;
	}
	dev_info(sc->dev, "Firmware %s: %zu bytes\n", SV6160_FW_NAME, fw->size);

	offset = 0;
	while (offset < fw->size) {
		chunk = min_t(size_t, fw->size - offset, SV6160_MAX_BLOCK_SIZE);
		ret = sv6160_sdio_write_data(sc, offset,
					     (void *)(fw->data + offset), chunk);
		if (ret) {
			dev_err(sc->dev, "FW upload failed at %zu: %d\n", offset, ret);
			goto out_fw;
		}
		offset += chunk;
	}
	dev_info(sc->dev, "Firmware uploaded: %zu bytes\n", offset);

	start_cmd.id     = cpu_to_le32(SDIO_START_CMD_MAGIC);
	start_cmd.length = cpu_to_le32(4);
	start_cmd.addr   = cpu_to_le32(0);

	ret = sv6160_sdio_write_data(sc, SDIO_CMD_ADDR,
				     &start_cmd, sizeof(start_cmd));
	if (ret)
		goto out_fw;

	ret = sv6160_poll_fw_state(sc, FW_STATE_WAIT_PATCH,
				   SV6160_FW_POLL_RETRIES);
	if (ret)
		goto out_fw;

	dev_info(sc->dev, "Firmware started, uploading patch\n");

	ret = request_firmware(&patch, mod_patch_name, sc->dev);
	if (ret) {
		dev_err(sc->dev, "Failed to load %s: %d\n", mod_patch_name, ret);
		goto out_fw;
	}
	if (ret == 0) {
		sv6160_sdio_writeb(sc, SDIO_REG_PATCH_LEN_L,
				   patch->size & 0xFF);
		sv6160_sdio_writeb(sc, SDIO_REG_PATCH_LEN_H,
				   (patch->size >> 8) & 0xFF);

		offset = 0;
		while (offset < patch->size) {
			chunk = min_t(size_t, patch->size - offset,
				      SV6160_MAX_BLOCK_SIZE);
			ret = sv6160_sdio_write_data(sc, offset,
						     (void *)(patch->data + offset),
						     chunk);
			if (ret) {
				release_firmware(patch);
				goto out_fw;
			}
			offset += chunk;
		}
		sv6160_sdio_writeb(sc, SDIO_REG_PATCH_LEN_L, 0);
		sv6160_sdio_writeb(sc, SDIO_REG_PATCH_LEN_H, 0);
		release_firmware(patch);
	}

	ret = sv6160_poll_fw_state(sc, FW_STATE_M0_READY,
				   SV6160_FW_POLL_RETRIES);
	if (ret) {
		u8 state;

		if (!sv6160_sdio_readb(sc, SDIO_REG_FW_STATE, &state) &&
		    state == FW_STATE_LPW_READY)
			ret = 0;
		else
			goto out_fw;
	}

	sc->is_firmware_loaded = true;

	/* Clear boot GPIO state BEFORE enabling the GPIO IRQ.
	 * The firmware asserts GPIO during power-on.  If we enable the
	 * level-triggered IRQ while GPIO is HIGH, it fires immediately,
	 * entering a rapid disable/work/re-enable cycle that starves
	 * USB interrupts and destabilizes the USB gadget network. */
	{
		int error;

		sdio_claim_host(sc->func);
		sdio_writeb(sc->func, SDIO_REG_INT_CLR_VAL,
			    SDIO_REG_INT_ADDR, &error);
		sdio_writeb(sc->func, 1, SDIO_REG_NOTIFY_ADDR, &error);
		sv6160_sdio_write_comp(sc->func);
		sdio_writeb(sc->func, 0x02, 34, &error);
		sdio_writeb(sc->func, 0x02, 33, &error);
		sdio_release_host(sc->func);
		dev_info(sc->dev, "Boot GPIO state cleared\n");
	}

	/* Wait for GPIO to deassert after clear+notify */
	usleep_range(2000, 3000);

	/* IRQs are requested by probe after core state is initialized. */

	dev_info(sc->dev, "Firmware ready\n");

out_fw:
	release_firmware(fw);
	return ret;
}

/* ===================================================================
 * RX Worker - Read events/packets from SDIO data FIFO
 *
 * Architecture matches gamely BSP: pure GPIO IRQ-driven reads.
 * RECV_LEN is ONLY valid when GPIO fires - it retains stale values
 * otherwise. A -110 CMD53 timeout on stale RECV_LEN permanently
 * kills the DW MMC SDIO bus. Therefore:
 *   - Post-init: ONLY read when gpio_irq_pending is true
 *   - Init phase: gpio_irq_pending is force-set by init polling
 *   - Drain ALL data per GPIO assertion (loop until RECV_LEN=0)
 *   - Clear+notify only after draining (gamely pattern)
 * =================================================================== */

static void sv6160_dwmmc_fifo_reset(struct sv6160_softc *sc)
{
	if (!sc->dwmmc_regs)
		return;
	{
		u32 ctrl = readl(sc->dwmmc_regs);
		int tries = 1000;

		ctrl |= (1 << 1) | (1 << 2);  /* FIFO_RESET | DMA_RESET */
		writel(ctrl, sc->dwmmc_regs);
		while (tries-- > 0) {
			ctrl = readl(sc->dwmmc_regs);
			if (!(ctrl & ((1 << 1) | (1 << 2))))
				break;
			udelay(1);
		}
	}
}

void sv6160_rx_worker(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc, rx_work);
	struct sk_buff *skb;
	struct rpu_msg_hdr *hdr;
	u16 pkt_len;
	u8 lo, hi;
	int ret, err;
	int chain_depth = 0;
	const int max_chain = 16;  /* prevent runaway chains */

	/* Only read when GPIO IRQ has signalled data availability.
	 * During init, gpio_irq_pending is force-set by the init
	 * polling path. Post-init, it's set by the GPIO handler. */
	if (!sc->gpio_irq_pending) {
		dev_dbg(sc->dev, "rx_worker: no GPIO pending - skip\n");
		return;
	}

	sdio_claim_host(sc->func);

	dev_info(sc->dev, "read_next: checking RECV_LEN\n");
	lo = sdio_readb(sc->func, SDIO_REG_RECV_LEN_L, &ret);
	if (ret)
		goto done;
	hi = sdio_readb(sc->func, SDIO_REG_RECV_LEN_H, &ret);
	if (ret)
		goto done;

	pkt_len = lo | (hi << 8);

	dev_info(sc->dev, "RECV_LEN=%u at read_next\n", pkt_len);

	if (pkt_len == 0) {
		/* No more data - clear+notify and mark GPIO consumed. */
		sdio_writeb(sc->func, SDIO_REG_INT_CLR_VAL,
			    SDIO_REG_INT_ADDR, &err);
		sdio_writeb(sc->func, 1, SDIO_REG_NOTIFY_ADDR, &err);
		sv6160_sdio_write_comp(sc->func);
		sc->gpio_irq_pending = false;
		/* Do NOT reset last_recv_len here - firmware keeps RECV_LEN
	 * at the old value after clear+notify. Resetting would make
	 * the stale guard re-read the same stale value → -110. */
		goto done;
	}

	/* SDIO-busy RECV_LEN - back off, do NOT touch the bus.
	 * When the card is internally busy it drives the bus high, so a
	 * CMD52 read of RECV_LEN can succeed (ret == 0) yet return 0xFF in
	 * each byte → pkt_len == 0xFFFF. That is never a real length
	 * (0xFFFF > SV6160_MAX_RX_SIZE), and it must not fall through to the
	 * oversized/clear_and_done path, which would write clear+notify onto
	 * a bus the card is not servicing - precisely the hammering that turns
	 * the transient stall into the permanent -110 DW MMC bus kill.
	 * Consume the GPIO and let the next assertion retry; do not update
	 * last_recv_len (0xFFFF must not poison the stale guard).
	 *
	 * On the vendor-based driver for this same chip, the identical busy
	 * condition under sustained RX wedges *every* SDIO transaction with
	 * -EBUSY until a chip power cycle, and there it surfaces as 0xF0F0
	 * because that driver folds the -EBUSY errno into the length bytes.
	 * See VENDOR-DRIVER-FINDINGS.md. */
	if (pkt_len == 0xFFFF) {
		dev_warn_ratelimited(sc->dev,
			"RECV_LEN=0xFFFF - SDIO busy, backing off (not clearing)\n");
		sc->gpio_irq_pending = false;
		goto done;
	}

	/* Stale RECV_LEN protection - always active.
	 * The firmware never zeros RECV_LEN after clear+notify. Without
	 * this guard, reading stale RECV_LEN causes CMD53 -110 timeout
	 * that permanently kills the DW MMC bus. Trade-off: consecutive
	 * same-size events are blocked unless chained via rx_next_len.
	 * Commands that expect a response must clear last_recv_len to
	 * allow the response through (done in completion wait loops). */
	if (pkt_len == sc->last_recv_len) {
		dev_info(sc->dev, "stale guard BLOCKED: pkt_len=%u == last=%u\n",
			 pkt_len, sc->last_recv_len);
		sc->gpio_irq_pending = false;  /* consumed - wait for next poll */
		goto done;
	}
	dev_info(sc->dev, "stale guard PASSED: pkt_len=%u != last=%u\n",
		 pkt_len, sc->last_recv_len);
	sc->last_recv_len = pkt_len;

	/* DMA readiness delay: the firmware writes RECV_LEN before
	 * fully configuring the DMA source address. Without this delay,
	 * CMD53 reads timeout (-110) because the DMA source isn't ready.
	 * Use 5ms for polling mode (no interrupt guarantees freshness). */
	usleep_range(5000, 6000);

	dev_dbg(sc->dev, "RECV_LEN=%u (lo=0x%02x hi=0x%02x)\n",
		pkt_len, lo, hi);

	if (pkt_len > SV6160_MAX_RX_SIZE) {
		dev_warn(sc->dev, "RX oversized: %u - clearing\n", pkt_len);
		goto clear_and_done;
	}

	skb = dev_alloc_skb(pkt_len);
	if (!skb)
		goto clear_and_done;

	ret = sdio_memcpy_fromio(sc->func, skb_put(skb, pkt_len),
				 SDIO_DATA_ADDR, pkt_len);
	if (ret) {
		if (net_ratelimit())
			dev_warn(sc->dev, "RX read %u bytes failed: %d\n",
				 pkt_len, ret);
		dev_kfree_skb(skb);
		sv6160_dwmmc_fifo_reset(sc);
		/* Recovery: clear+notify to reset firmware SDIO state,
		 * then exit gracefully. Do NOT set recv_len_failed -
		 * that blocks ALL future reads permanently. Instead,
		 * set last_recv_len = pkt_len so the stale guard
		 * blocks only this specific (stale) size. The bus
		 * can recover and the next different-sized event
		 * passes the guard normally. */
		sdio_writeb(sc->func, SDIO_REG_INT_CLR_VAL,
			    SDIO_REG_INT_ADDR, &err);
		sdio_writeb(sc->func, 1, SDIO_REG_NOTIFY_ADDR, &err);
		sv6160_sdio_write_comp(sc->func);
		sc->last_recv_len = pkt_len;
		sc->gpio_irq_pending = false;
		goto done;
	}

	dev_dbg(sc->dev, "RX OK %u bytes\n", pkt_len);
	print_hex_dump_debug("sv6160 rx: ", DUMP_PREFIX_OFFSET,
			     16, 1, skb->data,
			     min_t(u16, pkt_len, 64), true);

	sc->recv_len_failed = false;  /* successful read - allow stale guard reset */

	/* DW MMC FIFO/DMA reset between CMD53 reads */
	sv6160_dwmmc_fifo_reset(sc);

	/* Parse header - upper 16 bits of length = next pkt length.
	 * Gamely uses this to decide whether to clear+notify or read more.
	 * If rx_next_len > 0, firmware has more data - DON'T clear+notify. */
	hdr = (struct rpu_msg_hdr *)skb->data;
	{
		u32 raw_len = le32_to_cpu(hdr->length);
		u16 rx_next_len = raw_len >> 16;

		hdr->length = cpu_to_le32(raw_len & 0x0000FFFF);

		sv6160_rpu_process_event(sc, skb);
		sc->last_activity = jiffies;

		if (rx_next_len > sizeof(struct rpu_msg_hdr) &&
		    chain_depth < max_chain) {
read_chain:
			chain_depth++;
			/* More data follows - read using rx_next_len as size.
			 * ONLY chain-read if > 28 bytes - 28-byte chain reads
			 * hit stale DMA and cause -110 that kills the bus.
			 * instead of re-reading RECV_LEN (which may be stale).
			 * This matches gamely's buffer parsing pattern. */
			dev_info(sc->dev, "rx_next_len=%u - reading more\n",
				rx_next_len);

			sv6160_dwmmc_fifo_reset(sc);

			if (rx_next_len > SV6160_MAX_RX_SIZE)
				goto clear_and_done;

			skb = dev_alloc_skb(rx_next_len);
			if (!skb)
				goto clear_and_done;

			udelay(2000);
			ret = sdio_memcpy_fromio(sc->func,
						 skb_put(skb, rx_next_len),
						 SDIO_DATA_ADDR, rx_next_len);
			if (ret) {
				dev_warn(sc->dev, "RX chain read %u failed: %d\n",
					 rx_next_len, ret);
				dev_kfree_skb(skb);
				sv6160_dwmmc_fifo_reset(sc);
				goto clear_and_done;
			}

			sv6160_dwmmc_fifo_reset(sc);

			hdr = (struct rpu_msg_hdr *)skb->data;
			raw_len = le32_to_cpu(hdr->length);
			rx_next_len = raw_len >> 16;
			hdr->length = cpu_to_le32(raw_len & 0xFFFF);

			dev_info(sc->dev, "EVENT: id=%u len=%u skb_len=%u (chain)\n",
				 le32_to_cpu(hdr->id) & 0xffff,
				 raw_len & 0xffff, skb->len);

			sv6160_rpu_process_event(sc, skb);
			sc->last_activity = jiffies;

			/* Continue chain if more LARGE data follows.
			 * Skip 28-byte chain reads - they cause -110. */
			if (rx_next_len > sizeof(struct rpu_msg_hdr) &&
			    chain_depth < max_chain)
				goto read_chain;
		}
	}

	/* Last packet - clear+notify to tell firmware we're done.
	 * Re-enable interrupt generation for next GPIO assertion. */
clear_and_done:
	sdio_writeb(sc->func, SDIO_REG_INT_CLR_VAL,
		    SDIO_REG_INT_ADDR, &err);
	sdio_writeb(sc->func, 1, SDIO_REG_NOTIFY_ADDR, &err);
	sv6160_sdio_write_comp(sc->func);
	sdio_writeb(sc->func, 0x02, 34, &err);
	sdio_writeb(sc->func, 0x02, 33, &err);
	sc->gpio_irq_pending = false;
	/* Do NOT read back RECV_LEN after clear+notify.  The firmware
	 * may briefly return 0 then snap back to the stale value,
	 * causing last_recv_len=0 which defeats the stale guard.
	 * Instead, last_recv_len retains pkt_len from the successful
	 * read (set at line ~1533).  The stale guard will block any
	 * poll that sees the same RECV_LEN, and rpu_send_cmd resets
	 * the guard to 0 before sending new commands. */
done:
	sdio_release_host(sc->func);

	/* Re-enable level-triggered GPIO IRQ after reading data.
	 * Only if the handler actually disabled it - init polling
	 * sets gpio_irq_pending without disabling the IRQ. */
	if (sc->gpio_irq_disabled && !READ_ONCE(sc->removing)) {
		sc->gpio_irq_disabled = false;
		enable_irq(sc->host_wake_irq);
	}
}

/* RX poll timer - adaptive polling to balance latency vs CPU */
static void sv6160_rx_poll_fn(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       rx_poll.work);

	if (!sc->is_firmware_loaded || READ_ONCE(sc->removing))
		return;

	/* Post-init: GPIO IRQ is the sole RX trigger (gamely model).
	 * Polling is ONLY used during init phase (before is_started)
	 * because GPIO may not fire for early firmware events.
	 * A stale RECV_LEN read via polling causes -110 that kills
	 * the DW MMC bus permanently. */
	if (sc->is_started) {
		/* Post-init: GPIO level IRQ + SDIO in-band IRQ drive RX.
		 * No background polling - command wait loops have their
		 * own polling with stale guard reset. */
		return;
	}

	/* Init phase: poll for firmware events */
	sv6160_trigger_rx(sc);

	schedule_delayed_work(&sc->rx_poll, msecs_to_jiffies(100));
}

/* ===================================================================
 * TX Path
 * =================================================================== */

int sv6160_hci_tx(struct sv6160_softc *sc, struct sk_buff *skb, bool is_cmd)
{
	int ret;

	ret = sv6160_sdio_write_data(sc, SDIO_DATA_ADDR,
				     skb->data, skb->len);
	if (ret)
		dev_kfree_skb(skb);
	else
		consume_skb(skb);

	return ret;
}

void sv6160_stop_queues(struct sv6160_softc *sc)
{
	if (sc->hw)
		ieee80211_stop_queues(sc->hw);
}

void sv6160_start_queues(struct sv6160_softc *sc)
{
	if (sc->hw)
		ieee80211_wake_queues(sc->hw);
}

void sv6160_notify_pm(struct sv6160_softc *sc, int wakeup)
{
	sv6160_sdio_writeb(sc, SDIO_REG_NOTIFY_ADDR,
			   wakeup ? NOTIFY_WAKEUP : NOTIFY_SLEEP);
}

/* ===================================================================
 * Power Management - Idle Timer
 * =================================================================== */

static void sv6160_idle_work_fn(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       idle_work.work);
	unsigned long idle_ms;

	if (!sc->is_started || sc->is_suspended)
		return;

	idle_ms = jiffies_to_msecs(jiffies - sc->last_activity);

	/* After 5 seconds idle, enter PS mode to save power */
	if (idle_ms > 5000 && !sc->ps_enabled) {
		dev_dbg(sc->dev, "Entering power save after %lu ms idle\n",
			idle_ms);
		sv6160_rpu_ps_config(sc, PS_MODE_ENABLED, 200, 3);
		sc->ps_enabled = true;
	}

	/* Reschedule idle check */
	if (sc->is_started)
		schedule_delayed_work(&sc->idle_work, msecs_to_jiffies(5000));
}

static inline void sv6160_mark_active(struct sv6160_softc *sc)
{
	sc->last_activity = jiffies;
	if (sc->ps_enabled) {
		sv6160_rpu_ps_config(sc, PS_MODE_DISABLED, 0, 0);
		sc->ps_enabled = false;
	}
}

/* ===================================================================
 * Beacon TX (AP/IBSS) - software timer, like stolen/rk915 beacon.c
 *
 * The SV6160 firmware does not generate beacons autonomously.
 * We use a kernel timer to periodically fetch beacon skbs from
 * mac80211, prepend the TX control header, and transmit them.
 * After each beacon, drain buffered broadcast/multicast frames.
 * =================================================================== */

static void sv6160_bcn_timer_fn(struct timer_list *t)
{
	struct sv6160_softc *sc = from_timer(sc, t, bcn_timer);
	struct sk_buff *skb, *bc_skb;

	if (!sc->beacon_enabled || !sc->vif)
		return;

	skb = ieee80211_beacon_get(sc->hw, sc->vif);
	if (!skb)
		goto resched;

	/* Queue beacon for TX via the standard TX worker path */
	*(u32 *)skb->cb = sc->tx_desc_id++;
	skb_queue_tail(&sc->tx_queue, skb);
	schedule_work(&sc->tx_work);

	/* Drain buffered broadcast/multicast after beacon (DTIM) */
	while ((bc_skb = ieee80211_get_buffered_bc(sc->hw, sc->vif)) != NULL) {
		*(u32 *)bc_skb->cb = sc->tx_desc_id++;
		skb_queue_tail(&sc->tx_queue, bc_skb);
	}
	schedule_work(&sc->tx_work);

resched:
	if (sc->beacon_enabled && sc->vif)
		mod_timer(&sc->bcn_timer,
			  jiffies + msecs_to_jiffies(sc->beacon_int - 10));
}

static void sv6160_beacon_start(struct sv6160_softc *sc, u16 interval)
{
	sc->beacon_int = interval ?: 100;
	sc->beacon_enabled = true;
	mod_timer(&sc->bcn_timer,
		  jiffies + msecs_to_jiffies(sc->beacon_int - 10));
	dev_info(sc->dev, "Beacon TX started, interval=%u TU\n",
		 sc->beacon_int);
}

static void sv6160_beacon_stop(struct sv6160_softc *sc)
{
	sc->beacon_enabled = false;
	del_timer_sync(&sc->bcn_timer);
	dev_info(sc->dev, "Beacon TX stopped\n");
}

/* ===================================================================
 * mac80211 Operations
 * =================================================================== */

static int sv6160_mac_start(struct ieee80211_hw *hw)
{
	struct sv6160_softc *sc = hw->priv;
	int ret;

	if (!sc->is_firmware_loaded || READ_ONCE(sc->removing))
		return -EIO;

	/* Step 0: Notify firmware wakeup - gamely calls rk915_notify_pm(1)
	 * BEFORE rpu_prog_reset.  This writes IO_NOTIFY_WAKEUP (3) to
	 * register 17, signaling the firmware MCU to exit any low-power
	 * state before processing commands. */
	sv6160_notify_pm(sc, 1);
	msleep(10);

	/* Step 1: RPU_CMD_RESET + wait for response */
	ret = sv6160_rpu_reset(sc);
	if (ret)
		return ret;

	/* Step 2: FW_PRIV_INIT (type=6) - enable RX frame forwarding */
	ret = sv6160_rpu_fw_priv_init(sc);
	if (ret)
		dev_warn(sc->dev, "FW_PRIV_INIT failed: %d (continuing)\n", ret);

	/* Step 2b: PHY thresholds - set per-channel sensitivity after
	 * LMAC init. Gamely sends RPU_CMD_UPD_PHY_THRESH post-init. */
	ret = sv6160_rpu_phy_thresh(sc);
	if (ret)
		dev_warn(sc->dev, "PHY thresh failed: %d (continuing)\n", ret);

	/* Step 3: TX power */
	ret = sv6160_rpu_tx_power(sc, 20);
	if (ret)
		dev_warn(sc->dev, "TX power programming failed: %d\n", ret);

	/* Step 4: NOP ping */
	ret = sv6160_rpu_fw_priv_nop(sc);
	if (ret == 0)
		dev_info(sc->dev, "Firmware data path verified (NOP OK)\n");
	else
		dev_warn(sc->dev, "Firmware NOP failed: %d - data path may be broken\n", ret);

	/* Enable interrupt generation */
	sv6160_sdio_writeb(sc, 34, 0x02);
	sv6160_sdio_writeb(sc, 33, 0x02);

	sc->is_started = true;
	sc->last_activity = jiffies;

	/* Send deferred VIF add - mac80211 calls add_interface before start,
	 * so is_rpu_ready was false when add_interface ran.  Send now. */
	if (sc->vif) {
		int vif_mode = VIF_MODE_STA;

		if (sc->vif->type == NL80211_IFTYPE_AP)
			vif_mode = VIF_MODE_AP;
		else if (sc->vif->type == NL80211_IFTYPE_ADHOC)
			vif_mode = VIF_MODE_IBSS;

		ret = sv6160_rpu_add_vif(sc, sc->vif->addr, vif_mode);
		dev_info(sc->dev, "VIF add (deferred): mode=%d addr=%pM ret=%d\n",
			 vif_mode, sc->vif->addr, ret);

		/* Program initial channel (gamely does rpu_prog_channel
		 * right after VIF add - firmware may need a channel
		 * configured before it can process scan commands). */
		sv6160_rpu_channel(sc, 1, 0);  /* channel 1, 2.4 GHz */
	}

	/* GPIO level IRQ + SDIO in-band IRQ drive RX post-init.
	 * Command wait loops have their own polling. */
	schedule_delayed_work(&sc->idle_work, msecs_to_jiffies(5000));

	dev_info(sc->dev, "mac80211 started, RPU ready\n");
	return 0;
}

static void sv6160_mac_stop(struct ieee80211_hw *hw)
{
	struct sv6160_softc *sc = hw->priv;
	int i;

	sc->is_started = false;
	sc->is_rpu_ready = false;
	sc->beacon_enabled = false;
	sc->scan_in_progress = false;
	del_timer_sync(&sc->bcn_timer);
	cancel_delayed_work_sync(&sc->scan_timeout);
	cancel_delayed_work_sync(&sc->scan_poll);
	cancel_delayed_work_sync(&sc->scan_work);
	sc->scan_in_progress = false;
	cancel_delayed_work_sync(&sc->roc_timeout);
	cancel_delayed_work_sync(&sc->idle_work);
	cancel_delayed_work_sync(&sc->tx_rx_poll);
	cancel_delayed_work_sync(&sc->rx_poll);
	cancel_work_sync(&sc->rx_work);
	skb_queue_purge(&sc->rx_queue);
	cancel_work_sync(&sc->tx_work);
	skb_queue_purge(&sc->tx_queue);

	/* Flush any pending TX frames */
	for (i = 0; i < SV6160_TX_PENDING_MAX; i++) {
		if (sc->tx_pending[i]) {
			ieee80211_free_txskb(hw, sc->tx_pending[i]);
			sc->tx_pending[i] = NULL;
		}
	}
	atomic_set(&sc->tx_outstanding, 0);

	dev_info(sc->dev, "mac80211 stopped\n");
}

static int sv6160_mac_add_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct sv6160_softc *sc = hw->priv;
	int mode;

	if (sc->vif)
		return -EBUSY;

	switch (vif->type) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
		mode = VIF_MODE_STA;
		break;
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		mode = VIF_MODE_AP;
		break;
	case NL80211_IFTYPE_ADHOC:
		mode = VIF_MODE_IBSS;
		break;
	case NL80211_IFTYPE_MONITOR:
		mode = VIF_MODE_STA;
		break;
	default:
		return -EOPNOTSUPP;
	}

	sc->vif = vif;

	if (sc->is_rpu_ready) {
		int ret = sv6160_rpu_add_vif(sc, vif->addr, mode);
		dev_info(sc->dev, "VIF add: type=%d mode=%d addr=%pM ret=%d\n",
			 vif->type, mode, vif->addr, ret);

	} else {
		dev_warn(sc->dev, "VIF add deferred - RPU not ready\n");
	}

	return 0;
}

static void sv6160_mac_remove_interface(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif)
{
	struct sv6160_softc *sc = hw->priv;

	if (sc->is_rpu_ready)
		sv6160_rpu_remove_vif(sc);

	sc->vif = NULL;
}

static int sv6160_mac_config(struct ieee80211_hw *hw, u32 changed)
{
	struct sv6160_softc *sc = hw->priv;
	struct ieee80211_conf *conf = &hw->conf;

	if (!sc->is_rpu_ready)
		return 0;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL)
		sv6160_rpu_channel(sc, conf->chandef.chan->hw_value, 0);

	return 0;
}

static void sv6160_mac_configure_filter(struct ieee80211_hw *hw,
					unsigned int changed_flags,
					unsigned int *total_flags,
					u64 multicast)
{
	struct sv6160_softc *sc = hw->priv;

	*total_flags &= FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC |
			FIF_OTHER_BSS | FIF_FCSFAIL;

	/* Tell firmware to receive beacons - without this, the firmware
	 * never delivers WiFi RX frames to the host. Gamely sets
	 * RCV_ALL_BCNS (0) in configure_filter. */
	if (sc->is_rpu_ready) {
		struct rpu_cmd_vif_cfg cmd;

		memset(&cmd, 0, sizeof(cmd));
		cmd.changed_bitmap = cpu_to_le32(1 << 10); /* RCV_BCN_MODE_CHANGED */
		cmd.bcn_mode = cpu_to_le32(0); /* RCV_ALL_BCNS */
		cmd.if_index = cpu_to_le32(sc->vif_index);
		if (sc->vif)
			memcpy(cmd.vif_addr, sc->vif->addr, 6);

		sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_VIF_CFG);
		dev_info(sc->dev, "configure_filter: set RCV_ALL_BCNS\n");
	}
}

static void sv6160_mac_bss_info_changed(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					struct ieee80211_bss_conf *info,
					u32 changed)
{
	struct sv6160_softc *sc = hw->priv;

	if (!sc->is_rpu_ready)
		return;

	if (changed & BSS_CHANGED_BSSID) {
		dev_dbg(sc->dev, "Setting BSSID %pM (STA_CONN)\n",
			info->bssid);
		/* Send BSSID + STA_CONN together - firmware needs to know
		 * we're connecting BEFORE auth, otherwise it won't forward
		 * auth/assoc response frames to the host. The gamely BSP
		 * sets connect state during association setup, not after. */
		sv6160_rpu_vif_cfg(sc, info->bssid,
				   VIF_CFG_BSSID_CHANGED |
				   VIF_CFG_CONNECT_STATE,
				   STA_CONN, 0, 0);
	}

	if (changed & BSS_CHANGED_ASSOC) {
		if (info->assoc) {
			dev_info(sc->dev, "Associated to %pM\n", info->bssid);
			sv6160_rpu_vif_cfg(sc, info->bssid,
					   VIF_CFG_CONNECT_STATE | VIF_CFG_AID_CHANGED |
					   VIF_CFG_OP_CHAN_CHANGED,
					   STA_CONN, info->aid,
					   info->chandef.chan ? info->chandef.chan->hw_value : 0);
		} else {
			dev_info(sc->dev, "Disassociated\n");
			sv6160_rpu_vif_cfg(sc, info->bssid,
					   VIF_CFG_CONNECT_STATE,
					   STA_DISCONN, 0, 0);
		}
	}

	if (changed & BSS_CHANGED_BEACON_ENABLED) {
		if (info->enable_beacon)
			sv6160_beacon_start(sc, info->beacon_int ?: 100);
		else
			sv6160_beacon_stop(sc);
	}

	if (changed & BSS_CHANGED_BEACON_INT) {
		sc->beacon_int = info->beacon_int;
		sv6160_rpu_vif_cfg(sc, info->bssid,
				   VIF_CFG_BCN_INT_CHANGED, 0, 0, 0);
	}

	if (changed & BSS_CHANGED_BASIC_RATES) {
		/* Send basic rate set to firmware */
		sv6160_rpu_vif_cfg(sc, info->bssid,
				   VIF_CFG_BASICRATES_CHANGED, 0, 0, 0);
	}

	if (changed & BSS_CHANGED_ERP_SLOT) {
		sv6160_rpu_vif_cfg(sc, info->bssid,
				   VIF_CFG_SHORTSLOT_CHANGED, 0, 0, 0);
	}
}

/*
 * TX worker - drains the deferred TX queue in process context where
 * sdio_claim_host() (which takes a mutex) is safe.  mac80211 calls
 * sv6160_mac_tx() from softirq (tasklet), so we cannot do SDIO I/O
 * there directly.
 *
 * TX protocol (from gamely BSP):
 * 1. Write 512-byte cmd_tx_ctrl header to SDIO addr 0 (RPU_CMD_TX=7)
 * 2. Write raw 802.11 frame data to SDIO addr 0 separately
 * Firmware reads metadata from the header, then reads frame data
 * from the second write, and queues for radio TX.
 */
static void sv6160_tx_worker(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       tx_work);
	struct sk_buff *skb;
	u8 *tx_buf;  /* combined cmd_tx_ctrl + frame data buffer */

	tx_buf = kmalloc(SV6160_TX_CMD_SIZE + SV6160_MAX_RX_SIZE, GFP_KERNEL);
	if (!tx_buf)
		return;

	while ((skb = skb_dequeue(&sc->tx_queue))) {
		struct sv6160_cmd_tx_ctrl *tx_cmd;
		struct ieee80211_hdr *wh;
		u32 desc_id, frame_len;
		int slot, ret, hdrlen;

		if (!sc->is_rpu_ready) {
			ieee80211_free_txskb(sc->hw, skb);
			continue;
		}

		/* desc_id was stashed in cb[] by mac_tx */
		desc_id = *(u32 *)skb->cb;
		slot = desc_id % SV6160_TX_PENDING_MAX;

		if (sc->tx_pending[slot]) {
			dev_warn(sc->dev,
				 "TX pending slot %d overrun, dropping stale\n",
				 slot);
			ieee80211_free_txskb(sc->hw, sc->tx_pending[slot]);
			sc->tx_pending[slot] = NULL;
			atomic_dec(&sc->tx_outstanding);
		}

		wh = (struct ieee80211_hdr *)skb->data;
		frame_len = skb->len;
		hdrlen = ieee80211_hdrlen(wh->frame_control);

		/* Build combined TX buffer: 512-byte header + frame data.
		 * Gamely BSP sends both in a single scatter-gather SDIO
		 * write. The firmware expects frame data immediately
		 * after the cmd_tx_ctrl in the same SDIO transaction. */
		memset(tx_buf, 0, SV6160_TX_CMD_SIZE);
		tx_cmd = (struct sv6160_cmd_tx_ctrl *)tx_buf;

		tx_cmd->hdr.id = cpu_to_le32(RPU_CMD_TX);
		tx_cmd->hdr.descriptor_id = cpu_to_le32(
			(0 << 16) | (desc_id & 0xFFFF));  /* queue | desc */
		tx_cmd->hdr.length = cpu_to_le32(SV6160_TX_CMD_SIZE);

		tx_cmd->if_index = sc->vif_index;
		tx_cmd->queue_num = 0;  /* BE queue */
		tx_cmd->descriptor_id = cpu_to_le32(desc_id);
		tx_cmd->num_frames_per_desc = cpu_to_le32(1);
		tx_cmd->pkt_length[0] = cpu_to_le32(frame_len);

		/* Copy MAC header into control struct (gamely BSP always
		 * does this - firmware uses it for GRAM frame setup) */
		if (hdrlen > SV6160_MAC_HDR_SIZE)
			hdrlen = SV6160_MAC_HDR_SIZE;
		memcpy(tx_cmd->config_mac_header, skb->data, hdrlen);
		tx_cmd->config_mac_hdr_len = cpu_to_le32(hdrlen);

		/* Copy seq_ctrl into per_pkt_crypto_params (gamely BSP) */
		memcpy(tx_cmd->per_pkt_crypto_params[0],
		       &wh->seq_ctrl, 2);

		/* Rate: match gamely BSP defaults for mgmt frames.
		 * 1 Mbps (hw_value=2), long preamble, 1 spatial stream */
		tx_cmd->num_rates = 1;
		tx_cmd->rate[0] = 2;  /* 1 Mbps */
		tx_cmd->rate_retries[0] = 5;
		tx_cmd->rate_preamble_type[0] = 1; /* long preamble */
		tx_cmd->num_spatial_streams[0] = 1;

		dev_info(sc->dev,
			 "TX: id=%u len=%u hdrlen=%d %s desc_id=%u\n",
			 RPU_CMD_TX, frame_len, hdrlen,
			 ieee80211_is_mgmt(wh->frame_control) ?
			 "mgmt" : "data", desc_id);
		print_hex_dump_debug("  tx_frame: ",
				     DUMP_PREFIX_OFFSET, 16, 1,
				     skb->data, min_t(u32, frame_len, 64),
				     true);

		/* Two separate SDIO writes (gamely BSP default non-SG path):
		 * 1. cmd_tx_ctrl header (512 bytes)
		 * 2. raw 802.11 frame data */
		ret = sv6160_sdio_write_data(sc, SDIO_DATA_ADDR,
					     tx_buf, SV6160_TX_CMD_SIZE);
		if (ret) {
			dev_err(sc->dev, "TX cmd write failed: %d\n", ret);
			ieee80211_free_txskb(sc->hw, skb);
			continue;
		}
		ret = sv6160_sdio_write_data(sc, SDIO_DATA_ADDR,
					     skb->data,
					     ALIGN(frame_len, 4));
		if (ret) {
			dev_err(sc->dev, "TX write failed: %d\n", ret);
			ieee80211_free_txskb(sc->hw, skb);
			continue;
		}

		/* GPIO will fire when firmware has TX_DONE response. */

		/* Start RX polling to pick up auth/assoc responses */
		sc->last_tx_time = jiffies;
		if (!delayed_work_pending(&sc->tx_rx_poll))
			schedule_delayed_work(&sc->tx_rx_poll,
					      msecs_to_jiffies(50));

		sc->tx_pending[slot] = skb;
		if (atomic_inc_return(&sc->tx_outstanding) >=
		    SV6160_TX_PENDING_MAX - 4)
			ieee80211_stop_queues(sc->hw);
	}

	kfree(tx_buf);
}

static void sv6160_mac_tx(struct ieee80211_hw *hw,
			  struct ieee80211_tx_control *control,
			  struct sk_buff *skb)
{
	struct sv6160_softc *sc = hw->priv;
	u32 desc_id;

	if (!sc->is_rpu_ready) {
		ieee80211_free_txskb(hw, skb);
		return;
	}

	sc->last_activity = jiffies;

	/* Stash desc_id in skb->cb for the TX worker.
	 * The skb contains the raw 802.11 frame - no prepended header.
	 * The TX worker builds the 512-byte cmd_tx_ctrl separately. */
	desc_id = sc->tx_desc_id++;
	*(u32 *)skb->cb = desc_id;

	/* Enqueue and schedule - SDIO write happens in process context */
	skb_queue_tail(&sc->tx_queue, skb);
	schedule_work(&sc->tx_work);
}

/* Channel-sweep scan: host-driven scanning without pkt_type=0x16.
 *
 * Firmware RE shows that pkt_type=0x16 (scan handler) BREAKS the radio:
 * the scan processor writes *0x42000034=0 (RF disable) and clears WiFi
 * MAC control bit 19, killing GPIO notifications for ALL subsequent events.
 *
 * Instead, we implement scan as a channel sweep:
 * 1. Program channel via RPU_CMD_CHANNEL
 * 2. Send NOP to trigger LMAC processor (which processes pending RX frames)
 * 3. Dwell 120ms (beacon interval is typically 100ms)
 * 4. Move to next channel
 *
 * Beacons received on each channel are delivered via the normal RX path
 * (event_enqueue → sdio_tx_trigger → GPIO → rx_worker → mac80211).
 */
static void sv6160_scan_work_fn(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       scan_work.work);
	struct cfg80211_scan_request *sreq;
	int ch_idx;

	if (!sc->scan_request || !sc->is_rpu_ready || READ_ONCE(sc->removing)) {
		sc->scan_in_progress = false;
		return;
	}

	sreq = &sc->scan_request->req;

	/* Two-phase per channel: even idx = program channel + dwell,
	 * odd idx = send NOP to flush RX after dwell, then advance.
	 * The NOP triggers the LMAC processor which delivers any
	 * beacons received during the dwell period. */
	ch_idx = sc->scan_ch_idx / 2;

	if (ch_idx >= sreq->n_channels) {
		/* All channels scanned - complete */
		struct cfg80211_scan_info info = { .aborted = false };

		dev_info(sc->dev, "scan complete: swept %d channels\n",
			 sreq->n_channels);
		cancel_delayed_work(&sc->scan_timeout);
		ieee80211_scan_completed(sc->hw, &info);
		sc->scan_request = NULL;
		sc->scan_in_progress = false;
		return;
	}

	if (sc->scan_ch_idx % 2 == 0) {
		/* Phase 1: program channel, then dwell */
		dev_dbg(sc->dev, "scan: ch %d/%d (hw_value=%d)\n",
			ch_idx + 1, sreq->n_channels,
			sreq->channels[ch_idx]->hw_value);
		sv6160_rpu_channel(sc, sreq->channels[ch_idx]->hw_value, 0);
		sc->scan_ch_idx++;

		/* Dwell 150ms for beacon reception */
		schedule_delayed_work(&sc->scan_work, msecs_to_jiffies(150));
	} else {
		/* Phase 2: after dwell, re-send channel command to trigger
		 * LMAC processor. NOP responses don't trigger GPIO (firmware
		 * blocks them via *0x10005514 check). But channel commands
		 * DO trigger GPIO and also run the LMAC processor which
		 * delivers any pending WiFi RX frames. */
		sv6160_rpu_channel(sc, sreq->channels[ch_idx]->hw_value, 0);
		sc->scan_ch_idx++;

		/* Brief pause then next channel */
		schedule_delayed_work(&sc->scan_work, msecs_to_jiffies(10));
	}
}

/*
 * TX-triggered RX polling - after each TX frame, poll for RX data for
 * up to 3 seconds.  This picks up auth/assoc responses that the firmware
 * buffers in SDIO but can't signal via GPIO (GPIO doesn't fire post-init).
 */
static void sv6160_tx_rx_poll_fn(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       tx_rx_poll.work);

	if (!sc->is_started || !sc->is_rpu_ready)
		return;

	/* Post-init: safe to reset stale guard for TX response pickup */
	sc->last_recv_len = 0;
	sv6160_trigger_rx(sc);

	/* Keep polling every 50ms for up to 3s after last TX */
	if (time_before(jiffies, sc->last_tx_time + msecs_to_jiffies(3000)))
		schedule_delayed_work(&sc->tx_rx_poll, msecs_to_jiffies(50));
}

static void sv6160_scan_poll_fn(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       scan_poll.work);

	if (!sc->scan_in_progress || READ_ONCE(sc->removing))
		return;

	/* Reset stale guard - post-init, firmware TX path is enabled
	 * so RECV_LEN reflects real data, not boot-time stale values.
	 * Init polling does NOT reset the guard (stale reads kill bus). */
	sc->last_recv_len = 0;
	sv6160_trigger_rx(sc);
	schedule_delayed_work(&sc->scan_poll, msecs_to_jiffies(500));
}

static void sv6160_scan_timeout_fn(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       scan_timeout.work);

	if (sc->scan_request) {
		struct cfg80211_scan_info info = { .aborted = true };

		dev_warn(sc->dev, "scan timeout at ch %d\n", sc->scan_ch_idx);
		cancel_delayed_work(&sc->scan_poll);
		cancel_delayed_work(&sc->scan_work);
		sc->scan_in_progress = false;

		/* Re-enable GPIO IRQ after scan timeout + clear state */
		if (sc->gpio_irq_disabled && sc->host_wake_irq > 0 &&
		    !READ_ONCE(sc->removing)) {
			int err;

			sdio_claim_host(sc->func);
			sdio_writeb(sc->func, SDIO_REG_INT_CLR_VAL,
				    SDIO_REG_INT_ADDR, &err);
			sdio_writeb(sc->func, 1, SDIO_REG_NOTIFY_ADDR, &err);
			sv6160_sdio_write_comp(sc->func);
			sdio_release_host(sc->func);
			usleep_range(2000, 3000);
			sc->gpio_irq_disabled = false;
			enable_irq(sc->host_wake_irq);
			dev_info(sc->dev, "GPIO IRQ re-enabled after scan timeout\n");
		}

		ieee80211_scan_completed(sc->hw, &info);
		sc->scan_request = NULL;
	}
}

static int sv6160_mac_hw_scan(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_scan_request *req)
{
	struct sv6160_softc *sc = hw->priv;

	if (!sc->is_rpu_ready)
		return -EIO;

	if (sc->scan_in_progress) {
		/* Previous scan still in-flight - abort it cleanly so
		 * mac80211's scan FSM doesn't get stuck. */
		struct cfg80211_scan_info info = { .aborted = true };

		cancel_delayed_work_sync(&sc->scan_poll);
		cancel_delayed_work_sync(&sc->scan_work);
		cancel_delayed_work_sync(&sc->scan_timeout);
		if (sc->scan_request)
			ieee80211_scan_completed(sc->hw, &info);
		sc->scan_request = NULL;
		sc->scan_in_progress = false;
	}

	sc->scan_request = req;
	sc->scan_in_progress = true;
	sc->scan_ch_idx = 0;

	/* Disable GPIO IRQ during scan.  The original (unpatched) firmware
	 * clears GPIO bit 19 during scan_stop, which can cause the
	 * level-triggered IRQ to enter a rapid fire/disable/re-enable cycle
	 * that destabilizes USB.  Use scan_poll for RX during scan instead.
	 * GPIO IRQ is re-enabled after scan completes or times out. */
	if (sc->host_wake_irq > 0) {
		disable_irq_nosync(sc->host_wake_irq);
		sc->gpio_irq_disabled = true;
	}

	/* Try RPU_CMD_SCAN (id=1) first - goes through tier-2 LMAC
	 * processor which handles both scan and RX delivery.
	 * Gamely uses this path. If it works, beacons arrive via
	 * the LMAC processor's WiFi RX frame delivery. */
	{
		int ret;

		dev_info(sc->dev, "hw_scan: %d channels (RPU_CMD_SCAN mode)\n",
			 req->req.n_channels);
		ret = sv6160_rpu_scan(sc, req);
		if (ret) {
			dev_warn(sc->dev, "RPU_CMD_SCAN failed (%d), trying channel-sweep\n", ret);
			/* Fallback to channel-sweep */
			schedule_delayed_work(&sc->scan_work, 0);
		} else {
			/* Poll for scan results asynchronously */
			schedule_delayed_work(&sc->scan_poll,
					      msecs_to_jiffies(500));
		}
	}

	/* Timeout for scan completion */
	schedule_delayed_work(&sc->scan_timeout,
			      msecs_to_jiffies(15000));

	return 0;
}

static void sv6160_mac_cancel_hw_scan(struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif)
{
	struct sv6160_softc *sc = hw->priv;

	cancel_delayed_work_sync(&sc->scan_timeout);
	cancel_delayed_work_sync(&sc->scan_poll);
	cancel_delayed_work_sync(&sc->scan_work);
	if (sc->scan_request) {
		struct cfg80211_scan_info info = { .aborted = true };
		ieee80211_scan_completed(sc->hw, &info);
		sc->scan_request = NULL;
	}
	sc->scan_in_progress = false;
}

static int sv6160_mac_conf_tx(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      u16 queue,
			      const struct ieee80211_tx_queue_params *params)
{
	struct sv6160_softc *sc = hw->priv;
	struct rpu_cmd_txq_params cmd;

	if (!sc->is_rpu_ready)
		return 0;

	memset(&cmd, 0, sizeof(cmd));
	cmd.if_index = cpu_to_le32(sc->vif_index);
	cmd.queue = cpu_to_le32(queue);
	cmd.txop = cpu_to_le16(params->txop);
	cmd.cwmin = cpu_to_le16(params->cw_min);
	cmd.cwmax = cpu_to_le16(params->cw_max);
	cmd.aifs = params->aifs;

	/* RPU_CMD_TXQ_PARAMS (26) → firmware tier-2 LMAC processor (verified) */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd), RPU_CMD_TXQ_PARAMS);
}

int sv6160_rpu_ba_session(struct sv6160_softc *sc, u32 op, u32 tid,
			  u32 ssn, const u8 *vif_addr, const u8 *peer_addr)
{
	struct rpu_cmd_ba_session cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.if_index = cpu_to_le32(sc->vif_index);
	cmd.op = cpu_to_le32(op);
	cmd.tid = cpu_to_le32(tid);
	cmd.ssn = cpu_to_le32(ssn);
	cmd.policy = cpu_to_le32(1); /* immediate BA */
	memcpy(cmd.vif_addr, vif_addr, ETH_ALEN);
	memcpy(cmd.peer_addr, peer_addr, ETH_ALEN);

	/* RPU_CMD_BA_SESSION_INFO (18) → firmware tier-2 LMAC processor (verified) */
	return sv6160_rpu_send_cmd(sc, &cmd, sizeof(cmd),
				   RPU_CMD_BA_SESSION_INFO);
}

static int sv6160_mac_ampdu_action(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_ampdu_params *params)
{
	struct sv6160_softc *sc = hw->priv;
	struct ieee80211_sta *sta = params->sta;
	enum ieee80211_ampdu_mlme_action action = params->action;
	u16 tid = params->tid;
	const u8 *vif_addr = vif->addr;
	int ret;

	if (!sc->is_rpu_ready)
		return -EIO;

	switch (action) {
	case IEEE80211_AMPDU_TX_START:
		ret = sv6160_rpu_ba_session(sc, 1, tid, params->ssn,
					    vif_addr, sta->addr);
		if (ret)
			return ret;
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		return 0;

	case IEEE80211_AMPDU_TX_STOP_CONT:
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		ret = sv6160_rpu_ba_session(sc, 0, tid, 0,
					    vif_addr, sta->addr);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		return ret;

	case IEEE80211_AMPDU_TX_OPERATIONAL:
		/* Firmware manages TX aggregation once session is started */
		return 0;

	case IEEE80211_AMPDU_RX_START:
	case IEEE80211_AMPDU_RX_STOP:
		/* Firmware manages RX aggregation internally */
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static int sv6160_mac_set_key(struct ieee80211_hw *hw,
			      enum set_key_cmd cmd,
			      struct ieee80211_vif *vif,
			      struct ieee80211_sta *sta,
			      struct ieee80211_key_conf *key)
{
	struct sv6160_softc *sc = hw->priv;
	struct rpu_cmd_setkey setkey;
	u32 cipher;
	int ret;

	if (!sc->is_rpu_ready)
		return -EIO;

	memset(&setkey, 0, sizeof(setkey));
	setkey.if_index = cpu_to_le32(sc->vif_index);
	setkey.ctrl = cpu_to_le32(cmd == SET_KEY ? KEY_CTRL_ADD : KEY_CTRL_DEL);
	setkey.key_id = cpu_to_le32(key->keyidx);

	/* Map mac80211 cipher to firmware cipher type */
	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		cipher = CIPHER_TYPE_WEP40;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		cipher = CIPHER_TYPE_WEP104;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		cipher = CIPHER_TYPE_TKIP;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		cipher = CIPHER_TYPE_CCMP;
		break;
	default:
		return -EOPNOTSUPP;
	}
	setkey.cipher_type = cpu_to_le32(cipher);

	/* Pairwise (unicast) vs group (broadcast) key */
	if (sta) {
		setkey.key_type = cpu_to_le32(KEY_TYPE_UCAST);
		memcpy(setkey.mac_addr, sta->addr, ETH_ALEN);
	} else {
		setkey.key_type = cpu_to_le32(KEY_TYPE_BCAST);
		eth_broadcast_addr(setkey.mac_addr);
	}

	setkey.key_len = cpu_to_le32(key->keylen);
	if (key->keylen > TOTAL_KEY_LEN)
		return -EINVAL;
	memcpy(setkey.key, key->key, key->keylen);

	/* RSC not populated at set_key time - mac80211 manages replay */

	dev_info(sc->dev, "set_key: %s cipher=0x%x keyidx=%d keylen=%d %s\n",
		 cmd == SET_KEY ? "ADD" : "DEL", key->cipher,
		 key->keyidx, key->keylen,
		 sta ? "pairwise" : "group");

	/* RPU_CMD_SETKEY (4) → firmware tier-2 LMAC processor (verified) */
	ret = sv6160_rpu_send_cmd(sc, &setkey, sizeof(setkey),
				  RPU_CMD_SETKEY);
	if (ret)
		return ret;

	if (cmd == SET_KEY) {
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;
		if (key->cipher == WLAN_CIPHER_SUITE_TKIP)
			key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC;
	}

	return 0;
}

/* ===================================================================
 * Remain-on-Channel (P2P)
 * =================================================================== */

static void sv6160_roc_timeout_fn(struct work_struct *work)
{
	struct sv6160_softc *sc = container_of(work, struct sv6160_softc,
					       roc_timeout.work);

	if (sc->roc_in_progress) {
		sc->roc_in_progress = false;
		ieee80211_remain_on_channel_expired(sc->hw);
	}
}

static int sv6160_mac_remain_on_channel(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					struct ieee80211_channel *chan,
					int duration,
					enum ieee80211_roc_type type)
{
	struct sv6160_softc *sc = hw->priv;
	u32 roc_type;
	int ret;

	if (sc->roc_in_progress)
		return -EBUSY;

	roc_type = (type == IEEE80211_ROC_TYPE_MGMT_TX) ?
		    ROC_TYPE_OFFCHAN_TX : ROC_TYPE_NORMAL;

	ret = sv6160_rpu_roc(sc, ROC_START, chan->hw_value, duration, roc_type);
	if (ret)
		return ret;

	sc->roc_in_progress = true;
	sc->roc_chan = chan;
	schedule_delayed_work(&sc->roc_timeout, msecs_to_jiffies(duration));

	return 0;
}

static int sv6160_mac_cancel_remain_on_channel(struct ieee80211_hw *hw)
{
	struct sv6160_softc *sc = hw->priv;

	cancel_delayed_work_sync(&sc->roc_timeout);
	sv6160_rpu_roc(sc, ROC_STOP, 0, 0, 0);
	sc->roc_in_progress = false;

	return 0;
}

static int sv6160_mac_set_rts_threshold(struct ieee80211_hw *hw,
					u32 value)
{
	struct sv6160_softc *sc = hw->priv;

	/* Firmware handles RTS internally */
	dev_dbg(sc->dev, "RTS threshold set to %u\n", value);
	return 0;
}

/* ===================================================================
 * Interface Combination Limits (P2P)
 * =================================================================== */

static void sv6160_mac_flush(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif, u32 queues, bool drop)
{
	struct sv6160_softc *sc = hw->priv;
	int i, timeout = 50; /* 50ms max wait */

	if (drop) {
		/* Force-complete all pending TX */
		for (i = 0; i < SV6160_TX_PENDING_MAX; i++) {
			if (sc->tx_pending[i]) {
				ieee80211_free_txskb(hw, sc->tx_pending[i]);
				sc->tx_pending[i] = NULL;
			}
		}
		atomic_set(&sc->tx_outstanding, 0);
		return;
	}
	/* Wait for pending TX to complete naturally */
	while (atomic_read(&sc->tx_outstanding) > 0 && timeout-- > 0)
		msleep(1);
}

const struct ieee80211_ops sv6160_ops = {
	.tx                = sv6160_mac_tx,
	.start             = sv6160_mac_start,
	.stop              = sv6160_mac_stop,
	.add_interface     = sv6160_mac_add_interface,
	.remove_interface  = sv6160_mac_remove_interface,
	.config            = sv6160_mac_config,
	.configure_filter  = sv6160_mac_configure_filter,
	.bss_info_changed  = sv6160_mac_bss_info_changed,
	.set_key           = sv6160_mac_set_key,
	.ampdu_action      = sv6160_mac_ampdu_action,
	.conf_tx           = sv6160_mac_conf_tx,
	.hw_scan           = sv6160_mac_hw_scan,
	.cancel_hw_scan    = sv6160_mac_cancel_hw_scan,
	.remain_on_channel = sv6160_mac_remain_on_channel,
	.cancel_remain_on_channel = sv6160_mac_cancel_remain_on_channel,
	.set_rts_threshold = sv6160_mac_set_rts_threshold,
	.flush             = sv6160_mac_flush,
};

/* ===================================================================
 * Band/Channel Setup & Core Init
 * =================================================================== */

static void sv6160_setup_ht_cap(struct sv6160_softc *sc)
{
	struct ieee80211_sta_ht_cap *ht_cap = &sc->band_2ghz.ht_cap;

	ht_cap->ht_supported = true;

	/*
	 * SV6160 is 2.4 GHz b/g/n, single spatial stream, 20 MHz only.
	 * No 40 MHz, no STBC, no LDPC (single antenna).
	 */
	ht_cap->cap = IEEE80211_HT_CAP_SGI_20; /* 20 MHz only, no 40 MHz */

	ht_cap->ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K;
	ht_cap->ampdu_density = IEEE80211_HT_MPDU_DENSITY_8;

	/* Single spatial stream: MCS 0-7 */
	memset(&ht_cap->mcs, 0, sizeof(ht_cap->mcs));
	ht_cap->mcs.rx_mask[0] = 0xff; /* MCS 0-7 */
	ht_cap->mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED;
}

static void sv6160_setup_band(struct sv6160_softc *sc)
{
	memcpy(sc->channels, sv6160_2ghz_channels, sizeof(sv6160_2ghz_channels));
	memcpy(sc->rates, sv6160_2ghz_rates, sizeof(sv6160_2ghz_rates));

	sc->band_2ghz.channels  = sc->channels;
	sc->band_2ghz.n_channels = ARRAY_SIZE(sv6160_2ghz_channels);
	sc->band_2ghz.bitrates  = sc->rates;
	sc->band_2ghz.n_bitrates = ARRAY_SIZE(sv6160_2ghz_rates);
	sc->band_2ghz.band      = NL80211_BAND_2GHZ;

	sc->hw->wiphy->bands[NL80211_BAND_2GHZ] = &sc->band_2ghz;
}

/* ===================================================================
 * debugfs - runtime inspection of driver state
 * =================================================================== */

static int sv6160_debugfs_scan_state_show(struct seq_file *s, void *data)
{
	struct sv6160_softc *sc = s->private;

	if (sc->scan_request)
		seq_puts(s, "scanning\n");
	else if (delayed_work_pending(&sc->scan_timeout))
		seq_puts(s, "timeout\n");
	else
		seq_puts(s, "idle\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sv6160_debugfs_scan_state);

static int sv6160_debugfs_fw_state_show(struct seq_file *s, void *data)
{
	struct sv6160_softc *sc = s->private;
	static const char * const names[] = {
		[FW_STATE_WAIT_FW]        = "wait_fw",
		[FW_STATE_M0_STARTED]     = "m0_started",
		[FW_STATE_WAIT_PATCH]     = "wait_patch",
		[FW_STATE_WAIT_PATCH2]    = "wait_patch2",
		[FW_STATE_LPW_READY]      = "lpw_ready",
		[FW_STATE_M0_READY]       = "m0_ready",
		[FW_STATE_M0_SLEEP]       = "m0_sleep",
		[FW_STATE_RESET_COMPLETE] = "reset_complete",
	};
	u8 val = 0;
	int ret;

	sdio_claim_host(sc->func);
	ret = sv6160_sdio_readb(sc, SDIO_REG_FW_STATE, &val);
	sdio_release_host(sc->func);

	if (ret)
		seq_printf(s, "error: %d\n", ret);
	else if (val < ARRAY_SIZE(names) && names[val])
		seq_printf(s, "%s (%u)\n", names[val], val);
	else
		seq_printf(s, "unknown (%u)\n", val);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sv6160_debugfs_fw_state);

static void sv6160_debugfs_init(struct sv6160_softc *sc)
{
	struct dentry *dir;

	dir = debugfs_create_dir("sv6160", sc->hw->wiphy->debugfsdir);
	if (IS_ERR_OR_NULL(dir))
		return;

	sc->debugfs_dir = dir;

	debugfs_create_u32("gpio_irq_count", 0444, dir, &sc->gpio_irq_count);
	debugfs_create_u16("last_recv_len", 0444, dir, &sc->last_recv_len);
	debugfs_create_atomic_t("tx_outstanding", 0444, dir,
				&sc->tx_outstanding);
	debugfs_create_file("scan_state", 0444, dir, sc,
			    &sv6160_debugfs_scan_state_fops);
	debugfs_create_file("fw_state", 0444, dir, sc,
			    &sv6160_debugfs_fw_state_fops);
}

int sv6160_core_init(struct sv6160_softc *sc)
{
	int ret;
	u8 mac[ETH_ALEN];

	sc->coalescing.enabled          = mod_coalescing;
	sc->coalescing.rx_bundle_limit  = mod_rx_bundle;

	init_completion(&sc->reset_done);
	init_completion(&sc->chan_prog_done);
	init_completion(&sc->cmd_done);

	sc->last_recv_len = 0;
	sc->recv_len_failed = false;

	INIT_WORK(&sc->rx_work, sv6160_rx_worker);
	INIT_WORK(&sc->tx_work, sv6160_tx_worker);
	INIT_DELAYED_WORK(&sc->rx_poll, sv6160_rx_poll_fn);
	INIT_DELAYED_WORK(&sc->idle_work, sv6160_idle_work_fn);
	INIT_DELAYED_WORK(&sc->roc_timeout, sv6160_roc_timeout_fn);
	INIT_DELAYED_WORK(&sc->scan_timeout, sv6160_scan_timeout_fn);
	INIT_DELAYED_WORK(&sc->scan_poll, sv6160_scan_poll_fn);
	INIT_DELAYED_WORK(&sc->tx_rx_poll, sv6160_tx_rx_poll_fn);
	INIT_DELAYED_WORK(&sc->scan_work, sv6160_scan_work_fn);
	timer_setup(&sc->bcn_timer, sv6160_bcn_timer_fn, 0);
	skb_queue_head_init(&sc->rx_queue);
	skb_queue_head_init(&sc->tx_queue);

	sc->hw->wiphy->max_scan_ssids    = 4;
	sc->hw->wiphy->max_scan_ie_len   = 256;
	sc->hw->wiphy->interface_modes   = BIT(NL80211_IFTYPE_STATION) |
					   BIT(NL80211_IFTYPE_AP) |
					   BIT(NL80211_IFTYPE_ADHOC) |
					   BIT(NL80211_IFTYPE_MONITOR) |
					   BIT(NL80211_IFTYPE_P2P_CLIENT) |
					   BIT(NL80211_IFTYPE_P2P_GO);
	sc->hw->wiphy->max_scan_ssids    = 4;
	sc->hw->wiphy->flags            |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;
	sc->hw->wiphy->max_remain_on_channel_duration = 10000;

	ieee80211_hw_set(sc->hw, SIGNAL_DBM);
	ieee80211_hw_set(sc->hw, HOST_BROADCAST_PS_BUFFERING);
	ieee80211_hw_set(sc->hw, SINGLE_SCAN_ON_ALL_BANDS);
	ieee80211_hw_set(sc->hw, REPORTS_TX_ACK_STATUS);
	ieee80211_hw_set(sc->hw, MFP_CAPABLE);
	ieee80211_hw_set(sc->hw, AMPDU_AGGREGATION);
	ieee80211_hw_set(sc->hw, CONNECTION_MONITOR);

	sc->hw->extra_tx_headroom = sizeof(struct sv6160_cmd_tx_ctrl);
	sc->hw->queues            = 4;

	SET_IEEE80211_DEV(sc->hw, sc->dev);

	eth_random_addr(mac);
	SET_IEEE80211_PERM_ADDR(sc->hw, mac);

	sv6160_setup_band(sc);
	sv6160_setup_ht_cap(sc);

	ret = ieee80211_register_hw(sc->hw);
	if (ret) {
		dev_err(sc->dev, "ieee80211_register_hw failed: %d\n", ret);
	} else {
		dev_info(sc->dev, "Registered as mac80211 device\n");
		sv6160_debugfs_init(sc);
	}

	return ret;
}

void sv6160_core_exit(struct sv6160_softc *sc)
{
	int i;

	WRITE_ONCE(sc->is_firmware_loaded, false);
	sc->is_started = false;
	sc->is_rpu_ready = false;
	sc->beacon_enabled = false;
	sc->scan_in_progress = false;
	debugfs_remove_recursive(sc->debugfs_dir);
	del_timer_sync(&sc->bcn_timer);
	cancel_delayed_work_sync(&sc->roc_timeout);
	cancel_delayed_work_sync(&sc->idle_work);
	cancel_delayed_work_sync(&sc->rx_poll);
	cancel_delayed_work_sync(&sc->scan_timeout);
	cancel_delayed_work_sync(&sc->scan_poll);
	cancel_delayed_work_sync(&sc->scan_work);
	cancel_delayed_work_sync(&sc->tx_rx_poll);
	cancel_work_sync(&sc->tx_work);
	cancel_work_sync(&sc->rx_work);
	ieee80211_unregister_hw(sc->hw);
	skb_queue_purge(&sc->tx_queue);
	skb_queue_purge(&sc->rx_queue);
	for (i = 0; i < SV6160_TX_PENDING_MAX; i++) {
		if (sc->tx_pending[i]) {
			ieee80211_free_txskb(sc->hw, sc->tx_pending[i]);
			sc->tx_pending[i] = NULL;
		}
	}
}

MODULE_DESCRIPTION("Seekwave SV6160 / Rockchip RK915 WiFi Driver");
