// SPDX-License-Identifier: GPL-2.0
/*
 * Status LED of the R36S and its clones, one to three GPIOs per console.
 *
 * Which pin states make which colour comes from the DT, because the wiring
 * differs per board: some colours need a pin floating (input), and the
 * genuine R36S's green LED is always on, so its "off" is green. The colour
 * tables follow Raoni07's R36S LED Control, which found them by trying every
 * state on the consoles.
 *
 * What the LED shows, strongest first:
 *   raw       pin states written by the colour check in Settings
 *   notify    blinking for a notification (the lights HAL)
 *   charging  a colour while a charger is in ("release" leaves the LED to
 *             the hardware, "battery" keeps following the battery rules)
 *   mode      "release" (all pins floating: what the board does without
 *             software), "battery" (colour by charge level) or a colour
 *
 * Battery rules: "30 blue 11 purple 0 red" means blue from 30 %, purple from
 * 11 % and red below. "c1+c2" blinks between two colours once a second.
 * Going up a level needs 2 % more than its threshold, so the colour doesn't
 * flicker at the edge. The rules run here, not in Android, so they also work
 * while charging switched off. Blinking stops in sleep; the LED then holds
 * the first colour, or the notification colour while one is waiting.
 *
 * Files in /sys/devices/platform/andr36oid-status-led/:
 *   colors (r)  names of the colours this LED can show
 *   pins (r)    how many pins it uses
 *   mode, battery, charging, full (rw)   see above; "full" is like
 *               "charging" for a full battery with the charger still in.
 *               "default" goes back to the DT's andr36oid,<name> value
 *   notify (rw) "<colour> <on ms> <off ms>" or "none"
 *   raw (rw)    one state per pin, 0 low, 1 high, 2 floating, or "none"
 *   state (r)   what it shows now and why
 */

#include <linux/ctype.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define MAX_PINS	3
#define MAX_COLORS	8
#define MAX_ZONES	4
#define NAME_LEN	16
#define PIN_LOW		0
#define PIN_HIGH	1
#define PIN_FLOAT	2
#define BLINK_MS	1000
#define HYSTERESIS	2
#define REFRESH_MS	30000

/* a colour, or two to blink between; -1 none. RELEASE: all pins floating */
#define RELEASE		-2
#define FOLLOW		-3	/* charging/full: keep the battery rules */

struct show {
	int a, b;
};

struct level {
	int min;
	struct show show;
};

struct status_led {
	struct device *dev;
	struct mutex lock;
	struct gpio_descs *gpios;
	int npins;
	int ncolors;
	char names[MAX_COLORS][NAME_LEN];
	u8 states[MAX_COLORS][MAX_PINS];

	/* settings */
	struct show mode;	/* RELEASE, a colour, or a = -1 for battery */
	bool battery_mode;
	struct level zones[MAX_ZONES];
	int nzones;
	struct show charging, full;
	int notify, notify_on, notify_off;	/* notify -1: none */
	bool raw;
	u8 raw_states[MAX_PINS];

	/* state */
	int zone;		/* -1 until the first battery reading */
	int capacity, status;
	struct show base;	/* what mode/battery/charger want */
	const char *why;
	int phase;
	bool suspended;
	int shown;		/* colour on the pins, RELEASE, or -1 for raw */

	struct delayed_work blink;
	struct delayed_work refresh;
	struct work_struct update;
	struct notifier_block psy_nb;
};

/* ---------------------------------------------------------------- pins */

static void set_pins(struct status_led *led, const u8 *st)
{
	int i;

	for (i = 0; i < led->npins; i++) {
		struct gpio_desc *d = led->gpios->desc[i];

		if (st[i] == PIN_FLOAT)
			gpiod_direction_input(d);
		else
			gpiod_direction_output_raw(d, st[i] == PIN_HIGH);
	}
}

static void show_color(struct status_led *led, int c)
{
	static const u8 floating[MAX_PINS] = { PIN_FLOAT, PIN_FLOAT, PIN_FLOAT };

	if (c == led->shown)
		return;
	led->shown = c;
	set_pins(led, c >= 0 ? led->states[c] : floating);
}

/* ---------------------------------------------------------------- parsing */

static int find_color(struct status_led *led, const char *name, size_t len)
{
	int i;

	for (i = 0; i < led->ncolors; i++)
		if (strlen(led->names[i]) == len && !strncmp(led->names[i], name, len))
			return i;
	return -EINVAL;
}

/* "red", "red+off", or with allow_special also "release" / "battery" */
static int parse_show(struct status_led *led, const char *s, size_t len,
		      bool allow_special, struct show *out)
{
	const char *plus = strnchr(s, len, '+');

	if (allow_special && len == 7 && !strncmp(s, "release", 7)) {
		out->a = out->b = RELEASE;
		return 0;
	}
	if (allow_special && len == 7 && !strncmp(s, "battery", 7)) {
		out->a = out->b = FOLLOW;
		return 0;
	}
	if (plus) {
		out->a = find_color(led, s, plus - s);
		out->b = find_color(led, plus + 1, len - (plus + 1 - s));
	} else {
		out->a = out->b = find_color(led, s, len);
	}
	return out->a < 0 || out->b < 0 ? -EINVAL : 0;
}

static int next_word(const char **p, const char **w, size_t *len)
{
	const char *s = skip_spaces(*p);
	const char *e = s;

	while (*e && !isspace(*e))
		e++;
	*w = s;
	*len = e - s;
	*p = e;
	return *len;
}

static int parse_zones(struct status_led *led, const char *buf,
		       struct level *zones)
{
	const char *p = buf, *w;
	size_t len;
	int n = 0, last = 101;
	char num[8];

	while (next_word(&p, &w, &len)) {
		int min;

		if (n == MAX_ZONES || len >= sizeof(num))
			return -EINVAL;
		memcpy(num, w, len);
		num[len] = 0;
		if (kstrtoint(num, 10, &min) || min < 0 || min >= last)
			return -EINVAL;
		if (!next_word(&p, &w, &len) ||
		    parse_show(led, w, len, false, &zones[n].show))
			return -EINVAL;
		zones[n].min = min;
		last = min;
		n++;
	}
	/* the last level has to cover 0 % */
	return n && last == 0 ? n : -EINVAL;
}

static int print_show(struct status_led *led, const struct show *s, char *buf)
{
	if (s->a == RELEASE)
		return sprintf(buf, "release");
	if (s->a == FOLLOW)
		return sprintf(buf, "battery");
	if (s->a == s->b)
		return sprintf(buf, "%s", led->names[s->a]);
	return sprintf(buf, "%s+%s", led->names[s->a], led->names[s->b]);
}

/* ---------------------------------------------------------------- logic */

static int read_battery(struct device *dev, void *data)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct status_led *led = data;
	union power_supply_propval v;

	if (!psy || psy->desc->type != POWER_SUPPLY_TYPE_BATTERY)
		return 0;
	if (power_supply_get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &v))
		return 0;
	led->capacity = v.intval;
	if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS, &v))
		led->status = v.intval;
	return 1;
}

static int pick_zone(struct status_led *led)
{
	int target = led->nzones - 1, i;

	for (i = 0; i < led->nzones; i++) {
		if (led->capacity >= led->zones[i].min) {
			target = i;
			break;
		}
	}
	if (led->zone < 0 || led->zone >= led->nzones || target >= led->zone)
		return target;
	/* going up: only as far as the hysteresis allows */
	for (i = target; i < led->zone; i++)
		if (led->capacity >= led->zones[i].min + HYSTERESIS)
			return i;
	return led->zone;
}

static void apply(struct status_led *led);

/* work out base from the settings and the battery; called with the lock */
static void evaluate(struct status_led *led)
{
	bool have_battery;
	bool plugged;

	led->capacity = -1;
	led->status = POWER_SUPPLY_STATUS_UNKNOWN;
	have_battery = class_for_each_device(power_supply_class, NULL, led,
					     read_battery) > 0;
	plugged = led->status == POWER_SUPPLY_STATUS_CHARGING ||
		  led->status == POWER_SUPPLY_STATUS_FULL ||
		  led->status == POWER_SUPPLY_STATUS_NOT_CHARGING;

	if (have_battery && led->battery_mode)
		led->zone = pick_zone(led);

	if (plugged && led->status == POWER_SUPPLY_STATUS_FULL &&
	    led->full.a != FOLLOW) {
		led->base = led->full;
		led->why = "full";
	} else if (plugged && led->charging.a != FOLLOW) {
		led->base = led->charging;
		led->why = "charging";
	} else if (!led->battery_mode) {
		led->base = led->mode;
		led->why = "mode";
	} else if (!have_battery) {
		led->base.a = led->base.b = RELEASE;
		led->why = "no battery";
	} else {
		led->base = led->zones[led->zone].show;
		led->why = "battery";
	}
	apply(led);
}

/* put the right colour on the pins and start or stop blinking */
static void apply(struct status_led *led)
{
	int c, delay = 0;

	if (led->raw) {
		led->shown = -1;
		set_pins(led, led->raw_states);
		return;
	}
	if (led->notify >= 0 && led->suspended) {
		c = led->notify;
	} else if (led->notify >= 0) {
		/* phase 0: the notification colour, 1: what would show otherwise */
		c = led->phase ? led->base.a : led->notify;
		delay = led->phase ? led->notify_off : led->notify_on;
	} else if (led->base.a != led->base.b && !led->suspended) {
		c = led->phase ? led->base.b : led->base.a;
		delay = BLINK_MS;
	} else {
		c = led->base.a;
	}
	show_color(led, c);
	if (delay)
		mod_delayed_work(system_wq, &led->blink, msecs_to_jiffies(delay));
	else
		cancel_delayed_work(&led->blink);
}

static void blink_work(struct work_struct *work)
{
	struct status_led *led = container_of(to_delayed_work(work),
					      struct status_led, blink);

	mutex_lock(&led->lock);
	led->phase ^= 1;
	apply(led);
	mutex_unlock(&led->lock);
}

static void update_work(struct work_struct *work)
{
	struct status_led *led = container_of(work, struct status_led, update);

	mutex_lock(&led->lock);
	if (!led->suspended)
		evaluate(led);
	mutex_unlock(&led->lock);
}

static void refresh_work(struct work_struct *work)
{
	struct status_led *led = container_of(to_delayed_work(work),
					      struct status_led, refresh);

	update_work(&led->update);
	schedule_delayed_work(&led->refresh, msecs_to_jiffies(REFRESH_MS));
}

static int psy_changed(struct notifier_block *nb, unsigned long event,
		       void *data)
{
	struct status_led *led = container_of(nb, struct status_led, psy_nb);

	schedule_work(&led->update);
	return NOTIFY_OK;
}

/* ---------------------------------------------------------------- sysfs */

/* "default": the DT's andr36oid,<name>, or the built-in one without it */
static const char *dt_value(struct status_led *led, const char *name,
			    const char *buf, const char *builtin)
{
	char prop[32];
	const char *s;

	if (strncmp(buf, "default", 7))
		return buf;
	snprintf(prop, sizeof(prop), "andr36oid,%s", name);
	if (!of_property_read_string(led->dev->of_node, prop, &s))
		return s;
	return builtin;
}

static ssize_t colors_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);
	int i, n = 0;

	for (i = 0; i < led->ncolors; i++)
		n += sprintf(buf + n, "%s%s", i ? " " : "", led->names[i]);
	return n + sprintf(buf + n, "\n");
}
static DEVICE_ATTR_RO(colors);

static ssize_t pins_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", led->npins);
}
static DEVICE_ATTR_RO(pins);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);
	int n;

	mutex_lock(&led->lock);
	n = led->battery_mode ? sprintf(buf, "battery")
			      : print_show(led, &led->mode, buf);
	mutex_unlock(&led->lock);
	return n + sprintf(buf + n, "\n");
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct status_led *led = dev_get_drvdata(dev);
	size_t len;
	struct show s;
	int ret;

	buf = dt_value(led, "mode", buf, "release");
	len = strcspn(buf, " \n");
	ret = parse_show(led, buf, len, true, &s);
	if (ret)
		return ret;
	mutex_lock(&led->lock);
	led->battery_mode = s.a == FOLLOW;
	if (!led->battery_mode)
		led->mode = s;
	led->zone = -1;
	evaluate(led);
	mutex_unlock(&led->lock);
	return count;
}
static DEVICE_ATTR_RW(mode);

static ssize_t battery_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);
	int i, n = 0;

	mutex_lock(&led->lock);
	for (i = 0; i < led->nzones; i++) {
		n += sprintf(buf + n, "%s%d ", i ? " " : "", led->zones[i].min);
		n += print_show(led, &led->zones[i].show, buf + n);
	}
	mutex_unlock(&led->lock);
	return n + sprintf(buf + n, "\n");
}

static ssize_t battery_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct status_led *led = dev_get_drvdata(dev);
	struct level zones[MAX_ZONES];
	int n;

	buf = dt_value(led, "battery", buf, led->names[0]);
	if (buf == led->names[0]) {
		/* no DT rules: always the first colour */
		zones[0].min = 0;
		zones[0].show.a = zones[0].show.b = 0;
		n = 1;
	} else {
		n = parse_zones(led, buf, zones);
	}

	if (n < 0)
		return n;
	mutex_lock(&led->lock);
	memcpy(led->zones, zones, sizeof(zones));
	led->nzones = n;
	led->zone = -1;
	evaluate(led);
	mutex_unlock(&led->lock);
	return count;
}
static DEVICE_ATTR_RW(battery);

static ssize_t charger_show(struct status_led *led, struct show *s, char *buf)
{
	int n;

	mutex_lock(&led->lock);
	n = print_show(led, s, buf);
	mutex_unlock(&led->lock);
	return n + sprintf(buf + n, "\n");
}

static ssize_t charger_store(struct status_led *led, struct show *s,
			     const char *name, const char *builtin,
			     const char *buf, size_t count)
{
	size_t len;
	struct show v;
	int ret;

	buf = dt_value(led, name, buf, builtin);
	len = strcspn(buf, " \n");
	ret = parse_show(led, buf, len, true, &v);

	if (ret)
		return ret;
	mutex_lock(&led->lock);
	*s = v;
	evaluate(led);
	mutex_unlock(&led->lock);
	return count;
}

static ssize_t charging_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);

	return charger_show(led, &led->charging, buf);
}

static ssize_t charging_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct status_led *led = dev_get_drvdata(dev);

	return charger_store(led, &led->charging, "charging", "release", buf,
			     count);
}
static DEVICE_ATTR_RW(charging);

static ssize_t full_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);

	return charger_show(led, &led->full, buf);
}

static ssize_t full_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct status_led *led = dev_get_drvdata(dev);

	return charger_store(led, &led->full, "full", "battery", buf, count);
}
static DEVICE_ATTR_RW(full);

static ssize_t notify_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);
	ssize_t n;

	mutex_lock(&led->lock);
	if (led->notify < 0)
		n = sprintf(buf, "none\n");
	else
		n = sprintf(buf, "%s %d %d\n", led->names[led->notify],
			    led->notify_on, led->notify_off);
	mutex_unlock(&led->lock);
	return n;
}

static ssize_t notify_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct status_led *led = dev_get_drvdata(dev);
	char name[NAME_LEN];
	int on = 0, off = 0, c = -1;

	if (!strncmp(buf, "none", 4)) {
		c = -1;
	} else {
		if (sscanf(buf, "%15s %d %d", name, &on, &off) != 3 ||
		    on < 50 || off < 50 || on > 60000 || off > 60000)
			return -EINVAL;
		c = find_color(led, name, strlen(name));
		if (c < 0)
			return c;
	}
	mutex_lock(&led->lock);
	led->notify = c;
	if (c >= 0) {
		led->notify_on = on;
		led->notify_off = off;
	}
	led->phase = 0;
	apply(led);
	mutex_unlock(&led->lock);
	return count;
}
static DEVICE_ATTR_RW(notify);

static ssize_t raw_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);
	int i, n = 0;

	mutex_lock(&led->lock);
	if (!led->raw) {
		n = sprintf(buf, "none");
	} else {
		for (i = 0; i < led->npins; i++)
			n += sprintf(buf + n, "%s%d", i ? " " : "",
				     led->raw_states[i]);
	}
	mutex_unlock(&led->lock);
	return n + sprintf(buf + n, "\n");
}

static ssize_t raw_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct status_led *led = dev_get_drvdata(dev);
	unsigned int v[MAX_PINS];
	int i, n;

	if (!strncmp(buf, "none", 4)) {
		mutex_lock(&led->lock);
		led->raw = false;
		led->shown = -1;
		evaluate(led);
		mutex_unlock(&led->lock);
		return count;
	}
	n = sscanf(buf, "%u %u %u", &v[0], &v[1], &v[2]);
	if (n != led->npins)
		return -EINVAL;
	for (i = 0; i < n; i++)
		if (v[i] > PIN_FLOAT)
			return -EINVAL;
	mutex_lock(&led->lock);
	for (i = 0; i < n; i++)
		led->raw_states[i] = v[i];
	led->raw = true;
	apply(led);
	mutex_unlock(&led->lock);
	return count;
}
static DEVICE_ATTR_RW(raw);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct status_led *led = dev_get_drvdata(dev);
	ssize_t n;

	mutex_lock(&led->lock);
	if (led->raw)
		n = sprintf(buf, "raw\n");
	else if (led->notify >= 0)
		n = sprintf(buf, "notify %s\n", led->names[led->notify]);
	else {
		n = print_show(led, &led->base, buf);
		n += sprintf(buf + n, " %s %d%%\n", led->why, led->capacity);
	}
	mutex_unlock(&led->lock);
	return n;
}
static DEVICE_ATTR_RO(state);

static struct attribute *status_led_attrs[] = {
	&dev_attr_colors.attr,
	&dev_attr_pins.attr,
	&dev_attr_mode.attr,
	&dev_attr_battery.attr,
	&dev_attr_charging.attr,
	&dev_attr_full.attr,
	&dev_attr_notify.attr,
	&dev_attr_raw.attr,
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(status_led);

/* ---------------------------------------------------------------- setup */

static int status_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct status_led *led;
	u32 states[MAX_COLORS * MAX_PINS];
	int i, j, n, ret;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;
	led->dev = dev;
	mutex_init(&led->lock);
	platform_set_drvdata(pdev, led);

	led->gpios = devm_gpiod_get_array(dev, NULL, GPIOD_ASIS);
	if (IS_ERR(led->gpios))
		return PTR_ERR(led->gpios);
	led->npins = led->gpios->ndescs;
	if (led->npins < 1 || led->npins > MAX_PINS)
		return -EINVAL;

	n = of_property_count_strings(np, "color-names");
	if (n < 1 || n > MAX_COLORS)
		return -EINVAL;
	led->ncolors = n;
	for (i = 0; i < n; i++) {
		const char *s;

		of_property_read_string_index(np, "color-names", i, &s);
		strscpy(led->names[i], s, NAME_LEN);
	}
	ret = of_property_read_u32_array(np, "color-states", states,
					 n * led->npins);
	if (ret)
		return ret;
	for (i = 0; i < n; i++)
		for (j = 0; j < led->npins; j++) {
			if (states[i * led->npins + j] > PIN_FLOAT)
				return -EINVAL;
			led->states[i][j] = states[i * led->npins + j];
		}

	led->notify = -1;
	led->zone = -1;
	led->shown = -1;
	INIT_DELAYED_WORK(&led->blink, blink_work);
	INIT_DELAYED_WORK(&led->refresh, refresh_work);
	INIT_WORK(&led->update, update_work);

	ret = devm_device_add_groups(dev, status_led_groups);
	if (ret)
		return ret;

	/* the DT's settings, through the same parsers as the files */
	if (battery_store(dev, NULL, "default", 7) < 0 ||
	    charging_store(dev, NULL, "default", 7) < 0 ||
	    full_store(dev, NULL, "default", 7) < 0 ||
	    mode_store(dev, NULL, "default", 7) < 0)
		dev_warn(dev, "bad andr36oid,* setting in the DT\n");

	led->psy_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&led->psy_nb);
	if (ret)
		return ret;
	schedule_delayed_work(&led->refresh, msecs_to_jiffies(REFRESH_MS));

	dev_info(dev, "%d colours on %d pins\n", led->ncolors, led->npins);
	return 0;
}

static int status_led_remove(struct platform_device *pdev)
{
	struct status_led *led = platform_get_drvdata(pdev);

	power_supply_unreg_notifier(&led->psy_nb);
	cancel_delayed_work_sync(&led->refresh);
	cancel_work_sync(&led->update);
	cancel_delayed_work_sync(&led->blink);
	return 0;
}

static int status_led_suspend(struct device *dev)
{
	struct status_led *led = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&led->refresh);
	cancel_work_sync(&led->update);
	mutex_lock(&led->lock);
	led->suspended = true;
	led->phase = 0;
	apply(led);
	mutex_unlock(&led->lock);
	cancel_delayed_work_sync(&led->blink);
	return 0;
}

static int status_led_resume(struct device *dev)
{
	struct status_led *led = dev_get_drvdata(dev);

	mutex_lock(&led->lock);
	led->suspended = false;
	evaluate(led);
	mutex_unlock(&led->lock);
	schedule_delayed_work(&led->refresh, msecs_to_jiffies(REFRESH_MS));
	return 0;
}

static SIMPLE_DEV_PM_OPS(status_led_pm, status_led_suspend, status_led_resume);

static const struct of_device_id status_led_of_match[] = {
	{ .compatible = "andr36oid,status-led" },
	{ }
};
MODULE_DEVICE_TABLE(of, status_led_of_match);

static struct platform_driver status_led_driver = {
	.probe = status_led_probe,
	.remove = status_led_remove,
	.driver = {
		.name = "andr36oid-status-led",
		.of_match_table = status_led_of_match,
		.pm = &status_led_pm,
	},
};
module_platform_driver(status_led_driver);

MODULE_DESCRIPTION("R36S status LED: colours, battery levels, notifications");
MODULE_LICENSE("GPL v2");
