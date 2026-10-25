// SPDX-License-Identifier: GPL-2.0
/*
 * Seekwave SV6160 / Rockchip RK915 WiFi Driver - SDIO Interface
 * Copyright (c) 2025 Joel Wirāmu Pauling <aenertia@aenertia.net>
 */
#include <linux/module.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/pm_runtime.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include "sv6160.h"

static const struct sdio_device_id sv6160_id_table[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_SSV, SDIO_DEVICE_ID_SV6160) },
	{ SDIO_DEVICE(SDIO_VENDOR_ID_SSV_ALT, SDIO_DEVICE_ID_SV6160_ALT) },
	{ SDIO_DEVICE(SDIO_VENDOR_ID_SSV_ALT, 0x5347) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, sv6160_id_table);

/* GPIO interrupt handler - level-triggered (DTS: IRQ_TYPE_LEVEL_HIGH).
 * Fires whenever GPIO is HIGH (data available). MUST disable_irq on
 * EVERY call to prevent IRQ storms - level-triggered fires continuously
 * while pin is HIGH. Re-enabled by rx_worker after clear+notify. */
static irqreturn_t sv6160_gpio_irq_handler(int irq, void *dev_id)
{
	struct sv6160_softc *sc = dev_id;

	/* Always disable to prevent level-triggered IRQ storm.
	 * Even before firmware is loaded, GPIO may be HIGH. */
	disable_irq_nosync(irq);

	if (sc && sc->is_firmware_loaded && !READ_ONCE(sc->removing)) {
		sc->gpio_irq_disabled = true;
		sc->gpio_irq_count++;
		sc->gpio_irq_pending = true;
		/* Reset stale guard - a real GPIO edge (level-triggered,
		 * handler disables IRQ to prevent storm) means the firmware
		 * has new data.  Safe because the handler disables the IRQ
		 * on entry (disable_irq_nosync above), so it can't re-fire
		 * until rx_worker explicitly re-enables after processing. */
		sc->last_recv_len = 0;
		schedule_work(&sc->rx_work);
	}

	return IRQ_HANDLED;
}

/* SDIO in-band interrupt handler - called when the SV6160 asserts the
 * SDIO DATA1 interrupt line. This is the standard SDIO interrupt
 * mechanism and may be used instead of (or alongside) GPIO. */
static void sv6160_sdio_irq_handler(struct sdio_func *func)
{
	struct sv6160_softc *sc = sdio_get_drvdata(func);

	if (sc && sc->is_firmware_loaded && !READ_ONCE(sc->removing)) {
		sc->gpio_irq_count++;  /* reuse counter for diagnostics */
		sc->gpio_irq_pending = true;
		/* Do NOT reset last_recv_len - same reasoning as GPIO
		 * handler.  Only rpu_send_cmd resets the stale guard
		 * (after sending a command that expects a response). */
		schedule_work(&sc->rx_work);
	}
}

/*
 * SDIO device initialization - matches vendor rk915 BSP protocol:
 * - Block size 512 (not 128)
 * - CCCR interrupt enable
 * - SDIO in-band interrupt mode (DATA1 line, NOT GPIO)
 * - Function-specific interrupt enable
 */
static int sv6160_sdio_device_init(struct sdio_func *func)
{
	int ret, error;
	u8 val;

	sdio_claim_host(func);

	/* Enable interrupts in CCCR (function 0, register 0x04) */
	sdio_f0_writeb(func, 0x07, 0x04, &error);
	if (error)
		dev_warn(&func->dev, "CCCR IEN write failed: %d\n", error);

	/* Switch chip to GPIO interrupt mode (gamely BSP requirement).
	 * MUST be set BEFORE firmware boot - the firmware reads this
	 * during init to decide whether to signal via GPIO or SDIO DATA1.
	 * Without this, WiFi RX frame events may never reach the host
	 * even though command responses work via SDIO in-band. */
	sdio_f0_writeb(func, 0x80, 0x16, &error);
	if (error)
		dev_warn(&func->dev, "CCCR 0x16 GPIO mode write failed: %d\n",
			 error);
	else
		dev_info(&func->dev, "CCCR 0x16=0x80: GPIO interrupt mode enabled\n");

	/* Set block size (configurable via sdio_blksize param, default 512) */
	{
		int blksize = sv6160_get_sdio_blksize();

		ret = sdio_set_block_size(func, blksize);
		if (ret) {
			dev_err(&func->dev, "Block size %d failed: %d\n",
				blksize, ret);
			sdio_release_host(func);
			return ret;
		}
	}

	/* Enable host interrupt generation */
	sdio_writeb(func, 0x02, 34, &error);
	sdio_writeb(func, 0x02, 33, &error);

	/* Clear any pending interrupt */
	val = sdio_readb(func, 32, &error);
	sdio_writeb(func, val, 32, &error);

	sdio_release_host(func);

	dev_info(&func->dev, "SDIO device init complete (blksize=%d)\n",
		 sv6160_get_sdio_blksize());
	return 0;
}

/* PM Suspend/Resume */

static int __maybe_unused sv6160_pm_suspend(struct device *dev)
{
	struct sdio_func    *func = dev_to_sdio_func(dev);
	struct sv6160_softc *sc   = sdio_get_drvdata(func);
	mmc_pm_flag_t        flags = MMC_PM_KEEP_POWER;

	if (!sc || !sc->is_firmware_loaded)
		return 0;

	sc->is_suspended = true;
	cancel_delayed_work_sync(&sc->idle_work);
	cancel_delayed_work_sync(&sc->rx_poll);
	cancel_work_sync(&sc->rx_work);

	sv6160_stop_queues(sc);
	sv6160_rpu_ps_config(sc, PS_MODE_ENABLED, 200, 3);
	sv6160_notify_pm(sc, 0);  /* sleep */

	if (device_may_wakeup(dev))
		flags |= MMC_PM_WAKE_SDIO_IRQ;

	return sdio_set_host_pm_flags(func, flags);
}

static int __maybe_unused sv6160_pm_resume(struct device *dev)
{
	struct sdio_func    *func = dev_to_sdio_func(dev);
	struct sv6160_softc *sc   = sdio_get_drvdata(func);

	if (!sc || !sc->is_firmware_loaded)
		return 0;

	sc->is_suspended = false;
	sv6160_notify_pm(sc, 1);  /* wakeup */
	sv6160_rpu_ps_config(sc, PS_MODE_DISABLED, 0, 0);
	sv6160_start_queues(sc);

	sc->last_activity = jiffies;
	if (sc->is_started) {
		schedule_work(&sc->rx_work);
		schedule_delayed_work(&sc->rx_poll, msecs_to_jiffies(10));
		schedule_delayed_work(&sc->idle_work, msecs_to_jiffies(5000));
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(sv6160_pm_ops, sv6160_pm_suspend, sv6160_pm_resume);

/*
 * Probe - only bind to SDIO function 1 (function 2 is unused/BT).
 * The vendor rk915 driver only uses function 1 for all WiFi I/O.
 */
static int sv6160_probe(struct sdio_func *func,
			const struct sdio_device_id *id)
{
	struct sv6160_softc *sc;
	struct ieee80211_hw *hw;
	int ret;

	dev_info(&func->dev, "SV6160 (RK915) probe: func=%d vendor=0x%04x device=0x%04x\n",
		 func->num, id->vendor, id->device);

	/* Only use SDIO function 1 - skip function 2 */
	if (func->num != 1) {
		dev_info(&func->dev, "Skipping SDIO function %d (only func 1 used)\n",
			 func->num);
		return -ENODEV;
	}

	sdio_claim_host(func);
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;
	func->enable_timeout = jiffies_to_msecs(HZ);
	if (!func->max_blksize)
		func->max_blksize = 512;
	ret = sdio_enable_func(func);
	if (ret) {
		dev_err(&func->dev, "sdio_enable_func failed: %d\n", ret);
		sdio_release_host(func);
		return ret;
	}

	sdio_release_host(func);

	/* Full SDIO device initialization (block size, interrupts, etc) */
	ret = sv6160_sdio_device_init(func);
	if (ret)
		goto err_disable;

	/* Allocate mac80211 hardware struct */
	hw = ieee80211_alloc_hw(sizeof(struct sv6160_softc), &sv6160_ops);
	if (!hw) {
		ret = -ENOMEM;
		goto err_disable;
	}

	sc = hw->priv;
	sc->func = func;
	sc->hw   = hw;
	sc->dev  = &func->dev;
	mutex_init(&sc->lock);

	/* Map the DW MMC controller CTRL register for FIFO/DMA reset.
	 * The SDIO host controller's parent device has the MMIO resource.
	 * DW MMC CTRL register is at offset 0x000 from the controller base. */
	{
		struct device *host_dev = func->card->host->parent;
		struct resource *res;

		res = NULL;
		if (host_dev->bus == &platform_bus_type &&
		    of_device_is_compatible(host_dev->of_node,
					   "rockchip,px30-dw-mshc"))
			res = platform_get_resource(to_platform_device(host_dev),
					    IORESOURCE_MEM, 0);
		if (res) {
			sc->dwmmc_regs = devm_ioremap(&func->dev, res->start, 0x100);
			if (sc->dwmmc_regs)
				dev_info(&func->dev, "DW MMC regs mapped at %pa\n",
					 &res->start);
			else
				dev_warn(&func->dev, "Failed to map DW MMC regs\n");
		}
	}

	sdio_set_drvdata(func, sc);

	pm_runtime_set_active(&func->dev);
	pm_runtime_enable(&func->dev);

	/* Firmware polling needs no IRQ. Initialize mac80211 and every work
	 * item before the chip can interrupt and schedule RX processing. */
	ret = sv6160_boot_firmware(sc);
	if (ret)
		goto err_free;
	ret = sv6160_core_init(sc);
	if (ret)
		goto err_free;

	/* Register BOTH SDIO in-band IRQ and GPIO level-triggered IRQ.
	 * SDIO in-band may not fire (firmware/host controller issue).
	 * GPIO level-triggered (DTS: IRQ_TYPE_LEVEL_HIGH) should fire
	 * whenever the chip has data, staying HIGH until clear+notify.
	 * Previous IRQF_TRIGGER_RISING was wrong - only fired during boot. */
	sdio_claim_host(func);
	ret = sdio_claim_irq(func, sv6160_sdio_irq_handler);
	sdio_release_host(func);
	if (ret)
		dev_warn(&func->dev, "sdio_claim_irq failed: %d\n", ret);
	else {
		sc->sdio_irq_claimed = true;
		dev_info(&func->dev, "SDIO in-band IRQ registered\n");
	}

	/* Always register GPIO IRQ - level-triggered from DTS */
	{
		struct device_node *np = func->dev.of_node;

		if (np) {
			sc->host_wake_irq = of_irq_get_byname(np, "host-wake");
			if (sc->host_wake_irq <= 0)
				sc->host_wake_irq = of_irq_get(np, 0);
		}

		if (sc->host_wake_irq > 0) {
			ret = devm_request_irq(&func->dev, sc->host_wake_irq,
					       sv6160_gpio_irq_handler,
					       IRQF_NO_SUSPEND,
					       "sv6160", sc);
			if (ret) {
				dev_warn(&func->dev, "GPIO IRQ %d failed: %d\n",
					 sc->host_wake_irq, ret);
				sc->host_wake_irq = 0;
			} else {
				sc->irq_wake_enabled =
					!enable_irq_wake(sc->host_wake_irq);
				dev_info(&func->dev, "GPIO IRQ %d registered (level-triggered)\n",
					 sc->host_wake_irq);
			}
		}
	}

	if (!sc->sdio_irq_claimed && sc->host_wake_irq <= 0) {
		ret = -ENXIO;
		WRITE_ONCE(sc->removing, true);
		sv6160_core_exit(sc);
		goto err_free;
	}
	return 0;

err_free:
	sdio_set_drvdata(func, NULL);
	pm_runtime_disable(&func->dev);
	ieee80211_free_hw(hw);
err_disable:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	return ret;
}

static void sv6160_remove(struct sdio_func *func)
{
	struct sv6160_softc *sc = sdio_get_drvdata(func);

	if (!sc)
		return;

	/* Prevent work from re-enabling a level IRQ during teardown. */
	WRITE_ONCE(sc->removing, true);
	if (sc->host_wake_irq > 0) {
		disable_irq(sc->host_wake_irq);
		if (sc->irq_wake_enabled)
			disable_irq_wake(sc->host_wake_irq);
	}
	sdio_claim_host(func);
	if (sc->sdio_irq_claimed)
		sdio_release_irq(func);
	sdio_release_host(func);

	sv6160_core_exit(sc);
	if (sc->host_wake_irq > 0)
		devm_free_irq(&func->dev, sc->host_wake_irq, sc);
	pm_runtime_disable(&func->dev);
	sdio_set_drvdata(func, NULL);
	ieee80211_free_hw(sc->hw);

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
}

static struct sdio_driver sv6160_driver = {
	.name     = "sv6160",
	.id_table = sv6160_id_table,
	.probe    = sv6160_probe,
	.remove   = sv6160_remove,
	.drv = {
		.pm = &sv6160_pm_ops,
	},
};

module_driver(sv6160_driver, sdio_register_driver, sdio_unregister_driver);

MODULE_LICENSE("GPL");
MODULE_FIRMWARE(SV6160_FW_NAME);
MODULE_FIRMWARE(SV6160_PATCH_NAME);
