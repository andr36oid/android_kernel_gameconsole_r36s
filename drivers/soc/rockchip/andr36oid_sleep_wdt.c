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
 * It also takes the other cores down at the start of
 * suspend (early_cpu_off, see sleep_cpus_pm_notify) and watches that too,
 * and watches a shutdown once init says one started (shutdown_watch).
 * It does not use the watchdog framework: there is no /dev/watchdog and no
 * restart handler, so reboot works as before. The DT node stays disabled
 * for dw_wdt; this driver finds it by its compatible string.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/hrtimer.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reset.h>
#include <linux/freezer.h>
#include <linux/sched/clock.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/workqueue.h>

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
/* the watchdog probed fine: it can guard the early core steps too */
static bool sw_ready;

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
		/* an offline core's debug registers abort (its power is off) */
		if (!cpu_online(cpu)) {
			pr_emerg("sleep watchdog: CPU%d is offline\n", cpu);
			continue;
		}
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

/*
 * For hangs with interrupts off on every core, where the watchdog's interrupt
 * never runs (a core that doesn't answer while stop_machine takes a core down
 * for deep sleep): the waiting core calls this, the debug registers still work.
 */
void andr36oid_stuck_dump(void)
{
	sleep_wdt_dump_pcs();
}

/* blocked tasks that aren't frozen: the frozen ones are every app, and say nothing */
static void sleep_wdt_dump_blocked(void)
{
	struct task_struct *g, *t;

	pr_emerg("sleep watchdog: blocked tasks:\n");
	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (t->state == TASK_UNINTERRUPTIBLE && !frozen(t))
			sched_show_task(t);
	}
	rcu_read_unlock();
}

/*
 * The rk_timer wakes the cores out of their idle states (those stop each core's own
 * timer). Freezes so far looked like no timer firing on any core for minutes, so
 * its clocks and registers go into the dump, plus every core's timers.
 */
static void __iomem *rk_timer;
static struct clk *rk_timer_pclk, *rk_timer_clk;

static void sleep_wdt_find_rk_timer(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL,
							 "rockchip,rk3288-timer");

	if (!np)
		return;
	rk_timer = of_iomap(np, 0);
	rk_timer_pclk = of_clk_get_by_name(np, "pclk");
	rk_timer_clk = of_clk_get_by_name(np, "timer");
	of_node_put(np);
	if (IS_ERR(rk_timer_pclk))
		rk_timer_pclk = NULL;
	if (IS_ERR(rk_timer_clk))
		rk_timer_clk = NULL;
}

static void sleep_wdt_dump_rk_timer(void)
{
	bool pclk_on = rk_timer_pclk && __clk_is_enabled(rk_timer_pclk);

	pr_emerg("sleep watchdog: rk_timer pclk %d, timer clk %d\n", pclk_on,
		 rk_timer_clk ? __clk_is_enabled(rk_timer_clk) : -1);
	/* registers behind a gated pclk would hang the bus */
	if (!rk_timer || !pclk_on)
		return;
	pr_emerg("sleep watchdog: rk_timer load %08x current %08x control %08x int %08x\n",
		 readl(rk_timer + 0x00), readl(rk_timer + 0x08),
		 readl(rk_timer + 0x10), readl(rk_timer + 0x18));
}

/* armed for a shutdown (andr36oid_sleep_wdt.shutdown_watch), not a sleep */
static bool sw_shutdown;

/*
 * Awake watch (andr36oid_sleep_wdt.awake_watch=1, off by default): while the console
 * runs, a timer kicks the watchdog every AWAKE_KICK_S. A console whose timers have
 * stopped firing (it then only moves on when a button is pressed) is reset with a
 * dump instead of crawling on. The sleep steps arm, kick and stop it as before; the
 * worker is freezable, so it stays out of the way from the freezing of tasks on.
 */
#define AWAKE_KICK_S	5
static bool awake_watch;
module_param(awake_watch, bool, 0644);
MODULE_PARM_DESC(awake_watch, "Reset with a dump if timers stop firing while awake");
static u64 awake_kick_ns;

static void awake_kick_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(awake_kick_work, awake_kick_fn);

static void awake_kick_fn(struct work_struct *work)
{
	if (sw_shutdown || system_state != SYSTEM_RUNNING)
		return;
	if (awake_watch) {
		sleep_wdt_arm();
		sleep_wdt_kick();
		awake_kick_ns = local_clock();
	} else if (awake_kick_ns) {
		sleep_wdt_disarm();
		awake_kick_ns = 0;
	}
	queue_delayed_work(system_freezable_wq, &awake_kick_work,
			   AWAKE_KICK_S * HZ);
}

static irqreturn_t sleep_wdt_irq(int irq, void *data)
{
	if (awake_kick_ns)
		pr_emerg("sleep watchdog: last awake kick %llu ms ago\n",
			 div_u64(local_clock() - awake_kick_ns, NSEC_PER_MSEC));
	if (sw_shutdown)
		pr_emerg("sleep watchdog: the shutdown didn't finish, on CPU%d now\n",
			 smp_processor_id());
	else
		pr_emerg("sleep watchdog: a sleep step hung for %u s, on CPU%d now\n",
			 top_seconds(sw.top), smp_processor_id());
	sleep_wdt_dump_pcs();
	sleep_wdt_dump_rk_timer();
	/*
	 * The sleep task is usually not stuck on a core but waiting for something:
	 * the stacks of the blocked tasks show what, the workqueues who is still busy.
	 */
	sleep_wdt_dump_blocked();
	show_workqueue_state();
	/* last, it's long: each core's tick device, the broadcast one, pending hrtimers */
	sysrq_timer_list_show();
	/* the panic saves the log in ramoops; the next timeout resets */
	if (sw_shutdown)
		panic("shutdown hung");
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
	/* the awake watch may have armed it already */
	sleep_wdt_kick();
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

/* a restart resets the SoC anyway; stopped so nothing can bite after it */
static void sleep_wdt_syscore_shutdown(void)
{
	if (system_state == SYSTEM_RESTART)
		sleep_wdt_disarm();
}

static struct syscore_ops sleep_wdt_syscore_ops = {
	.suspend	= sleep_wdt_syscore_suspend,
	.resume		= sleep_wdt_syscore_resume,
	.shutdown	= sleep_wdt_syscore_shutdown,
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

	sleep_wdt_find_rk_timer();
	register_syscore_ops(&sleep_wdt_syscore_ops);
	sw_ready = true;
	queue_delayed_work(system_freezable_wq, &awake_kick_work, AWAKE_KICK_S * HZ);
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

/*
 * Deep sleep hangs taking the other cores down at the very end of suspend:
 * after every device's noirq suspend, a core's teardown waits for an RCU
 * grace period that never ends (dumps from a genuine R36S and a G80C, with
 * a core still in f2fs writeback by then). Here they go down at the start
 * instead, while every device and interrupt still works, and come back after
 * the resume, so the late step has nothing left to do.
 * Off by default (andr36oid_sleep_wdt.early_cpu_off=1 turns it on): on a
 * genuine R36S it still hung, and the console then ran with timers that
 * hardly fired.
 */
static bool early_cpu_off;
module_param(early_cpu_off, bool, 0644);
MODULE_PARM_DESC(early_cpu_off, "take the other cores down before suspend, not at its end");

static struct cpumask early_off_cpus;
static void sleep_cpus_online_fn(struct work_struct *work);
static DECLARE_WORK(sleep_cpus_online_work, sleep_cpus_online_fn);

/*
 * The cores' steps happen before the devices' .prepare arms the watchdog and
 * after their .complete stops it: watched here too, its interrupt on the core
 * that stays up.
 */
static void sleep_cpus_watch(bool on)
{
	if (!sw_ready)
		return;
	if (on) {
		if (sw.irq > 0)
			irq_set_affinity(sw.irq, cpumask_of(cpumask_first(cpu_online_mask)));
		sleep_wdt_arm();
		sleep_wdt_kick();
	} else {
		sleep_wdt_disarm();
	}
}

static int sleep_cpus_pm_notify(struct notifier_block *nb, unsigned long action,
				void *data)
{
	struct device *dev;
	int cpu, ret;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		if (!early_cpu_off)
			break;
		sleep_cpus_watch(true);
		lock_device_hotplug();
		for_each_online_cpu(cpu) {
			if (cpu == cpumask_first(cpu_online_mask))
				continue;
			dev = get_cpu_device(cpu);
			ret = dev ? device_offline(dev) : -ENODEV;
			if (ret == 0)
				cpumask_set_cpu(cpu, &early_off_cpus);
			else if (ret < 0)
				pr_warn("sleep: CPU%d didn't go down before suspend: %d\n",
					cpu, ret);
			sleep_wdt_kick();
		}
		unlock_device_hotplug();
		/* stays armed: the devices' .prepare goes on from here */
		break;
	case PM_POST_SUSPEND:
		if (cpumask_empty(&early_off_cpus)) {
			/* a suspend that failed before the devices' .complete */
			sleep_cpus_watch(false);
			break;
		}
		/*
		 * Core hotplug is still off here (cpu_hotplug_pm_callback comes
		 * after this one): the cores come back from a worker
		 */
		sleep_cpus_watch(false);
		schedule_work(&sleep_cpus_online_work);
		break;
	}
	return NOTIFY_OK;
}

static void sleep_cpus_online_fn(struct work_struct *work)
{
	struct device *dev;
	int cpu, ret, tries;

	/* no next suspend before the cores are back */
	lock_system_sleep();
	sleep_cpus_watch(true);
	lock_device_hotplug();
	for_each_cpu(cpu, &early_off_cpus) {
		dev = get_cpu_device(cpu);
		ret = -ENODEV;
		/* -EBUSY until cpu_hotplug_pm_callback has turned hotplug back on */
		for (tries = 0; dev && tries < 100; tries++) {
			ret = device_online(dev);
			if (ret != -EBUSY)
				break;
			unlock_device_hotplug();
			msleep(10);
			lock_device_hotplug();
		}
		if (ret < 0)
			pr_warn("sleep: CPU%d didn't come back after resume: %d\n",
				cpu, ret);
		sleep_wdt_kick();
	}
	cpumask_clear(&early_off_cpus);
	unlock_device_hotplug();
	sleep_cpus_watch(false);
	unlock_system_sleep();
}

/*
 * Before cpu_hotplug_pm_callback (priority 0), which turns core hotplug off
 * for the whole suspend
 */
static struct notifier_block sleep_cpus_pm_nb = {
	.notifier_call = sleep_cpus_pm_notify,
	.priority = 1,
};

/*
 * Shutdown: init's "on shutdown" sets andr36oid_sleep_wdt.shutdown_watch. A
 * shutdown that hung left a console on with a black screen until the battery
 * was flat; now the watchdog is kicked for SHUTDOWN_WATCH_S, then left to run
 * out: its interrupt dumps the cores and panics (ramoops keeps it), the next
 * timeout resets. A restart stops it at the end (syscore .shutdown).
 */
#define SHUTDOWN_WATCH_S	60
#define SHUTDOWN_KICK_S		10
static unsigned int shutdown_kicks;

static void shutdown_kick_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(shutdown_kick_work, shutdown_kick_fn);

static void shutdown_kick_fn(struct work_struct *work)
{
	if (!sw.armed || !sw_shutdown)
		return;
	sleep_wdt_kick();
	if (++shutdown_kicks < SHUTDOWN_WATCH_S / SHUTDOWN_KICK_S)
		schedule_delayed_work(&shutdown_kick_work, SHUTDOWN_KICK_S * HZ);
}

static int shutdown_watch_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (!on || !sw_ready || sw_shutdown)
		return 0;
	sw_shutdown = true;
	if (sw.irq > 0)
		irq_set_affinity(sw.irq, cpumask_of(cpumask_first(cpu_online_mask)));
	sleep_wdt_arm();
	sleep_wdt_kick();
	pr_info("sleep watchdog: watching the shutdown\n");
	schedule_delayed_work(&shutdown_kick_work, SHUTDOWN_KICK_S * HZ);
	return 0;
}

static const struct kernel_param_ops shutdown_watch_ops = {
	.set = shutdown_watch_set,
	.get = param_get_bool,
};
module_param_cb(shutdown_watch, &shutdown_watch_ops, &sw_shutdown, 0200);
MODULE_PARM_DESC(shutdown_watch, "1: watch the shutdown that has just started");

static int __init sleep_wdt_init(void)
{
	struct platform_device *pdev;
	int ret;

	register_pm_notifier(&sleep_cpus_pm_nb);

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
