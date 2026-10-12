/*
 * Rockchip Generic power configuration support.
 *
 * Copyright (c) 2017 ROCKCHIP, Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/machine.h>
#include <linux/rockchip/rockchip_sip.h>
#include <linux/suspend.h>
#include <dt-bindings/input/input.h>

#define PM_INVALID_GPIO	0xffff

static const struct of_device_id pm_match_table[] = {
	{ .compatible = "rockchip,pm-px30",},
	{ .compatible = "rockchip,pm-rk1808",},
	{ .compatible = "rockchip,pm-rk322x",},
	{ .compatible = "rockchip,pm-rk3288",},
	{ .compatible = "rockchip,pm-rk3308",},
	{ .compatible = "rockchip,pm-rk3328",},
	{ .compatible = "rockchip,pm-rk3368",},
	{ .compatible = "rockchip,pm-rk3399",},
	{ .compatible = "rockchip,pm-rv1126",},
	{ },
};

/*
 * andr36oid: Settings > Deep sleep changes the sleep and wake-up flags while the
 * console runs, to find the cause of consoles that never wake. The firmware (BL31)
 * keeps what it gets here and uses it at every sleep. Writing "default" goes back
 * to the DTB's value. Init writes the saved choice (persist.andr36oid.sleep_mode,
 * persist.andr36oid.sleep_wakeup) at every start.
 */
struct sleep_cfg {
	u32 ctrl;
	u32 dt;
	u32 cur;
	bool known;
};

static struct sleep_cfg sleep_mode_cfg = { .ctrl = SUSPEND_MODE_CONFIG };
static struct sleep_cfg sleep_wakeup_cfg = { .ctrl = WKUP_SOURCE_CONFIG };

static int sleep_cfg_set(const char *val, const struct kernel_param *kp)
{
	struct sleep_cfg *cfg = kp->arg;
	u32 v;
	int ret;

	if (!cfg->known)
		return -ENODEV;
	if (sysfs_streq(val, "default") || sysfs_streq(val, "")) {
		v = cfg->dt;
	} else {
		ret = kstrtou32(val, 0, &v);
		if (ret)
			return ret;
	}
	ret = sip_smc_set_suspend_mode(cfg->ctrl, v, 0);
	if (ret)
		return -EIO;
	cfg->cur = v;
	pr_info("rockchip-suspend: %s = 0x%x (DTB: 0x%x)\n", kp->name, v, cfg->dt);
	return 0;
}

static int sleep_cfg_get(char *buf, const struct kernel_param *kp)
{
	struct sleep_cfg *cfg = kp->arg;

	return sprintf(buf, "0x%x default 0x%x\n", cfg->cur, cfg->dt);
}

static const struct kernel_param_ops sleep_cfg_ops = {
	.set = sleep_cfg_set,
	.get = sleep_cfg_get,
};

module_param_cb(sleep_mode, &sleep_cfg_ops, &sleep_mode_cfg, 0644);
module_param_cb(sleep_wakeup, &sleep_cfg_ops, &sleep_wakeup_cfg, 0644);

static void sleep_cfg_init(struct sleep_cfg *cfg, u32 v)
{
	cfg->dt = v;
	cfg->cur = v;
	cfg->known = true;
}

static void rockchip_pm_virt_pwroff_prepare(void)
{
	int error;

	regulator_suspend_prepare(PM_SUSPEND_MEM);

	error = disable_nonboot_cpus();
	if (error) {
		pr_err("Disable nonboot cpus failed!\n");
		return;
	}

	sip_smc_set_suspend_mode(VIRTUAL_POWEROFF, 0, 1);
	sip_smc_virtual_poweroff();
}

static int __init pm_config_init(struct platform_device *pdev)
{
	const struct of_device_id *match_id;
	struct device_node *node;
	u32 mode_config = 0;
	u32 wakeup_config = 0;
	u32 pwm_regulator_config = 0;
	int gpio_temp[10];
	u32 sleep_debug_en = 0;
	u32 apios_suspend = 0;
	u32 virtual_poweroff_en = 0;
	enum of_gpio_flags flags;
	int i = 0;
	int length;

	match_id = of_match_node(pm_match_table, pdev->dev.of_node);
	if (!match_id)
		return -ENODEV;

	node = of_find_node_by_name(NULL, "rockchip-suspend");

	if (IS_ERR_OR_NULL(node)) {
		dev_err(&pdev->dev, "%s dev node err\n",  __func__);
		return -ENODEV;
	}

	if (of_property_read_u32_array(node,
				       "rockchip,sleep-mode-config",
				       &mode_config, 1))
		dev_warn(&pdev->dev, "not set sleep mode config\n");
	else {
		sip_smc_set_suspend_mode(SUSPEND_MODE_CONFIG, mode_config, 0);
		sleep_cfg_init(&sleep_mode_cfg, mode_config);
	}

	if (of_property_read_u32_array(node,
				       "rockchip,wakeup-config",
				       &wakeup_config, 1))
		dev_warn(&pdev->dev, "not set wakeup-config\n");
	else {
		sip_smc_set_suspend_mode(WKUP_SOURCE_CONFIG, wakeup_config, 0);
		sleep_cfg_init(&sleep_wakeup_cfg, wakeup_config);
	}

	if (of_property_read_u32_array(node,
				       "rockchip,pwm-regulator-config",
				       &pwm_regulator_config, 1))
		dev_warn(&pdev->dev, "not set pwm-regulator-config\n");
	else
		sip_smc_set_suspend_mode(PWM_REGULATOR_CONFIG,
					 pwm_regulator_config,
					 0);

	length = of_gpio_named_count(node, "rockchip,power-ctrl");

	if (length > 0 && length < 10) {
		for (i = 0; i < length; i++) {
			gpio_temp[i] = of_get_named_gpio_flags(node,
							     "rockchip,power-ctrl",
							     i,
							     &flags);
			if (!gpio_is_valid(gpio_temp[i]))
				break;
			sip_smc_set_suspend_mode(GPIO_POWER_CONFIG,
						 i,
						 gpio_temp[i]);
		}
	}
	sip_smc_set_suspend_mode(GPIO_POWER_CONFIG, i, PM_INVALID_GPIO);

	if (!of_property_read_u32_array(node,
					"rockchip,sleep-debug-en",
					&sleep_debug_en, 1))
		sip_smc_set_suspend_mode(SUSPEND_DEBUG_ENABLE,
					 sleep_debug_en,
					 0);

	if (!of_property_read_u32_array(node,
					"rockchip,apios-suspend",
					&apios_suspend, 1))
		sip_smc_set_suspend_mode(APIOS_SUSPEND_CONFIG,
					 apios_suspend,
					 0);

	if (!of_property_read_u32_array(node,
					"rockchip,virtual-poweroff",
					&virtual_poweroff_en, 1) &&
	    virtual_poweroff_en)
		pm_power_off_prepare = rockchip_pm_virt_pwroff_prepare;

	return 0;
}

static struct platform_driver pm_driver = {
	.driver = {
		.name = "rockchip-pm",
		.of_match_table = pm_match_table,
	},
};

static int __init rockchip_pm_drv_register(void)
{
	return platform_driver_probe(&pm_driver, pm_config_init);
}
subsys_initcall(rockchip_pm_drv_register);
MODULE_DESCRIPTION("Rockchip suspend mode config");
MODULE_LICENSE("GPL");
