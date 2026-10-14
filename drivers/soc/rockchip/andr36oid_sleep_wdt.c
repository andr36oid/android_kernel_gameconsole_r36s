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
 * It is stopped in the last step before the firmware puts the SoC to sleep
 * (syscore, after the other cores are off) and started again in the first
 * step after it, so it never runs while the console is asleep, however long
 * that is, and the only part it doesn't watch is the firmware itself. A
 * reset from it keeps the RAM, so ramoops shows where the kernel hung.
 * On the first timeout it raises an interrupt on another core first: that
 * reads every core's program counter through the CPU debug registers (they
 * answer even when a core is stuck on the bus), prints them and panics, so
 * the crash log names the function each core hung in. The second timeout
 * resets the SoC as before.
 * It does not use the watchdog framework: there is no /dev/watchdog and no
 * restart handler, so reboot works as before. The DT node stays disabled
 * for dw_wdt; this driver finds it by its compatible string.
 */

#include <linux/clk.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reset.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#define WDT_CR		0x00
#define WDT_CR_EN	BIT(0)
#define WDT_CR_RMOD	BIT(1)
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
	int irq;
};

/* RK3326/PX30 Cortex-A35 external debug, one 8 KiB block per core */
#define CPU_DEBUG_BASE	0xff690000
#define CPU_DEBUG_STEP	0x2000
#define CPU_DEBUG_NR	4
#define EDPCSR_LO	0x0a0
#define EDPCSR_HI	0x0ac
#define EDPRSR		0x314
#define EDPRSR_PU	BIT(0)
#define EDLAR		0xfb0
#define EDLAR_UNLOCK	0xc5acce55

static void __iomem *cpu_debug[CPU_DEBUG_NR];

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
	/*
	 * with the interrupt: interrupt on the first timeout, reset on the
	 * second; without it, reset on the first
	 */
	writel(WDT_CR_EN | (sw.irq > 0 ? WDT_CR_RMOD : 0), sw.regs + WDT_CR);
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

/* a few samples of each core's program counter, without stopping it */
static void sleep_wdt_dump_pcs(void)
{
	int cpu, i;

	for (cpu = 0; cpu < CPU_DEBUG_NR; cpu++) {
		void __iomem *base = cpu_debug[cpu];
		unsigned long pc, last = 0;

		if (!base)
			continue;
		if (!(readl(base + EDPRSR) & EDPRSR_PU)) {
			pr_emerg("sleep watchdog: CPU%d is powered down\n", cpu);
			continue;
		}
		writel(EDLAR_UNLOCK, base + EDLAR);
		pr_emerg("sleep watchdog: CPU%d (online %d):\n", cpu, cpu_online(cpu));
		for (i = 0; i < 64; i++) {
			pc = (unsigned long)readl(base + EDPCSR_LO) |
			     (unsigned long)readl(base + EDPCSR_HI) << 32;
			if (pc == last)
				continue;
			pr_emerg("  PC %px %pS\n", (void *)pc, (void *)pc);
			last = pc;
		}
	}
}

static irqreturn_t sleep_wdt_irq(int irq, void *data)
{
	pr_emerg("sleep watchdog: a sleep step hung for %u s, on CPU%d now\n",
		 top_seconds(sw.top), smp_processor_id());
	sleep_wdt_dump_pcs();
	/* the panic saves the log in ramoops; the next timeout resets */
	panic("sleep step hung");
	return IRQ_HANDLED;
}

static void sleep_wdt_setup_irq(struct device *dev, struct device_node *np)
{
	int cpu, irq, ret;

	if (!of_machine_is_compatible("rockchip,rk3326") &&
	    !of_machine_is_compatible("rockchip,px30"))
		return;
	irq = of_irq_get(np, 0);
	if (irq <= 0)
		return;
	for (cpu = 0; cpu < CPU_DEBUG_NR; cpu++)
		cpu_debug[cpu] = ioremap(CPU_DEBUG_BASE + cpu * CPU_DEBUG_STEP,
					 0x1000);
	ret = request_irq(irq, sleep_wdt_irq, IRQF_NO_SUSPEND | IRQF_NO_THREAD,
			  "andr36oid-sleep-wdt", NULL);
	if (ret) {
		dev_warn(dev, "no interrupt (%d), resets without a dump\n", ret);
		return;
	}
	/* not core 0, which is the one most likely to be stuck */
	irq_set_affinity(irq, cpumask_of(cpumask_last(cpu_online_mask)));
	sw.irq = irq;
}

static int sleep_wdt_prepare(struct device *dev)
{
	/* again each time: taking a core offline moves it to core 0 */
	if (sw.irq > 0)
		irq_set_affinity(sw.irq,
				 cpumask_of(cpumask_last(cpu_online_mask)));
	sleep_wdt_arm();
	return 0;
}

static int sleep_wdt_kick_cb(struct device *dev)
{
	sleep_wdt_kick();
	return 0;
}

/*
 * Light sleep (s2idle) never reaches syscore and can last any time, so there
 * it stops with the last device step and starts again with the first one.
 */
static int sleep_wdt_suspend_noirq(struct device *dev)
{
	if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
		sleep_wdt_disarm();
	else
		sleep_wdt_kick();
	return 0;
}

static int sleep_wdt_resume_noirq(struct device *dev)
{
	sleep_wdt_arm();
	sleep_wdt_kick();
	return 0;
}

/* the last step before the firmware, and the first one after it */
static int sleep_wdt_syscore_suspend(void)
{
	sleep_wdt_disarm();
	return 0;
}

static void sleep_wdt_syscore_resume(void)
{
	sleep_wdt_arm();
}

static struct syscore_ops sleep_wdt_syscore_ops = {
	.suspend	= sleep_wdt_syscore_suspend,
	.resume		= sleep_wdt_syscore_resume,
};

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
	sleep_wdt_setup_irq(&pdev->dev, np);
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

	register_syscore_ops(&sleep_wdt_syscore_ops);
	dev_info(&pdev->dev, "armed during suspend and resume, %u s per phase%s\n",
		 top_seconds(sw.top), sw.irq > 0 ? ", core PCs dumped before the reset" : "");
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
	 * Registered before the DT devices (the PM core walks devices in
	 * registration order): its .prepare runs before every other device's
	 * and its .complete after, and its kicks come after theirs on the way
	 * down and before theirs on the way up.
	 */
	pdev = platform_device_register_simple("andr36oid-sleep-wdt", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&sleep_wdt_driver);
		return PTR_ERR(pdev);
	}
	return 0;
}
arch_initcall(sleep_wdt_init);

MODULE_DESCRIPTION("andr36oid: hardware watchdog during suspend and resume");
MODULE_LICENSE("GPL");
