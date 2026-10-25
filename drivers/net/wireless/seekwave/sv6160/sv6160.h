/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Seekwave SV6160 / Rockchip RK915 WiFi Driver
 * Copyright (c) 2025 Joel Wirāmu Pauling <aenertia@aenertia.net>
 *
 * SDIO mailbox/FIFO protocol - the chip exposes a small register space
 * (0-64 bytes) and a data FIFO at SDIO address 0, NOT direct memory-mapped
 * access to internal chip addresses. See SDIO_ARCHITECTURE.md.
 *
 * RPU command protocol based on stolen/rk915 shared/host_rpu_if.h
 */
#ifndef _SV6160_H_
#define _SV6160_H_

#include <linux/mmc/sdio_func.h>
#include <linux/completion.h>
#include <linux/timer.h>
#include <linux/debugfs.h>
#include <net/mac80211.h>

/* SDIO Device IDs */
#define SDIO_VENDOR_ID_SSV          0x3030
#define SDIO_DEVICE_ID_SV6160       0x3030
/* XF40H variant (MANUFACTURER_ID_EAGLE_BASEX) */
#define SDIO_VENDOR_ID_SSV_ALT      0x0296
#define SDIO_DEVICE_ID_SV6160_ALT   0x5348

/* Firmware file names */
#define SV6160_FW_NAME              "ssv6160-fw.bin"
#define SV6160_PATCH_NAME           "ssv6160-patch.bin"

/*
 * SDIO Mailbox Register Map (byte-wide, accessed via sdio_readb/sdio_writeb)
 */
#define SDIO_REG_WRITE_EN           0
#define SDIO_REG_RECV_LEN_L         2
#define SDIO_REG_RECV_LEN_H         3
#define SDIO_REG_FW_STATE           64
#define SDIO_REG_PKT_INFO           17
#define SDIO_REG_PATCH_LEN_L        18
#define SDIO_REG_PATCH_LEN_H        19
#define SDIO_REG_INT_ADDR           32
#define SDIO_REG_INT_CLR_VAL        2
#define SDIO_REG_INT_STA            35
#define SDIO_REG_NOTIFY_ADDR        17

/* SDIO Bulk Data Addresses (within 17-bit CMD53 range) */
#define SDIO_DATA_ADDR              0x00000
#define SDIO_CMD_ADDR               0x10000

/* Firmware Start Command */
#define SDIO_START_CMD_MAGIC        0x5A5A5A5A

/* Firmware State Machine */
enum sv6160_fw_state {
	FW_STATE_WAIT_FW       = 0,
	FW_STATE_M0_STARTED    = 1,
	FW_STATE_WAIT_PATCH    = 2,
	FW_STATE_WAIT_PATCH2   = 3,
	FW_STATE_LPW_READY     = 4,
	FW_STATE_M0_READY      = 5,
	FW_STATE_M0_SLEEP      = 6,
	FW_STATE_RESET_COMPLETE = 7,
};

/* PM notification values */
#define NOTIFY_SLEEP                2
#define NOTIFY_WAKEUP               3

/* Transfer limits */
#define SV6160_MAX_BLOCK_SIZE       4096
#define SV6160_SDIO_BLOCK_SIZE      512
#define SV6160_FW_POLL_RETRIES      300
#define SV6160_FW_POLL_DELAY_MS     10
#define SV6160_MAX_AGG_SIZE         (8 * 1024)
#define SV6160_NUM_CHANNELS         14
#define SV6160_MAX_SCAN_CHANNELS    50
#define SV6160_RF_PARAMS_SIZE       369
#define SV6160_MAX_RX_SIZE          4096

/* ===================================================================
 * Firmware Packet Dispatch Architecture
 *
 * The firmware dispatcher at fcn.02002630 reads hdr->id (offset 0x10)
 * and routes packets through two tiers:
 *
 * TIER 1 - Fast-path handlers (6 dedicated packet types):
 *   0x00 (0)  → INIT/RESET handler, sub-dispatch on offset 0x1C
 *   0x07 (7)  → KEEPALIVE/NULL handler
 *   0x16 (22) → SCAN handler → fcn.02007a80 (sets scan_pending flag)
 *   0x17 (23) → MGMT frame TX (conditional on internal flag)
 *   0x2A (42) → HOST_CMD → fcn.02009750 (sub-dispatch on offset 0x1C)
 *   0x2C (44) → HOST_DATA → fcn.02002b78 (data frame TX)
 *
 * TIER 2 - General LMAC command processor at fcn.020010b8:
 *   ALL other hdr->id values (including RPU_CMD_* enum values like
 *   VIF_CTRL=15, CHANNEL=23, VIF_CFG=24, TX_POWER=10, PS=13, etc.)
 *   fall through to the general LMAC processor, which handles them
 *   correctly based on hdr->id. This is the path the gamely BSP
 *   (the working vendor driver) uses for configuration commands.
 *
 * Notable overlaps:
 *   - RPU_CMD_ROC_CTRL (22) matches FW_PKT_TYPE_SCAN - the scan
 *     handler's RF setup is appropriate for remain-on-channel.
 *   - RPU_CMD_CHANNEL (23) matches FW_PKT_TYPE_MGMT - but the MGMT
 *     path is only taken when an internal flag is set; otherwise it
 *     falls through to the general LMAC processor.
 *
 * Verified via radare2 disassembly of rk915_patched_linear.bin and
 * cross-referenced against the gamely-code/rk915 BSP (kernel 5.1).
 * =================================================================== */
#define FW_PKT_TYPE_INIT       0x00  /* INIT/CONFIG, sub-dispatch on offset 0x1C */
#define FW_PKT_TYPE_KEEPALIVE  0x07  /* NULL/KEEPALIVE */
#define FW_PKT_TYPE_SCAN       0x16  /* Scan → fcn.02007a80 */
#define FW_PKT_TYPE_MGMT       0x17  /* Management frame TX */
#define FW_PKT_TYPE_HOST_CMD   0x2A  /* HOST_CMD → fcn.02009750 (sub_cmd at 0x1C) */
#define FW_PKT_TYPE_HOST_DATA  0x2C  /* Data frame TX */

/* ===================================================================
 * RPU Command/Event Protocol (from stolen/rk915 shared/host_rpu_if.h)
 * =================================================================== */

#define HAL_PRIV_DATA_SIZE          8

/* RPU Command IDs */
enum rpu_cmd_tag {
	RPU_CMD_RESET           = 0,
	RPU_CMD_SCAN            = 1,
	RPU_CMD_SCAN_ABORT      = 2,
	RPU_CMD_SETKEY          = 4,
	RPU_CMD_TX              = 7,
	RPU_CMD_TX_POWER        = 10,
	RPU_CMD_PS              = 13,
	RPU_CMD_PS_ECON_CFG     = 14,
	RPU_CMD_VIF_CTRL        = 15,
	RPU_CMD_BA_SESSION_INFO = 18,
	RPU_CMD_ROC_CTRL        = 22,
	RPU_CMD_CHANNEL         = 23,
	RPU_CMD_VIF_CFG         = 24,
	RPU_CMD_TXQ_PARAMS      = 26,
	RPU_CMD_FW_PRIV_CMD     = 42,
};

/* FW_PRIV_CMD sub-command IDs (firmware internal HOST_CMD routing) */
enum fw_priv_subcmd {
	FW_PRIV_STATUS_QUERY    = 1,
	FW_PRIV_SET_PHY         = 3,
	FW_PRIV_SET_KEY         = 5,
	FW_PRIV_SET_BSS         = 6,
	FW_PRIV_SET_KEY_ALT     = 7,
	FW_PRIV_SET_CONFIG      = 8,
	FW_PRIV_GET_CONFIG      = 9,
	FW_PRIV_NOP             = 10,
	FW_PRIV_GET_BSS_INFO    = 11,
	FW_PRIV_CALIBRATE       = 12,
	FW_PRIV_READ_REGS       = 13,
	FW_PRIV_CMD_RESET       = 100,
};

/* RPU Event IDs */
enum rpu_event_tag {
	RPU_EVENT_RX                 = 0,
	RPU_EVENT_TX_DONE            = 1,
	RPU_EVENT_DISCONNECTED       = 2,
	RPU_EVENT_SCAN_COMPLETE      = 5,
	RPU_EVENT_SCAN_ABORT_COMPLETE = 6,
	RPU_EVENT_RESET_COMPLETE     = 8,
	RPU_EVENT_COMMAND_PROC_DONE  = 17,
	RPU_EVENT_CH_PROG_DONE       = 18,
	RPU_EVENT_ROC_STATUS         = 24,
	RPU_EVENT_FW_ERROR           = 25,
	RPU_EVENT_CALIBRATION_DONE   = 28,
	RPU_EVENT_FW_PRIV_CMD_DONE   = 29,
	RPU_EVENT_RX_SERIAS          = 30,
	RPU_EVENT_HOST_DATA_TX_DONE  = 32,
};

/* Aggregated RX magic */
#define RX_SERIAS_MAGIC              0x3412ccff
#define SV6160_MAX_AGG_PACKETS       16

/* RX MIC status values */
#define RX_MIC_SUCCESS        0
#define RX_MIC_FAILURE_TKIP   1
#define RX_MIC_FAILURE_CCMP   2

/* Rate flags */
#define ENABLE_11N_FORMAT     0x08
#define MARK_RATE_AS_MCS      0x80

/* Power save modes */
#define PS_MODE_DISABLED    0
#define PS_MODE_ENABLED     1
#define PS_MODE_DEEP        2

/* VIF control operations */
#define VIF_ADD     1
#define VIF_REMOVE  2

/* VIF modes */
#define VIF_MODE_STA    0
#define VIF_MODE_IBSS   1
#define VIF_MODE_AP     2

/* Reset types */
#define LMAC_ENABLE         0
#define LMAC_DISABLE        1
#define LMAC_DO_CALIB       0x10
#define LMAC_NO_SLEEP       0x20
#define LOAD_FACTORY_CAL    0x40
#define LMAC_MODE_NORMAL    0

/* Channel bandwidth */
#define RPU_CHAN_WIDTH_20_NOHT  0

/* VIF config changed bitmap */
#define VIF_CFG_BSSID_CHANGED       (1 << 9)
#define VIF_CFG_CONNECT_STATE       (1 << 14)
#define VIF_CFG_AID_CHANGED         (1 << 5)
#define VIF_CFG_BASICRATES_CHANGED  (1 << 0)
#define VIF_CFG_SHORTSLOT_CHANGED   (1 << 1)
#define VIF_CFG_BCN_INT_CHANGED     (1 << 11)
#define VIF_CFG_DTIM_CHANGED        (1 << 12)
#define VIF_CFG_OP_CHAN_CHANGED      (1 << 15)

/* Connect state */
#define STA_CONN        0
#define STA_DISCONN     1

/* Scan type */
#define SCAN_PASSIVE    0
#define SCAN_ACTIVE     1

/* ===================================================================
 * RPU Protocol Structures (packed, match firmware ABI)
 * =================================================================== */

struct rpu_msg_hdr {
	u8   hal_data[HAL_PRIV_DATA_SIZE];
	__le32 descriptor_id;
	__le32 payload_length;
	__le32 id;
	__le32 length;
	__le32 more_cmd_data;
} __packed;

/* RX_SERIAS batch header - precedes aggregated RX frames */
struct sv6160_rx_batch_hdr {
	struct rpu_msg_hdr hdr;       /* id = RPU_EVENT_RX_SERIAS */
	u32                pkt_count;
	__le16             pkt_len[SV6160_MAX_AGG_PACKETS];
} __packed;

struct rpu_cmd_reset {
	struct rpu_msg_hdr hdr;
	__le32 type;
	__le32 unused1;
	__le32 unused2;
	u8     rf_params[SV6160_RF_PARAMS_SIZE];
	__le32 unused3;
	u8     bg_scan[48];     /* bgscan_params placeholder */
	u8     num_spatial_streams;
	__le32 system_rev;
	__le32 lmac_mode;
	__le32 antenna_sel;
	__le32 reserv[2];
} __packed;

#define SV6160_MAX_NUM_SSIDS  4
#define SV6160_MAX_SSID_LEN   32

struct rpu_scan_ssid {
	__le32 len;
	u8     ssid[SV6160_MAX_SSID_LEN];
} __packed;

struct rpu_cmd_scan {
	struct rpu_msg_hdr hdr;
	__le32 if_index;
	__le32 type;            /* SCAN_PASSIVE / SCAN_ACTIVE */
	__le32 n_channel;
	__le32 n_ssids;
	u8     channel_list[SV6160_MAX_SCAN_CHANNELS];
	u8     chan_max_power[SV6160_MAX_SCAN_CHANNELS];
	u8     chan_flags[SV6160_MAX_SCAN_CHANNELS];
	struct rpu_scan_ssid ssids[SV6160_MAX_NUM_SSIDS];
	__le32 p2p_probe;
	__le32 extra_ies_len;
	u8     extra_ies[];     /* variable-length IEs */
} __packed;

struct rpu_cmd_channel {
	struct rpu_msg_hdr hdr;
	__le32 channel_bw;
	__le32 primary_ch_number;
	__le32 channel_number1;
	__le32 channel_number2;
	__le32 freq_band;
} __packed;

struct rpu_cmd_vif_ctrl {
	struct rpu_msg_hdr hdr;
	__le32 if_ctrl;         /* VIF_ADD / VIF_REMOVE */
	__le32 if_index;
	__le32 mode;            /* VIF_MODE_STA / VIF_MODE_AP */
	u8     mac_addr[6];
} __packed;

struct rpu_cmd_vif_cfg {
	struct rpu_msg_hdr hdr;
	__le32 changed_bitmap;
	__le32 basic_rate_set;
	__le32 use_short_slot;
	__le32 atim_window;
	__le32 aid;
	__le32 capability;
	__le32 short_retry;
	__le32 long_retry;
	__le32 bcn_mode;
	u8     dtim_period;
	__le32 beacon_interval;
	__le32 if_index;
	u8     vif_addr[6];
	u8     bssid[6];
	u8     smps_info;
	u8     connect_state;
	u8     op_channel;
} __packed;

/*
 * FW_PRIV_CMD payload - sent after RPU_CMD_RESET to enable the firmware's
 * TX-to-host data path.  Without this, the internal TX enable flag at
 * 0x10005474 stays 0 and all CMD53 reads will timeout (-110).
 */
/*
 * FW_PRIV_CMD layout - firmware reads fields at fixed offsets from buffer start:
 *   +0x1C: sub_cmd
 *   +0x20: reserved (24 bytes)
 *   +0x38: config_bitmap
 *   +0x3C: mac_addr (6 bytes)
 *   +0x42: pad (2 bytes)
 *   +0x44: prod_test
 */
struct rpu_fw_priv_cmd {
	struct rpu_msg_hdr hdr;     /* 0x00-0x1B (28 bytes) */
	__le32 sub_cmd;             /* 0x1C */
	u8     reserved[24];       /* 0x20-0x37 - firmware expects gap here */
	__le32 config_bitmap;       /* 0x38 - bit3=sleep, bit4=PS, bit5=rate */
	u8     mac_addr[6];         /* 0x3C */
	u8     pad[2];              /* 0x42 */
	__le32 prod_test;           /* 0x44 */
} __packed;

struct rpu_cmd_tx_power {
	struct rpu_msg_hdr hdr;
	__le32 tx_power;
	__le32 band;            /* 0 = 2.4 GHz */
	__le32 if_index;
} __packed;

struct rpu_cmd_ps {
	struct rpu_msg_hdr hdr;
	__le32 ps_mode;
	__le32 idle_timeout_ms;
	__le32 listen_interval;
} __packed;

struct rpu_cmd_txq_params {
	struct rpu_msg_hdr hdr;
	__le32 if_index;
	__le32 queue;
	__le16 txop;
	__le16 cwmin;
	__le16 cwmax;
	u8     aifs;
	u8     pad;
} __packed;

/* TX completion event (from firmware) */
#define SV6160_MAX_TX_CMDS      16

/*
 * TX control header - RPU_CMD_TX (id=7) from gamely BSP cmd_tx_ctrl.
 * Sent as a 512-byte block to SDIO addr 0, followed by raw 802.11
 * frame data in a separate write.  The firmware's tier-1 handler
 * reads frame metadata from this struct and queues for radio TX.
 *
 * The old sv6160_cmd_tx_ctrl (44 bytes, pkt_type=0x2C) was incorrect -
 * HOST_DATA only generates event 32 (SDIO ack), no air TX.
 */
#define SV6160_TX_CMD_SIZE	512
#define SV6160_MAC_HDR_SIZE	52

struct sv6160_cmd_tx_ctrl {
	struct rpu_msg_hdr hdr;         /* 28 bytes, hdr.id = RPU_CMD_TX */
	u8     if_index;
	u8     queue_num;
	__le32 descriptor_id;
	__le32 num_frames_per_desc;
	__le32 pkt_length[SV6160_MAX_TX_CMDS];
	u8     more_frms;
	__le32 force_tx;
	__le32 tx_flags;
	u8     num_rates;
	u8     rate_protection_type[4];
	u8     rate_preamble_type[4];
	u8     rate_retries[4];
	u8     rate[4];
	u8     rate_flags[4];
	u8     num_spatial_streams[4];
	u8     stbc_enabled;
	u8     bcc_or_ldpc;
	u8     aggregate_mpdu;
	u8     encrypt;
	__le32 config_mac_hdr_len;
	u8     frame_source;
	u8     config_mac_header[SV6160_MAC_HDR_SIZE];
	__le32 frame_ddr_pointer[SV6160_MAX_TX_CMDS];
	u8     per_pkt_crypto_params[SV6160_MAX_TX_CMDS][12];
	u8     _pad[0];   /* struct is zero-padded to SV6160_TX_CMD_SIZE */
} __packed;

/* Key management constants */
#define TOTAL_KEY_LEN         32
#define RX_SEQ_SIZE           6
#define KEY_CTRL_ADD          0
#define KEY_CTRL_DEL          1
#define KEY_TYPE_UCAST        0
#define KEY_TYPE_BCAST        1
#define CIPHER_TYPE_WEP40     0
#define CIPHER_TYPE_WEP104    1
#define CIPHER_TYPE_TKIP      2
#define CIPHER_TYPE_CCMP      3

struct rpu_cmd_setkey {
	struct rpu_msg_hdr hdr;
	__le32 if_index;
	__le32 ctrl;          /* KEY_CTRL_ADD / KEY_CTRL_DEL */
	__le32 key_type;      /* KEY_TYPE_UCAST / KEY_TYPE_BCAST */
	__le32 cipher_type;   /* CIPHER_TYPE_* */
	__le32 key_id;
	__le32 key_len;
	__le32 rsc_len;
	u8     mac_addr[6];
	u8     key[TOTAL_KEY_LEN];
	u8     rsc[RX_SEQ_SIZE];
} __packed;

#define TX_DONE_STAT_SUCCESS    0
#define TX_DONE_STAT_RETRY_LIM  1
#define TX_DONE_STAT_LIFETIME   2
#define TX_DONE_STAT_KEY_ERR    3

struct rpu_event_tx_done {
	struct rpu_msg_hdr hdr;
	u8     pdout_voltage;
	u8     frm_status[SV6160_MAX_TX_CMDS];
	u8     retries_num[SV6160_MAX_TX_CMDS];
	u8     rate[SV6160_MAX_TX_CMDS];
	u8     queue;
	__le32 descriptor_id;
	u8     reserved[12];
} __packed;

/* Block ACK session command */
struct rpu_cmd_ba_session {
	struct rpu_msg_hdr hdr;
	__le32 if_index;
	__le32 op;
	__le32 tid;
	__le32 ssn;
	__le32 policy;
	u8     vif_addr[6];
	u8     peer_addr[6];
} __packed;

/* ROC control */
#define ROC_STOP              0
#define ROC_START             1
#define ROC_TYPE_NORMAL       0
#define ROC_TYPE_OFFCHAN_TX   1

struct rpu_cmd_roc {
	struct rpu_msg_hdr hdr;
	__le32 roc_ctrl;
	__le32 roc_channel;
	__le32 roc_duration;
	__le32 roc_type;
} __packed;

/* RX event payload (from firmware) */
struct rpu_rx_pkt {
	struct rpu_msg_hdr hdr;
	__le32 pkt_length;
	u8     rate_or_mcs;
	u8     rssi;
	u8     rx_pkt_status;
	u8     rate_flags;
	u8     nss;
	u8     num_sts;
	u8     timestamp[8];
	u8     stbc_enabled;
	u8     ldpc_enabled;
	u8     unused;
	u8     channel;
	u8     reserved1[16];
	u8     reserved2[2];
	u8     payload[];       /* 802.11 frame */
} __packed;

/* ===================================================================
 * Packet Descriptor Structs (legacy, kept for compatibility)
 * =================================================================== */

struct sv6160_tx_desc {
	__le16 len;
	u8     c_type;
	u8     f80211;
	u8     q_idx;
	u8     rsvd[59];
} __packed;

#define SV6160_TX_HEADROOM          sizeof(struct sv6160_tx_desc)

/* Start command structure sent to SDIO_CMD_ADDR */
struct sv6160_start_cmd {
	__le32 id;
	__le32 length;
	__le32 addr;
} __packed;

struct sv6160_coalescing {
	bool enabled;
	int  rx_bundle_limit;
};

/* ===================================================================
 * Driver State
 * =================================================================== */

struct sv6160_softc {
	struct sdio_func        *func;
	struct ieee80211_hw     *hw;
	struct device           *dev;
	struct mutex             lock;
	bool                     is_firmware_loaded;
	bool                     removing;
	bool                     sdio_irq_claimed;
	bool                     irq_wake_enabled;
	bool                     is_started;
	bool                     is_rpu_ready;
	struct work_struct       rx_work;
	struct delayed_work      rx_poll;
	struct sk_buff_head      rx_queue;
	struct sv6160_coalescing coalescing;
	struct workqueue_struct *wq;

	/* RPU command state */
	struct completion        reset_done;
	struct completion        chan_prog_done;
	struct completion        cmd_done;
	int                      vif_index;
	u32                      tx_desc_id;

	/* TX deferred work (mac_tx runs in softirq, SDIO needs process ctx) */
	struct sk_buff_head      tx_queue;
	struct work_struct       tx_work;

	/* TX completion tracking */
#define SV6160_TX_PENDING_MAX  64
	struct sk_buff          *tx_pending[SV6160_TX_PENDING_MAX];
	atomic_t                 tx_outstanding;

	/* Power management */
	bool                     is_suspended;
	bool                     ps_enabled;
	struct delayed_work      idle_work;
	unsigned long            last_activity;

	/* DW MMC controller registers (for debug diagnostics) */
	void __iomem            *dwmmc_regs;

	/* GPIO interrupt */
	int                      host_wake_irq;
	bool                     gpio_irq_pending;  /* GPIO fired, data ready */
	bool                     gpio_irq_disabled; /* level IRQ disabled by handler */
	u32                      gpio_irq_count;    /* total GPIO IRQs received */

	/* debugfs */
	struct dentry           *debugfs_dir;

	/* Smart RECV_LEN polling - detect stale vs new data without GPIO */
	u16                      last_recv_len;     /* RECV_LEN from previous poll */
	bool                     recv_len_failed;   /* last CMD53 at this RECV_LEN timed out */

	/* mac80211 state */
	struct ieee80211_vif    *vif;
	struct ieee80211_channel channels[SV6160_NUM_CHANNELS];
	struct ieee80211_rate    rates[12];
	struct ieee80211_supported_band band_2ghz;
	u32                      rx_filter;
	struct ieee80211_scan_request *scan_request;
	bool                     scan_in_progress;
	int                      scan_ch_idx;  /* current channel in sweep */
	struct delayed_work      scan_work;    /* channel-sweep scan worker */
	struct delayed_work      scan_timeout;
	struct delayed_work      scan_poll;    /* trigger_rx poll during RPU scan */

	/* TX-triggered RX polling (auth/assoc response pickup) */
	struct delayed_work      tx_rx_poll;
	unsigned long            last_tx_time;

	/* ROC state */
	bool                     roc_in_progress;
	struct ieee80211_channel *roc_chan;
	struct delayed_work       roc_timeout;

	/* Beacon state (AP/IBSS) */
	struct timer_list        bcn_timer;
	bool                     beacon_enabled;
	u16                      beacon_int;
};

/* SDIO I/O */
int  sv6160_sdio_writeb(struct sv6160_softc *sc, u8 addr, u8 val);
int  sv6160_sdio_readb(struct sv6160_softc *sc, u8 addr, u8 *val);
int  sv6160_sdio_write_data(struct sv6160_softc *sc, u32 addr,
			    void *buf, size_t len);
int  sv6160_sdio_read_data(struct sv6160_softc *sc, u32 addr,
			   void *buf, size_t len);

/* RPU Commands */
int  sv6160_rpu_send_cmd(struct sv6160_softc *sc, void *cmd, size_t len,
			 u32 id);
int  sv6160_rpu_reset(struct sv6160_softc *sc);
int  sv6160_rpu_fw_priv_init(struct sv6160_softc *sc);
int  sv6160_rpu_fw_priv_nop(struct sv6160_softc *sc);
int  sv6160_rpu_phy_thresh(struct sv6160_softc *sc);
int  sv6160_rpu_tx_power(struct sv6160_softc *sc, u32 power_dbm);
int  sv6160_rpu_ps_config(struct sv6160_softc *sc, int mode,
			  u32 idle_timeout, u32 listen_interval);
int  sv6160_rpu_add_vif(struct sv6160_softc *sc, const u8 *mac_addr, int mode);
int  sv6160_rpu_remove_vif(struct sv6160_softc *sc);
int  sv6160_rpu_scan(struct sv6160_softc *sc,
		     struct ieee80211_scan_request *req);
int  sv6160_rpu_scan_abort(struct sv6160_softc *sc);
int  sv6160_rpu_channel(struct sv6160_softc *sc, u32 channel, u32 freq_band);
int  sv6160_rpu_vif_cfg(struct sv6160_softc *sc, const u8 *bssid,
			u32 changed, u8 connect_state, u32 aid, u8 op_channel);
int  sv6160_rpu_ba_session(struct sv6160_softc *sc, u32 op, u32 tid,
			   u32 ssn, const u8 *vif_addr, const u8 *peer_addr);
int  sv6160_rpu_roc(struct sv6160_softc *sc, u32 ctrl, u32 channel,
		    u32 duration, u32 type);

/* RPU Event Processing */
void sv6160_rpu_process_event(struct sv6160_softc *sc, struct sk_buff *skb);
void sv6160_rpu_rx_frame(struct sv6160_softc *sc, struct sk_buff *skb);
void sv6160_rpu_process_rx_batch(struct sv6160_softc *sc, struct sk_buff *skb);

/* Firmware & Core */
int  sv6160_boot_firmware(struct sv6160_softc *sc);
int  sv6160_core_init(struct sv6160_softc *sc);
void sv6160_core_exit(struct sv6160_softc *sc);
int  sv6160_get_sdio_blksize(void);

/* Data Path */
int  sv6160_hci_tx(struct sv6160_softc *sc, struct sk_buff *skb, bool is_cmd);
void sv6160_rx_worker(struct work_struct *work);

/* Queue Control */
void sv6160_stop_queues(struct sv6160_softc *sc);
void sv6160_start_queues(struct sv6160_softc *sc);

/* PM Support */
void sv6160_notify_pm(struct sv6160_softc *sc, int wakeup);

/* mac80211 ops */
extern const struct ieee80211_ops sv6160_ops;

#endif /* _SV6160_H_ */
