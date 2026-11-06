// SPDX-License-Identifier: GPL-2.0
/*
 * Pulse-count LED driver for the unmarked "keros" effect-LED IC found on
 * R36 Ultra style handhelds. The chip is not an I2C device despite vendor
 * device-tree templates placing it on an I2C bus: it is driven entirely by
 * a pulse-count sequence on a single control GPIO, gated by a power-enable
 * GPIO.
 *
 * Protocol (documented hardware facts):
 *   - enable GPIO high powers the chip; it must be high before pulsing.
 *   - control GPIO carries a pulse frame that selects one of a fixed set of
 *     built-in effects by pulse count N:
 *         drive control low, settle 1 ms
 *         control high 6 ms            (sync)
 *         control low  1 ms            (drop)
 *         repeat N times: high 1 ms, low 1 ms   (N data pulses -> mode N)
 *         low 5 ms                     (end)
 *     The sync pulse and the 1 ms data pulses are timing critical, so the
 *     frame is emitted with local interrupts disabled.
 *   - N = 1..9 select built-in effects, N = 11 is the chip init/off command,
 *     N = 0 is treated here as "off": the enable GPIO is dropped so the chip
 *     is unpowered and guaranteed dark with zero draw.
 *   - After power-on the chip ignores effect modes until it has received one
 *     init frame (N = 11). Every power-up therefore emits mode 11 first; a
 *     cold effect frame alone is silently dropped and the rings stay dark.
 */

#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/irqflags.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/workqueue.h>

/* Pulse frame timings, milliseconds. */
#define KEROS_SETTLE_MS		1
#define KEROS_SYNC_MS		6
#define KEROS_DROP_MS		1
#define KEROS_DATA_MS		1
#define KEROS_END_MS		5
/* Time for the chip to come up after the enable GPIO is asserted. */
#define KEROS_POWERUP_MS	100

/* Highest user-selectable effect mode (chip presets 1..9). */
#define KEROS_MAX_MODE		9
/* Chip init/off command: required as the first frame after every power-on. */
#define KEROS_MODE_INIT		11

/*
 * sleep_mode: what the rings do when the system idles. Stored in the driver and
 * set from userspace (persist.andr36oid.stickled.sleep via init.rk30board.rc).
 *   SCREEN - turn the rings off when the display blanks, restore on unblank.
 *   SLEEP  - keep the rings on with the screen off; turn off when the SoC
 *            suspends and restore on resume (the historical default).
 *   NEVER  - leave the rings lit through screen-off and suspend (HW permitting).
 */
#define KEROS_SLEEP_SCREEN	0
#define KEROS_SLEEP_SLEEP	1
#define KEROS_SLEEP_NEVER	2

struct keros_led {
	struct led_classdev	cdev;
	struct gpio_desc	*ctrl;
	struct gpio_desc	*enable;
	struct mutex		lock;	/* serialises pulse frames and mode */
	unsigned int		mode;	/* last mode applied; 0 == off/unpowered */
	unsigned int		sleep_mode;	/* KEROS_SLEEP_* */
	bool			powered;
	bool			screen_blanked;	/* last display blank state */
	struct notifier_block	fb_notif;	/* display blank notifications */
	struct work_struct	restore_work;	/* deferred preset re-apply */
};

/*
 * Emit one pulse frame selecting mode N on the control line. The enable GPIO
 * must already be asserted. Must be called with led->lock held.
 */
static void keros_pulse(struct keros_led *led, unsigned int n)
{
	unsigned long flags;
	unsigned int i;

	gpiod_set_value(led->ctrl, 0);
	mdelay(KEROS_SETTLE_MS);

	local_irq_save(flags);

	gpiod_set_value(led->ctrl, 1);
	mdelay(KEROS_SYNC_MS);
	gpiod_set_value(led->ctrl, 0);
	mdelay(KEROS_DROP_MS);

	for (i = 0; i < n; i++) {
		gpiod_set_value(led->ctrl, 1);
		mdelay(KEROS_DATA_MS);
		gpiod_set_value(led->ctrl, 0);
		mdelay(KEROS_DATA_MS);
	}

	local_irq_restore(flags);

	mdelay(KEROS_END_MS);
	gpiod_set_value(led->ctrl, 0);
}

/*
 * Drive the chip dark (drop control and remove power) but KEEP led->mode as the
 * remembered preset. Used for display-blank and suspend, where the rings go off
 * temporarily and must come back to the same preset. Must hold led->lock.
 */
static void keros_power_off_locked(struct keros_led *led)
{
	gpiod_set_value(led->ctrl, 0);
	gpiod_set_value(led->enable, 0);
	led->powered = false;
}

/*
 * Re-apply the remembered preset after a power-off (blank/resume). Because
 * keros_power_off_locked left powered == false, keros_apply_locked re-powers the
 * chip and re-sends the init frame first. If the remembered mode is 0 (user off)
 * this stays dark. Must hold led->lock.
 */
static void keros_apply_locked(struct keros_led *led, unsigned int mode);

static void keros_restore_locked(struct keros_led *led)
{
	unsigned int mode = led->mode;

	led->mode = 0;
	keros_apply_locked(led, mode);
}

static void keros_apply_locked(struct keros_led *led, unsigned int mode)
{
	if (mode == 0) {
		/*
		 * Off means guaranteed dark: drop the control line and remove
		 * power from the chip so it cannot sit in a lit default state.
		 */
		keros_power_off_locked(led);
		led->mode = 0;
		return;
	}

	if (!led->powered) {
		gpiod_set_value(led->enable, 1);
		/*
		 * Sleep rather than busy-wait the power-up settle: all callers
		 * run in process context (brightness_set_blocking and the
		 * restore workqueue), and the settle only needs to be "at least
		 * this long", so msleep frees the CPU for the ~100 ms.
		 */
		msleep(KEROS_POWERUP_MS);
		led->powered = true;
		/*
		 * The chip rejects effect frames until it has seen one init
		 * frame after power-on, so send mode 11 before the first
		 * effect. Without this the enable/pulse looks correct in
		 * software but the rings never light.
		 */
		keros_pulse(led, KEROS_MODE_INIT);
	}

	keros_pulse(led, mode);
	led->mode = mode;
}

/*
 * Deferred preset re-apply. A restore runs the full power-up + init + effect
 * frame, which is ~160 ms of mdelay/pulsing. Display-unblank and resume schedule
 * this instead of running it inline, so the shared fb blank-notifier chain (also
 * used by the touch and hall drivers) and the dpm_resume callback are not stalled
 * by the frame. Runs in process context off the critical path.
 */
static void keros_restore_work(struct work_struct *work)
{
	struct keros_led *led = container_of(work, struct keros_led, restore_work);

	mutex_lock(&led->lock);
	/*
	 * Skip a stale restore: if the display re-blanked after this work was
	 * queued (rapid unblank->blank in "with screen" mode), the rings should
	 * stay dark. SLEEP mode keeps them on with the screen off, so it is not
	 * gated on the blank state.
	 */
	if (!(led->sleep_mode == KEROS_SLEEP_SCREEN && led->screen_blanked))
		keros_restore_locked(led);
	mutex_unlock(&led->lock);
}

static int keros_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct keros_led *led = container_of(cdev, struct keros_led, cdev);

	mutex_lock(&led->lock);
	keros_apply_locked(led, brightness);
	mutex_unlock(&led->lock);

	return 0;
}

/* ---- sleep_mode sysfs attribute ------------------------------------------ */

static ssize_t sleep_mode_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct keros_led *led = container_of(cdev, struct keros_led, cdev);

	return sprintf(buf, "%u\n", led->sleep_mode);
}

static ssize_t sleep_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct keros_led *led = container_of(cdev, struct keros_led, cdev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;
	if (val > KEROS_SLEEP_NEVER)
		return -EINVAL;

	mutex_lock(&led->lock);
	led->sleep_mode = val;
	mutex_unlock(&led->lock);

	return size;
}
static DEVICE_ATTR_RW(sleep_mode);

static struct attribute *keros_attrs[] = {
	&dev_attr_sleep_mode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(keros);

/* ---- display-blank handling (KEROS_SLEEP_SCREEN) -------------------------- */

/*
 * Turn the rings off when the display blanks and back on when it unblanks, but
 * only in the "with screen" sleep mode. The other two modes ignore blank: SLEEP
 * leaves the rings on with the screen off (handled on SoC suspend), NEVER keeps
 * them lit. The stored preset is preserved across the off, so unblank restores it.
 */
static int keros_fb_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct keros_led *led = container_of(nb, struct keros_led, fb_notif);
	struct fb_event *event = data;
	int blank;

	if (action != FB_EVENT_BLANK)
		return NOTIFY_OK;
	blank = *((int *)event->data);

	mutex_lock(&led->lock);
	led->screen_blanked = (blank != FB_BLANK_UNBLANK);
	if (led->sleep_mode == KEROS_SLEEP_SCREEN) {
		if (blank == FB_BLANK_UNBLANK) {
			/*
			 * Restore is the heavy ~160 ms frame; defer it to the
			 * workqueue so the blank-notifier chain (shared with the
			 * touch/hall drivers) and the screen-on path are not
			 * stalled. keros_restore_work re-checks screen_blanked so
			 * a fast re-blank cannot leave the rings lit.
			 */
			mutex_unlock(&led->lock);
			schedule_work(&led->restore_work);
			return NOTIFY_OK;
		}
		/* Blank: power-off is two GPIO writes, cheap enough inline. */
		keros_power_off_locked(led);		/* dark, preset kept */
	}
	mutex_unlock(&led->lock);

	return NOTIFY_OK;
}

static int keros_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct keros_led *led;
	int ret;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	/*
	 * Both lines start low: the chip is left unpowered and the rings dark
	 * at probe. Nothing lights until userspace writes a nonzero mode, so a
	 * user who disabled the lights never sees them come on at boot.
	 */
	led->ctrl = devm_gpiod_get(dev, "ctrl", GPIOD_OUT_LOW);
	if (IS_ERR(led->ctrl))
		return dev_err_probe(dev, PTR_ERR(led->ctrl),
				     "failed to get ctrl GPIO\n");

	led->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(led->enable))
		return dev_err_probe(dev, PTR_ERR(led->enable),
				     "failed to get enable GPIO\n");

	mutex_init(&led->lock);
	INIT_WORK(&led->restore_work, keros_restore_work);
	led->mode = 0;
	led->sleep_mode = KEROS_SLEEP_SLEEP;	/* historical default */
	led->powered = false;

	led->cdev.name = "keros::ambient";
	led->cdev.max_brightness = KEROS_MAX_MODE;
	led->cdev.brightness = 0;
	led->cdev.brightness_set_blocking = keros_set;
	led->cdev.groups = keros_groups;	/* adds the sleep_mode attr */

	ret = devm_led_classdev_register(dev, &led->cdev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, led);

	/*
	 * Watch display blank so the "with screen" sleep mode can follow it.
	 * The fb notifier is delivered on this tree's DRM fbdev path (the same
	 * one the touch/hall drivers hook); not devm, so remove() undoes it.
	 */
	led->fb_notif.notifier_call = keros_fb_notify;
	fb_register_client(&led->fb_notif);

	return 0;
}

static int keros_remove(struct platform_device *pdev)
{
	struct keros_led *led = platform_get_drvdata(pdev);

	fb_unregister_client(&led->fb_notif);
	cancel_work_sync(&led->restore_work);	/* no restore after unregister */

	mutex_lock(&led->lock);
	keros_power_off_locked(led);
	mutex_unlock(&led->lock);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int keros_suspend(struct device *dev)
{
	struct keros_led *led = dev_get_drvdata(dev);

	/* Settle any deferred restore before we decide the suspend state. */
	cancel_work_sync(&led->restore_work);

	mutex_lock(&led->lock);
	/*
	 * NEVER mode keeps the rings lit through suspend, so do not drop power.
	 * Whether they actually stay lit through *deep* sleep depends on the
	 * chip's power rail staying up in the SoC suspend state - a hardware
	 * limitation outside this driver's control. SCREEN and SLEEP both go
	 * dark on suspend (the historical behaviour) and restore on resume.
	 */
	if (led->sleep_mode != KEROS_SLEEP_NEVER)
		keros_power_off_locked(led);
	mutex_unlock(&led->lock);

	return 0;
}

static int keros_resume(struct device *dev)
{
	struct keros_led *led = dev_get_drvdata(dev);

	/*
	 * Only SLEEP mode restores on resume: it is the mode that keeps the
	 * rings lit with the screen off, so they must come back as soon as the
	 * SoC is up. SCREEN mode's restore is owned by the display-unblank
	 * notifier (restoring here would light the rings while the panel is
	 * still blanked, and double-apply when unblank then fires); NEVER was
	 * never powered down. Defer the frame to the workqueue so it does not
	 * stall dpm_resume. A plain read of sleep_mode is fine here.
	 */
	if (led->sleep_mode == KEROS_SLEEP_SLEEP)
		schedule_work(&led->restore_work);

	return 0;
}
#endif

/* 4.19 backport: SIMPLE_DEV_PM_OPS and a direct .pm assignment (no pm_sleep_ptr). */
static SIMPLE_DEV_PM_OPS(keros_pm_ops, keros_suspend, keros_resume);

static const struct of_device_id keros_of_match[] = {
	{ .compatible = "keros,pulse-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, keros_of_match);

static struct platform_driver keros_driver = {
	.probe	= keros_probe,
	.remove	= keros_remove,
	.driver	= {
		.name		= "leds-keros",
		.of_match_table	= keros_of_match,
		.pm		= &keros_pm_ops,
	},
};
module_platform_driver(keros_driver);

MODULE_DESCRIPTION("Keros pulse-count effect-LED driver");
MODULE_LICENSE("GPL v2");
