/*
 * Copyright (c) 2021, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "wow.h"
#include "core.h"
#include "hal_io.h"

#ifdef CONFIG_PM

unsigned char img_suspend_status;
static int host_suspend(void);
struct syscore_ops host_syscore_ops = {
	.suspend = host_suspend,
};

#if 0
static int wait_for_econ_ps_cfg(struct img_priv *priv)
{
	int count = 0;
	char econ_ps_cfg_done = 0;

check_econ_ps_cfg_complete:
	mutex_lock(&priv->mutex);
	econ_ps_cfg_done = priv->econ_ps_cfg_stats.completed;
	mutex_unlock(&priv->mutex);

	if (!econ_ps_cfg_done && (count < PS_ECON_CFG_TIMEOUT_TICKS)) {
		count++;
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(1);
		goto check_econ_ps_cfg_complete;
	}

	if (!econ_ps_cfg_done) {
		RPU_INFO_UMACIF("%s: Didn't get ECON_PS_CFG_DONE event\n",
		       __func__);
		return -1;
	}

	RPU_DEBUG_UMACIF("%s : Received ECON_PS_CFG_DONE event\n",
						__func__);
	return 0;
}
#endif

static int wait_for_all_cmd_done(struct img_priv *priv)
{
	int count = 0;

check_all_cmd_done:

	if (cmd_info.outstanding_ctrl_req && (count < msecs_to_jiffies(1000))) {
		count++;
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(1);
		goto check_all_cmd_done;
	}

	if (cmd_info.outstanding_ctrl_req) {
		RPU_INFO_UMACIF("%s: Failed to wait all cmd done\n",
		       __func__);
		return -1;
	}

	RPU_DEBUG_UMACIF("%s : All cmd done\n",
						__func__);
	return 0;
}

/*
 * Wake the firmware on resume. During a real system resume (during_pm_resume)
 * the M0 did not survive deep sleep, so this SDIO write times out and kicks the
 * fw recovery (power-cycle + re-download). Doing that while dpm_resume is still
 * running races the SoC clock/regulator restore and hard-hangs the Ultra. Defer
 * it to the PM_POST_SUSPEND notifier, which runs once resume has completed.
 */
static void rk915_resume_wake(struct hal_priv *priv)
{
	if (priv->during_pm_resume) {
		priv->defer_resume_wake = 1;
		return;
	}
	rk915_notify_pm(priv, 1);
}

int img_resume(struct ieee80211_hw *hw)

{

	int i = 0;

	int active_vif_index = -1;

	struct img_priv *priv = NULL;



	if (hw == NULL) {

		RPU_ERROR_UMACIF("%s: Invalid parameters\n",

		       __func__);

		return -EINVAL;

	}



	priv = (struct img_priv *)hw->priv;



	RPU_INFO_UMACIF("%s: Resume starting\n", __func__);



	/* If not associated, just notify firmware to wake up */

	if (!priv->params->is_associated) {

		rk915_resume_wake(hpriv);

		img_suspend_status = 0;

		return 0;

	}



	/* Find active VIF */

	for (i = 0; i < MAX_VIFS; i++) {

		if (priv->active_vifs & (1 << i)) {

			active_vif_index = i;

			break;

		}

	}



	if (active_vif_index < 0) {

		RPU_INFO_UMACIF("%s: No active VIF found\n", __func__);

		rk915_resume_wake(hpriv);

		img_suspend_status = 0;

		return 0;

	}



	/* Notify firmware to wake up */

	rk915_resume_wake(hpriv);

	hal_ops.disable_irq_wake();

	img_suspend_status = 0;



	RPU_INFO_UMACIF("%s: Resume successful\n", __func__);



	return 0;

}

int img_suspend(struct ieee80211_hw *hw,
		       struct cfg80211_wowlan *wowlan)
{
	int i = 0;
	int active_vif_index = -1;
	int count = 0;
	struct img_priv *priv = NULL;
	struct ieee80211_vif *vif = NULL;

	if (hw == NULL) {
		RPU_ERROR_UMACIF("%s: Invalid parameters\n",
		       __func__);
		return -EINVAL;
	}

	priv = (struct img_priv *)hw->priv;

	RPU_INFO_UMACIF("%s: Suspend starting\n", __func__);

	/* If not associated, just notify firmware to sleep */
	if (!priv->params->is_associated) {
		rk915_notify_pm(hpriv, 0);
		img_suspend_status = 1;
		return 0;
	}

	/* Check if scanning or ROC is in progress */
	if ((wifi->params.hw_scan_status == HW_SCAN_STATUS_PROGRESS) ||
		(priv->roc_params.roc_starting == 1)) {
		RPU_INFO_UMACIF("%s: Scan/ROC in progress, cannot suspend\n",
			__func__);
		return -EBUSY;
	}

	mutex_lock(&priv->mutex);

	/* Find active VIFs */
	for (i = 0; i < MAX_VIFS; i++) {
		if (priv->active_vifs & (1 << i)) {
			active_vif_index = i;
			count++;
		}
	}

	/* Only support single VIF (STA mode) */
	if (count != 1) {
		RPU_ERROR_UMACIF("%s: Power save only for single VIF (STA mode)\n",
		       __func__);
		mutex_unlock(&priv->mutex);
		return -ENOTSUPP;
	}

	rcu_read_lock();
	vif = rcu_dereference(priv->vifs[active_vif_index]);
	rcu_read_unlock();

	if (vif->type != NL80211_IFTYPE_STATION) {
		RPU_ERROR_UMACIF("%s: VIF is not in STA Mode\n",
		       __func__);
		mutex_unlock(&priv->mutex);
		return -ENOTSUPP;
	}

	/* Wait for all outstanding commands to complete */
	if (wait_for_all_cmd_done(priv) != 0) {
		RPU_ERROR_UMACIF("%s: Outstanding commands pending\n",
			__func__);
		mutex_unlock(&priv->mutex);
		return -EBUSY;
	}

	/* Enter power save mode */
	if (priv->power_save == PWRSAVE_STATE_AWAKE) {
		priv->power_save = PWRSAVE_STATE_DOZE;
		rpu_prog_ps_state(active_vif_index, vif->addr, priv->power_save);
	}

	mutex_unlock(&priv->mutex);

	/* Notify firmware to sleep */
	rk915_notify_pm(hpriv, 0);
	hal_ops.enable_irq_wake();
	img_suspend_status = 1;

	RPU_INFO_UMACIF("%s: Suspend successful\n", __func__);

	return 0;
}

static int host_suspend(void)
{
	/*if ((img_suspend_status == 1) && (rx_interrupt_status == 1)) {
		RPU_ERROR_UMACIF("%s: Interrupt raised during Suspend, cancel suspend",
				__func__);
		return -EBUSY;
	} else */{
		return 0;
	}
}
#endif


