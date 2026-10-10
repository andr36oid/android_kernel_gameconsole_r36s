// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware watchdog for the way into and out of system sleep.
 *
 * A driver that stalls the bus while suspending or resuming (dwc2 on a core
 * that never came out of reset, a wedged SD card) freezes the CPU itself, so
 * the kernel's own DPM watchdog never fires and only the reset hole helps.
 * This arms the SoC watchdog (snps,dw-wdt) from the start of the device
 * suspend phases to the end of the device resume phases, and kicks it at
 * every phase. If one phase hangs for longer than the timeout (~21 s at
 * 100 MHz), the SoC resets and the console boots normally.
 *
 * It is stopped in the noirq phase, before the firmware puts the SoC to
 * sleep, so it never runs while the console is asleep, however long that is.
 * It does not use the watchdog framework: there is no /dev/watchdog and no
 * restart handler, so reboot works as before. The DT node stays disabled
 * for dw_wdt; this driver finds it by its compatible string.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reset.h>

#define WDT_CR		0x00
#define WDT_CR_EN	BIT(0)
#define WDT_TORR	0x04
#define WDT_CRR		0x0c
#define WDT_CRR_KICK	0x76
#define WDT_MAX_TOP	15

struct sleep_wdt {
	void __iomem *regs;
	struct clk *clk;
	struct reset_control *rst;
	unsigned long rate;
	u32 top;
	bool armed;
};

static struct sleep_wdt sw;

static unsigned int top_seconds(u32 top)
{
	return (1U << (16 + top)) / sw.rate;
}

static void sleep_wdt_kick(void)
{
	if (sw.armed)
		writel(WDT_CRR_KICK, sw.regs + WDT_CRR);
}

static void sleep_wdt_arm(void)
{
	if (sw.armed)
		return;
	writel(sw.top | sw.top << 4, sw.regs + WDT_TORR);
	writel(WDT_CRR_KICK, sw.regs + WDT_CRR);
	/* response mode 0: reset the SoC on the first timeout */
	writel(WDT_CR_EN, sw.regs + WDT_CR);
	sw.armed = true;
}

/* once enabled, the dw-wdt can only be stopped by a reset */
static void sleep_wdt_disarm(void)
{
	if (!sw.armed)
		return;
	reset_control_assert(sw.rst);
	udelay(1);
	reset_control_deassert(sw.rst);
	sw.armed = false;
}

static int sleep_wdt_prepare(struct device *dev)
{
	sleep_wdt_arm();
	return 0;
}

static int sleep_wdt_kick_cb(struct device *dev)
{
	sleep_wdt_kick();
	return 0;
}

static int sleep_wdt_suspend_noirq(struct device *dev)
{
	sleep_wdt_disarm();
	return 0;
}

static int sleep_wdt_resume_noirq(struct device *dev)
{
	sleep_wdt_arm();
	return 0;
}

static void sleep_wdt_complete(struct device *dev)
{
	sleep_wdt_disarm();
}

static const struct dev_pm_ops sleep_wdt_pm_ops = {
	.prepare	= sleep_wdt_prepare,
	.suspend	= sleep_wdt_kick_cb,
	.suspend_late	= sleep_wdt_kick_cb,
	.suspend_noirq	= sleep_wdt_suspend_noirq,
	.resume_noirq	= sleep_wdt_resume_noirq,
	.resume_early	= sleep_wdt_kick_cb,
	.resume		= sleep_wdt_kick_cb,
	.complete	= sleep_wdt_complete,
};

static int sleep_wdt_probe(struct platform_device *pdev)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "snps,dw-wdt");
	if (!np)
		return -ENODEV;

	sw.regs = of_iomap(np, 0);
	sw.clk = of_clk_get(np, 0);
	sw.rst = of_reset_control_get_exclusive(np, NULL);
	of_node_put(np);
	if (!sw.regs)
		return -ENOMEM;
	if (IS_ERR(sw.clk)) {
		ret = PTR_ERR(sw.clk);
		goto err_unmap;
	}
	/* without the reset it could not be stopped before sleep */
	if (IS_ERR_OR_NULL(sw.rst)) {
		ret = sw.rst ? PTR_ERR(sw.rst) : -ENODEV;
		goto err_clk;
	}

	ret = clk_prepare_enable(sw.clk);
	if (ret)
		goto err_rst;
	sw.rate = clk_get_rate(sw.clk);
	if (!sw.rate) {
		ret = -EINVAL;
		goto err_disable;
	}
	/* the longest timeout the hardware has */
	sw.top = WDT_MAX_TOP;
	reset_control_deassert(sw.rst);

	dev_info(&pdev->dev, "armed during suspend and resume, %u s per phase\n",
		 top_seconds(sw.top));
	return 0;

err_disable:
	clk_disable_unprepare(sw.clk);
err_rst:
	reset_control_put(sw.rst);
err_clk:
	clk_put(sw.clk);
err_unmap:
	iounmap(sw.regs);
	return ret;
}

static struct platform_driver sleep_wdt_driver = {
	.probe	= sleep_wdt_probe,
	.driver	= {
		.name	= "andr36oid-sleep-wdt",
		.pm	= &sleep_wdt_pm_ops,
		.suppress_bind_attrs = true,
	},
};

static int __init sleep_wdt_init(void)
{
	struct platform_device *pdev;
	int ret;

	ret = platform_driver_register(&sleep_wdt_driver);
	if (ret)
		return ret;
	/*
	 * Registered after the DT devices, so its suspend callback runs
	 * before theirs and its resume callback after theirs; the watchdog
	 * is armed in .prepare, before any device suspends.
	 */
	pdev = platform_device_register_simple("andr36oid-sleep-wdt", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&sleep_wdt_driver);
		return PTR_ERR(pdev);
	}
	return 0;
}
late_initcall(sleep_wdt_init);

MODULE_DESCRIPTION("andr36oid: hardware watchdog during suspend and resume");
MODULE_LICENSE("GPL");
