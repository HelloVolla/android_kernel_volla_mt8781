// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 MediaTek Inc.
 */

#include <linux/extcon-provider.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/role.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/phy/phy.h>

#include "extcon-mtk-usb.h"

#if IS_ENABLED(CONFIG_CM_MIDMISC_V_SUPPORT)
#include <mt-plat/middle_misc_v.h>
#endif

#if IS_ENABLED(CONFIG_MID_CSCI_SUPPORT)
#include <mt-plat/csci.h>
#endif

#if IS_ENABLED(CONFIG_TCPC_CLASS)
#include "tcpm.h"

#if IS_ENABLED(CONFIG_CM_CUST_GPIOS_SUPPORT)
#include <mt-plat/cust_gpios.h>
#endif
#if IS_ENABLED(CONFIG_WB_EXTCON_WAKELOCK)
#include <linux/pm_wakeup.h>
struct wakeup_source *extcon_wakelock;
#endif

#if IS_ENABLED(CONFIG_WB_DOCKING_SUPPORT)
int docking_iddig_debounce = 50;
/* Hardware debounce for the dock-detect EINT. A marginal pogo contact makes
 * the line bounce; each bounce fires the EINT and is acted on as a
 * dock/undock event, cycling the keyboard rails and dropping the keyboard
 * off the bus. Filter bounces shorter than this so only a settled
 * dock/undock edge gets through. */
#define DOCKING_EINT_DEBOUNCE_US 128000
static bool docking_iddig_boot = true;
static int docking_eint_gpio;
static int is_docking_det = false;
static int docking_eint_num;
struct pinctrl *extcon_pinctrl;
struct pinctrl_state *docking_eint_default;
static struct delayed_work docking_delay_work;
static struct workqueue_struct *docking_wq;
#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT) //Leo 20231123
static int is_typec_src = false;
static struct delayed_work docking_delay_track_work;
static struct workqueue_struct *docking_track_wq;
#endif
static struct mtk_extcon_info *docking_extcon;
static void cust_usb_docking_otg_vbus_en(int is_on);
static void cust_usb_switch_on(int is_on);
static void cust_hub_enable(int is_on);
extern bool get_is_docking(void);
struct wakeup_source *docking_suspend_lock;
#endif
#endif

//caozy add for usb-switch
#if IS_ENABLED(CONFIG_USB_SWITCH_DEBUG)
static int cur_usb_mode;
static struct mtk_extcon_info *usb_switch_extcon;
struct delayed_work host_mode_dwork;
#endif
//caozy add for usb-switch

static const unsigned int usb_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

#if defined(M100TB_DG_P3PRO_527) //Leo 20230525
#if IS_ENABLED(CONFIG_WB_BOARD_ID_SUPPORT)
extern int cust_midmisc_get_board_id(void);
static struct charger_device *sed_chgdev = NULL;
extern int charger_dev_enable_hz(struct charger_device *chg_dev, bool en);
extern struct charger_device *get_charger_by_name(const char *name);

static void cust_enable_sed_chgdev_hz(bool en) 
{ 
	if (sed_chgdev == NULL) {
		sed_chgdev = get_charger_by_name("secondary_chg");
		pr_info("%s get secondary_chg \n",__func__);
	}

	if (cust_midmisc_get_board_id() == 2) {
		if (sed_chgdev != NULL)
			charger_dev_enable_hz(sed_chgdev, en);
	}
}
#endif
#endif


static void mtk_usb_extcon_update_role(struct work_struct *work)
{
	struct usb_role_info *role = container_of(to_delayed_work(work),
					struct usb_role_info, dwork);
	struct mtk_extcon_info *extcon = role->extcon;
	unsigned int cur_dr, new_dr;

	cur_dr = extcon->c_role;
	new_dr = role->d_role;

	dev_info(extcon->dev, "cur_dr(%d) new_dr(%d)\n", cur_dr, new_dr);
#ifdef CONFIG_WB_EXTCON_WAKELOCK
	if(new_dr == USB_ROLE_HOST) {
		__pm_stay_awake(extcon_wakelock);
	} else {
		if (extcon_wakelock->active)
			__pm_relax(extcon_wakelock);
	}
#endif
	/* none -> device */
	if (cur_dr == USB_ROLE_NONE &&
			new_dr == USB_ROLE_DEVICE) {
		extcon_set_state_sync(extcon->edev, EXTCON_USB, true);
	/* none -> host */
	} else if (cur_dr == USB_ROLE_NONE &&
			new_dr == USB_ROLE_HOST) {
		extcon_set_state_sync(extcon->edev, EXTCON_USB_HOST, true);
	/* device -> none */
	} else if (cur_dr == USB_ROLE_DEVICE &&
			new_dr == USB_ROLE_NONE) {
		extcon_set_state_sync(extcon->edev, EXTCON_USB, false);
	/* host -> none */
	} else if (cur_dr == USB_ROLE_HOST &&
			new_dr == USB_ROLE_NONE) {
//caozy add for usb-switch
#if IS_ENABLED(CONFIG_USB_SWITCH_DEBUG)
		if(cur_usb_mode == 1){
			return;
		}
#endif
//caozy add for usb-switch
		extcon_set_state_sync(extcon->edev, EXTCON_USB_HOST, false);
	/* device -> host */
	} else if (cur_dr == USB_ROLE_DEVICE &&
			new_dr == USB_ROLE_HOST) {
		extcon_set_state_sync(extcon->edev, EXTCON_USB, false);
		extcon_set_state_sync(extcon->edev, EXTCON_USB_HOST, true);
	/* host -> device */
	} else if (cur_dr == USB_ROLE_HOST &&
			new_dr == USB_ROLE_DEVICE) {
#if IS_ENABLED(CONFIG_USB_SWITCH_DEBUG)
//caozy add for usb-switch
		if(cur_usb_mode == 1){
			return;
		}
#endif
//caozy add for usb-switch
		extcon_set_state_sync(extcon->edev, EXTCON_USB_HOST, false);
		extcon_set_state_sync(extcon->edev, EXTCON_USB, true);
	}

#if defined(M100TB_DG_P3PRO_527) //Leo 20230525
#if IS_ENABLED(CONFIG_WB_BOARD_ID_SUPPORT)
	cust_enable_sed_chgdev_hz((new_dr == USB_ROLE_HOST));
#endif
#endif

	/* usb role switch */
	if (extcon->role_sw)
		usb_role_switch_set_role(extcon->role_sw, new_dr);

	extcon->c_role = new_dr;
	kfree(role);
}

static int mtk_usb_extcon_set_role(struct mtk_extcon_info *extcon,
						unsigned int role)
{
	struct usb_role_info *role_info;

	/* create and prepare worker */
	role_info = kzalloc(sizeof(*role_info), GFP_ATOMIC);
	if (!role_info)
		return -ENOMEM;

	INIT_DELAYED_WORK(&role_info->dwork, mtk_usb_extcon_update_role);

	role_info->extcon = extcon;
	role_info->d_role = role;
	/* issue connection work */
	queue_delayed_work(extcon->extcon_wq, &role_info->dwork, 0);

	return 0;
}

static bool usb_is_online(struct mtk_extcon_info *extcon)
{
	union power_supply_propval pval;
	union power_supply_propval tval;
	int ret;

	ret = power_supply_get_property(extcon->usb_psy,
				POWER_SUPPLY_PROP_ONLINE, &pval);
	if (ret < 0) {
		dev_info(extcon->dev, "failed to get online prop\n");
		return false;
	}

	ret = power_supply_get_property(extcon->usb_psy,
				POWER_SUPPLY_PROP_TYPE, &tval);
	if (ret < 0) {
		dev_info(extcon->dev, "failed to get usb type\n");
		return false;
	}

	dev_info(extcon->dev, "online=%d, type=%d\n", pval.intval, tval.intval);

	if (pval.intval && (tval.intval == POWER_SUPPLY_TYPE_USB ||
			tval.intval == POWER_SUPPLY_TYPE_USB_CDP))
		return true;
	else
		return false;
}

static void mtk_usb_extcon_psy_detector(struct work_struct *work)
{
	struct mtk_extcon_info *extcon = container_of(to_delayed_work(work),
		struct mtk_extcon_info, wq_psy);

	/* Workaround for PR_SWAP, IF tcpc_dev, then do not switch role. */
	/* Since we will set USB to none when type-c plug out */
	if (extcon->tcpc_dev) {
		if (usb_is_online(extcon) && extcon->c_role == USB_ROLE_NONE)
			mtk_usb_extcon_set_role(extcon, USB_ROLE_DEVICE);
	} else {
#if IS_ENABLED(CONFIG_TCPC_FUSB302)
		if (usb_is_online(extcon)){
			mtk_usb_extcon_set_role(extcon, USB_ROLE_DEVICE);
		}else{
			if(!extcon->vbus_on){
				mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
			}
		}
#else
		if (usb_is_online(extcon))
			mtk_usb_extcon_set_role(extcon, USB_ROLE_DEVICE);
		else
			mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
#endif
	}

}

static int mtk_usb_extcon_psy_notifier(struct notifier_block *nb,
				unsigned long event, void *data)
{
	struct power_supply *psy = data;
	struct mtk_extcon_info *extcon = container_of(nb,
					struct mtk_extcon_info, psy_nb);

	if (event != PSY_EVENT_PROP_CHANGED || psy != extcon->usb_psy)
		return NOTIFY_DONE;

	queue_delayed_work(system_power_efficient_wq, &extcon->wq_psy, 0);

	return NOTIFY_DONE;
}

static int mtk_usb_extcon_psy_init(struct mtk_extcon_info *extcon)
{
	int ret = 0;
	struct device *dev = extcon->dev;

	extcon->usb_psy = devm_power_supply_get_by_phandle(dev, "charger");
	if (IS_ERR_OR_NULL(extcon->usb_psy)) {
		dev_err(dev, "fail to get usb_psy\n");
		return -EINVAL;
	}

	INIT_DELAYED_WORK(&extcon->wq_psy, mtk_usb_extcon_psy_detector);

#if IS_ENABLED(CONFIG_TCPC_FUSB302)
	queue_delayed_work(extcon->extcon_wq, &extcon->wq_psy, msecs_to_jiffies(1000));
#endif

	extcon->psy_nb.notifier_call = mtk_usb_extcon_psy_notifier;
	ret = power_supply_reg_notifier(&extcon->psy_nb);
	if (ret)
		dev_err(dev, "fail to register notifer\n");
/*
	if (usb_is_online(extcon))
		mtk_usb_extcon_set_role(extcon, USB_ROLE_DEVICE);
	else
		mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
*/
	return ret;
}

#if defined ADAPT_CHARGER_V1
#include <mt-plat/charger_class.h>
static struct charger_device *primary_charger;

static int mtk_usb_extcon_set_vbus_v1(bool is_on) {
	if (!primary_charger) {
		primary_charger = get_charger_by_name("primary_chg");
		if (!primary_charger) {
			pr_info("%s: get primary charger device failed\n", __func__);
			return -ENODEV;
		}
	}
	pr_info("%s: is_on=%d\n", __func__, is_on);
#if defined(CONFIG_MTK_GAUGE_VERSION) && (CONFIG_MTK_GAUGE_VERSION == 30)
	if (is_on) {
		charger_dev_enable_otg(primary_charger, true);
		charger_dev_set_boost_current_limit(primary_charger,
			1500000);
		#if 0
		{// # workaround
			charger_dev_kick_wdt(primary_charger);
			enable_boost_polling(true);
		}
		#endif
	} else {
		charger_dev_enable_otg(primary_charger, false);
		#if 0
			//# workaround
			enable_boost_polling(false);
		#endif
	}
#else
	if (is_on) {
		charger_dev_enable_otg(primary_charger, true);
		charger_dev_set_boost_current_limit(primary_charger,
			1500000);
	} else {
		charger_dev_enable_otg(primary_charger, false);
	}
#endif
		return 0;
}
#endif //ADAPT_CHARGER_V1

static int mtk_usb_extcon_set_vbus(struct mtk_extcon_info *extcon,
							bool is_on)
{
	int ret;


//caozy add for usb-switch
#if IS_ENABLED(CONFIG_USB_SWITCH_DEBUG)
	if(cur_usb_mode == 1){
		extcon->vbus_on = is_on;
		usb_switch_extcon->vbus_on = is_on;
		printk("mtk_usb_extcon_set_vbus use extern vbus\n");
		return 0;
	}
#endif
//caozy add for usb-switch

#if defined ADAPT_CHARGER_V1
	ret = mtk_usb_extcon_set_vbus_v1(is_on);
	extcon->vbus_on = is_on;
#else

	struct regulator *vbus = extcon->vbus;
	struct device *dev = extcon->dev;

	/* vbus is optional */
	if (!vbus || extcon->vbus_on == is_on)
		return 0;

	dev_info(dev, "vbus turn %s\n", is_on ? "on" : "off");

#ifdef CONFIG_WB_OTG_VBUS_USE_EXT_LDO
	ret = 0;
	if(is_on){
#if IS_ENABLED(CONFIG_WB_BOARD_M307TCR110)
		cust_gpio_set_value(CUST_GPIO_CUTOFF_VBUS, 1);
		mdelay(50);
#endif
		cust_gpio_set_value(CUST_GPIO_TYPEC_OTG_EN, 1);
		mdelay(50);
		cust_gpio_set_value(CUST_GPIO_OTG_5V_EN, 1);
	}else{
		cust_gpio_set_value(CUST_GPIO_OTG_5V_EN, 0);
		mdelay(50);
		cust_gpio_set_value(CUST_GPIO_TYPEC_OTG_EN, 0);
#if IS_ENABLED(CONFIG_WB_BOARD_M307TCR110)
		mdelay(50);
		cust_gpio_set_value(CUST_GPIO_CUTOFF_VBUS, 0);
#endif
	}
#else
	if (is_on) {
		if (extcon->vbus_vol) {
			ret = regulator_set_voltage(vbus,
					extcon->vbus_vol, extcon->vbus_vol);
			if (ret) {
				dev_err(dev, "vbus regulator set voltage failed\n");
				return ret;
			}
		}

		if (extcon->vbus_cur) {
			ret = regulator_set_current_limit(vbus,
					extcon->vbus_cur, extcon->vbus_cur);
			if (ret) {
				dev_err(dev, "vbus regulator set current failed\n");
				return ret;
			}
		}

		ret = regulator_enable(vbus);
		if (ret) {
			dev_err(dev, "vbus regulator enable failed\n");
			return ret;
		}
	} else {
		regulator_disable(vbus);
	}
#endif

	extcon->vbus_on = is_on;

#endif //ADAPT_CHARGER_V1
	return 0;
}

#if IS_ENABLED(CONFIG_TCPC_CLASS)
static int mtk_extcon_tcpc_notifier(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct tcp_notify *noti = data;
	struct mtk_extcon_info *extcon =
			container_of(nb, struct mtk_extcon_info, tcpc_nb);
	struct device *dev = extcon->dev;
	bool vbus_on;

	switch (event) {
	case TCP_NOTIFY_SOURCE_VBUS:
		dev_info(dev, "source vbus = %dmv\n",
				 noti->vbus_state.mv);
		vbus_on = (noti->vbus_state.mv) ? true : false;
		mtk_usb_extcon_set_vbus(extcon, vbus_on);
		break;
	case TCP_NOTIFY_TYPEC_STATE:
		dev_info(dev, "old_state=%d, new_state=%d\n",
				noti->typec_state.old_state,
				noti->typec_state.new_state);
		if (noti->typec_state.old_state == TYPEC_UNATTACHED &&
			noti->typec_state.new_state == TYPEC_ATTACHED_SRC) {
			dev_info(dev, "Type-C SRC plug in\n");
#if IS_ENABLED(CONFIG_WB_DOCKING_SUPPORT) //Leo 20230110
#if IS_ENABLED(CONFIG_WB_NO_HUB_SUPPORT)
		if (get_is_docking()){
		} else {
			cust_usb_switch_on(false);
		}
#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT) //Leo 20231222
		is_typec_src = true;
#endif
#else
			cust_usb_switch_on(true);
			cust_hub_enable(true);
#endif
#endif
			mtk_usb_extcon_set_role(extcon, USB_ROLE_HOST);
		} else if (!(extcon->bypss_typec_sink) &&
			noti->typec_state.old_state == TYPEC_UNATTACHED &&
			(noti->typec_state.new_state == TYPEC_ATTACHED_SNK ||
			noti->typec_state.new_state == TYPEC_ATTACHED_NORP_SRC ||
			noti->typec_state.new_state == TYPEC_ATTACHED_CUSTOM_SRC ||
			noti->typec_state.new_state == TYPEC_ATTACHED_DBGACC_SNK)) {
			dev_info(dev, "Type-C SINK plug in\n");
			mtk_usb_extcon_set_role(extcon, USB_ROLE_DEVICE);
		} else if ((noti->typec_state.old_state == TYPEC_ATTACHED_SRC ||
			noti->typec_state.old_state == TYPEC_ATTACHED_SNK ||
			noti->typec_state.old_state == TYPEC_ATTACHED_NORP_SRC ||
			noti->typec_state.old_state == TYPEC_ATTACHED_CUSTOM_SRC ||
			noti->typec_state.old_state == TYPEC_ATTACHED_DBGACC_SNK) &&
			noti->typec_state.new_state == TYPEC_UNATTACHED) {
			dev_info(dev, "Type-C plug out\n");
#if IS_ENABLED(CONFIG_WB_DOCKING_SUPPORT)
			if(get_is_docking()){
			}else{
				cust_usb_switch_on(false);
				cust_hub_enable(false);
				mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
			}
#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT) //Leo 20231222
		is_typec_src = false;
#endif
#else
			mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
#endif
		}
		break;
	case TCP_NOTIFY_DR_SWAP:
		dev_info(dev, "%s dr_swap, new role=%d\n",
				__func__, noti->swap_state.new_role);
		if (noti->swap_state.new_role == PD_ROLE_UFP &&
				extcon->c_role != USB_ROLE_DEVICE) {
			dev_info(dev, "switch role to device\n");
			mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
			mtk_usb_extcon_set_role(extcon, USB_ROLE_DEVICE);
		} else if (noti->swap_state.new_role == PD_ROLE_DFP &&
				extcon->c_role != USB_ROLE_HOST) {
			dev_info(dev, "switch role to host\n");
			mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
			mtk_usb_extcon_set_role(extcon, USB_ROLE_HOST);
		}
		break;
	}

	return NOTIFY_OK;
}

static int mtk_usb_extcon_tcpc_init(struct mtk_extcon_info *extcon)
{
	struct tcpc_device *tcpc_dev;
	struct device_node *np = extcon->dev->of_node;
	const char *tcpc_name;
	int ret;

	ret = of_property_read_string(np, "tcpc", &tcpc_name);
	if (ret < 0)
		return -ENODEV;

	tcpc_dev = tcpc_dev_get_by_name(tcpc_name);
	if (!tcpc_dev) {
		dev_err(extcon->dev, "get tcpc device fail\n");
		return -ENODEV;
	}

	extcon->tcpc_nb.notifier_call = mtk_extcon_tcpc_notifier;
	ret = register_tcp_dev_notifier(tcpc_dev, &extcon->tcpc_nb,
		TCP_NOTIFY_TYPE_USB | TCP_NOTIFY_TYPE_VBUS |
		TCP_NOTIFY_TYPE_MISC);
	if (ret < 0) {
		dev_err(extcon->dev, "register notifer fail\n");
		return -EINVAL;
	}

	extcon->tcpc_dev = tcpc_dev;

	return 0;
}
#endif

static void mtk_usb_extcon_detect_cable(struct work_struct *work)
{
	struct mtk_extcon_info *extcon = container_of(to_delayed_work(work),
		struct mtk_extcon_info, wq_detcable);
	int id;

	/* check ID and update cable state */
	id = extcon->id_gpiod ?
		gpiod_get_value_cansleep(extcon->id_gpiod) : 1;

	/* at first we clean states which are no longer active */
	if (id) {
		mtk_usb_extcon_set_vbus(extcon, false);
#if defined(CONFIG_WB_DC_USB_INPUT_CHARGER_SUPPORT)
		if(is_dc_in()){
			mtk_usb_extcon_set_role(extcon, USB_ROLE_DEVICE);
		}else{
			mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
		}
#else
		mtk_usb_extcon_set_role(extcon, USB_ROLE_NONE);
#endif
	} else {
		mtk_usb_extcon_set_vbus(extcon, true);
		mtk_usb_extcon_set_role(extcon, USB_ROLE_HOST);
	}
}

static irqreturn_t mtk_usb_idpin_handle(int irq, void *dev_id)
{
	struct mtk_extcon_info *extcon = dev_id;

	/* issue detection work */
	queue_delayed_work(system_power_efficient_wq, &extcon->wq_detcable, 0);

	return IRQ_HANDLED;
}

static int mtk_usb_extcon_id_pin_init(struct mtk_extcon_info *extcon)
{
	int ret = 0;
	int id;

#if IS_ENABLED(CONFIG_WB_NO_IDDIG_SUPPORT) //Leo 20240801
	pr_info("%s skip id_pin init !!!\n");
	return 0;
#endif

	extcon->id_gpiod = devm_gpiod_get(extcon->dev, "id", GPIOD_IN);

	if (!extcon->id_gpiod || IS_ERR(extcon->id_gpiod)) {
		dev_info(extcon->dev, "failed to get id gpio\n");
		return -ENODEV;
	}

	extcon->id_irq = gpiod_to_irq(extcon->id_gpiod);
	if (extcon->id_irq < 0) {
		dev_info(extcon->dev, "failed to get ID IRQ\n");
		return extcon->id_irq;
	}

	INIT_DELAYED_WORK(&extcon->wq_detcable, mtk_usb_extcon_detect_cable);

	ret = devm_request_threaded_irq(extcon->dev, extcon->id_irq, NULL,
			mtk_usb_idpin_handle, IRQF_TRIGGER_RISING |
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			dev_name(extcon->dev), extcon);

	if (ret < 0) {
		dev_info(extcon->dev, "failed to request handler for ID IRQ\n");
		return ret;
	}

	/* get id pin value when boot on */
	id = extcon->id_gpiod ?
		gpiod_get_value_cansleep(extcon->id_gpiod) : 1;
	dev_info(extcon->dev, "id value : %d\n", id);
	if (!id) {
		mtk_usb_extcon_set_vbus(extcon, true);
		mtk_usb_extcon_set_role(extcon, USB_ROLE_HOST);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_TCPC_CLASS)
#define PROC_FILE_SMT "mtk_typec"
#define FILE_SMT_U2_CC_MODE "mtk_typec/smt_u2_cc_mode"

static int usb_cc_smt_procfs_show(struct seq_file *s, void *unused)
{
	struct mtk_extcon_info *extcon = s->private;
	struct device_node *np = extcon->dev->of_node;
	const char *tcpc_name;
	uint8_t cc1, cc2;
	char buf[2];
	int ret;

	ret = of_property_read_string(np, "tcpc", &tcpc_name);
	if (ret < 0)
		return -ENODEV;

	extcon->tcpc_dev = tcpc_dev_get_by_name(tcpc_name);
	if (!extcon->tcpc_dev)
		return -ENODEV;

	tcpm_inquire_remote_cc(extcon->tcpc_dev, &cc1, &cc2, false);
	dev_info(extcon->dev, "cc1=%d, cc2=%d\n", cc1, cc2);

	if (cc1 == TYPEC_CC_VOLT_OPEN || cc1 == TYPEC_CC_DRP_TOGGLING)
		seq_puts(s, "0\n");
	else if (cc2 == TYPEC_CC_VOLT_OPEN || cc2 == TYPEC_CC_DRP_TOGGLING)
		seq_puts(s, "0\n");
	else
		seq_puts(s, "1\n");
	buf[1] = '\0';

	return 0;
}

static int usb_cc_smt_procfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, usb_cc_smt_procfs_show, PDE_DATA(inode));
}

static const struct  proc_ops usb_cc_smt_procfs_fops = {
	.proc_open = usb_cc_smt_procfs_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int mtk_usb_extcon_procfs_init(struct mtk_extcon_info *extcon)
{
	struct proc_dir_entry *file;
	struct proc_dir_entry *root;

	root = proc_mkdir(PROC_FILE_SMT, NULL);
	file = proc_create_data(FILE_SMT_U2_CC_MODE, 0400, NULL,
		&usb_cc_smt_procfs_fops, extcon);

	return 0;
}
#endif


#ifdef CONFIG_WB_DOCKING_TYPEC_OTG_SUPPORT
bool get_is_typec_otg_enable(void)
{
	return docking_extcon->vbus_on;
}
#endif

#if IS_ENABLED(CONFIG_WB_DOCKING_SUPPORT)
#define PHY_MODE_BC11_SET 1
#define PHY_MODE_BC11_CLR 2

#if 0 //Leo 20230202
static int cust_usb_set_usbsw(struct device *dev)
{
	struct phy *phy;
	int ret, mode;
	mode = PHY_MODE_BC11_CLR;
	
	phy = phy_get(dev, "usb2-phy");
	if (IS_ERR_OR_NULL(phy)) {
		dev_err(dev, "failed to get usb2-phy\n");
		return -ENODEV;
	}
	ret = phy_set_mode_ext(phy, PHY_MODE_USB_DEVICE, mode);
	if (ret)
		dev_err(dev, "failed to set phy ext mode\n");
	phy_put(dev, phy);
	return ret;
}
#endif

bool get_is_docking(void)
{
	is_docking_det= !gpio_get_value(docking_eint_gpio);
	printk("docking %s val:%d \n",__func__,is_docking_det);
	return is_docking_det;
}

static void cust_usb_docking_otg_vbus_en(int is_on)
{
#if IS_ENABLED(CONFIG_CM_CUST_GPIOS_SUPPORT)
	cust_gpio_set_value(CUST_GPIO_DOCKING_EN , is_on);
#endif
}

static void cust_usb_switch_on(int is_on)
{
#if IS_ENABLED(CONFIG_CM_CUST_GPIOS_SUPPORT)
	cust_gpio_set_value(CUST_GPIO_USB_SWITCH, is_on);
#endif
}

static void cust_hub_enable(int is_on) 
{
	cust_gpio_set_value(CUST_GPIO_OTG_5V_EN , is_on);
	mdelay(50);
}

static void do_docking_delay_work(struct work_struct *work)
{
	union power_supply_propval pval;
	union power_supply_propval prop2 = {0};
	int ret = 0;
	int is_docking = get_is_docking();
	ret = power_supply_get_property(docking_extcon->usb_psy,
				POWER_SUPPLY_PROP_ONLINE, &pval);
		ret = power_supply_get_property(docking_extcon->usb_psy,
			POWER_SUPPLY_PROP_TYPE, &prop2);

	if (!docking_extcon) {
		printk("docking_extcon = NULL\n");
		return;
	}

#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT)
	cancel_delayed_work_sync(&docking_delay_track_work);
#endif

#if IS_ENABLED(CONFIG_WB_NO_HUB_SUPPORT) //Leo 20230303
	cust_usb_switch_on(is_docking);
	cust_hub_enable(is_docking);
#else
	if (get_is_typec_otg_enable() == false) {
		cust_usb_switch_on(is_docking);
		cust_hub_enable(is_docking);
	}
#endif

	cust_usb_docking_otg_vbus_en(is_docking);

	if (get_is_docking()) {
		printk("docking mt_usb_host_connect 1 \n");
#if IS_ENABLED(CONFIG_CHARGER_MT6375)
		if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
			cust_set_bc12_en(0);
		}
#endif

#if 0//IS_ENABLED(CONFIG_CHARGER_SC8989X) //Leo 20231123
#if IS_ENABLED(CONFIG_TCPC_HUSB311) 
		if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
			cust_set_bc12_en_sc8989x(false);
		}
#endif
#endif

#ifdef CONFIG_WB_DOCKING_TYPEC_OTG_SUPPORT
		if(!get_is_typec_otg_enable())
			mtk_usb_extcon_set_role(docking_extcon,  USB_ROLE_HOST);
		irq_set_irq_type(docking_eint_num, IRQF_TRIGGER_HIGH);
#else
		mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_HOST);
		irq_set_irq_type(docking_eint_num, IRQF_TRIGGER_HIGH);
#endif
	} else {
			printk("docking mt_usb_host_connect 0 \n");
#if IS_ENABLED(CONFIG_CHARGER_MT6375)
			if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
				cust_set_bc12_en(1);
			}
#endif

#if 0//IS_ENABLED(CONFIG_CHARGER_SC8989X) //Leo 20231123
#if IS_ENABLED(CONFIG_TCPC_HUSB311) 
			if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
				cust_set_bc12_en_sc8989x(true);
			}
#endif
#endif

#ifdef CONFIG_WB_DOCKING_TYPEC_OTG_SUPPORT
		if(!get_is_typec_otg_enable()) {
			if(!pval.intval){
				mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_NONE);
			} else {
				mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_DEVICE);
			}
		}
		irq_set_irq_type(docking_eint_num, IRQF_TRIGGER_LOW);
#else
#if defined(M300S_XL_352) || defined(M300S_GRTY_342)
		mtk_usb_extcon_set_role(docking_extcon, docking_extcon, USB_ROLE_NONE); //jnier 20221025
#endif		
		//mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_DEVICE);
		if(!pval.intval){
			mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_NONE);
		}
		irq_set_irq_type(docking_eint_num, IRQF_TRIGGER_LOW);
#endif
	}

	enable_irq(docking_eint_num);

#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT) //Leo 20231123
	queue_delayed_work(docking_track_wq, &docking_delay_track_work, msecs_to_jiffies(3000)); //delay 3s check again
#endif

}

#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT) //Leo 20231123
static void do_docking_delay_track_work(struct work_struct *work)
{
	union power_supply_propval pval;
	union power_supply_propval prop2 = {0};
	int ret = 0;
	int is_docking = get_is_docking();
	int is_host = false;
	ret = power_supply_get_property(docking_extcon->usb_psy,
				POWER_SUPPLY_PROP_ONLINE, &pval);
		ret = power_supply_get_property(docking_extcon->usb_psy,
			POWER_SUPPLY_PROP_TYPE, &prop2);

	if (!docking_extcon) {
		printk("%s docking_extcon = NULL\n",__func__);
		return;
	}

	is_host = extcon_get_state(docking_extcon->edev, EXTCON_USB_HOST);

	pr_info("%s is_docking:%s is_host:%d \n",__func__, is_docking ? "in" : "out",is_host);
	if (is_host < 0) {
		pr_info("%s get host state error, return",__func__);
		return;
	}

#if IS_ENABLED(CONFIG_WB_NO_HUB_SUPPORT) //Leo 20230303
	cust_usb_switch_on(is_docking);
	cust_hub_enable(is_docking);
#else
	if (get_is_typec_otg_enable() == false) {
		cust_usb_switch_on(is_docking);
		cust_hub_enable(is_docking);
	}
#endif

	cust_usb_docking_otg_vbus_en(is_docking);

	if ((is_docking == true) && (is_host == false)) {
		pr_info("%s docking mt_usb_host_connect is_docking:%d is_host:%d\n",__func__,is_docking,is_host);
#if IS_ENABLED(CONFIG_CHARGER_MT6375)
		if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
			cust_set_bc12_en(0);
		}
#endif

#if 0//IS_ENABLED(CONFIG_CHARGER_SC8989X) //Leo 20231123
#if IS_ENABLED(CONFIG_TCPC_HUSB311) 
		if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
			cust_set_bc12_en_sc8989x(false);
		}
#endif
#endif

		mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_HOST);
	} else if ((is_docking == false) && (is_host == true)) {
		pr_info("%s docking mt_usb_host_disconect is_docking:%d is_host:%d\n",__func__,is_docking,is_host);

#ifdef CONFIG_WB_DOCKING_TYPEC_OTG_SUPPORT
		if(!get_is_typec_otg_enable()) {

			#if IS_ENABLED(CONFIG_CHARGER_MT6375)
			if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
				cust_set_bc12_en(1);
			}
			#endif
			#if 0//IS_ENABLED(CONFIG_CHARGER_SC8989X) //Leo 20231123
			#if IS_ENABLED(CONFIG_TCPC_HUSB311) 
			if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
				cust_set_bc12_en_sc8989x(true);
			}
			#endif
			#endif

			if (is_typec_src == true) {
				return;
			}

			if(!pval.intval){
				mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_NONE);
			} else {
				mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_DEVICE);
			}
		}
		//irq_set_irq_type(docking_eint_num, IRQF_TRIGGER_LOW);
#else
		#if IS_ENABLED(CONFIG_CHARGER_MT6375)
		if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
			cust_set_bc12_en(1);
		}
		#endif
		#if 0//IS_ENABLED(CONFIG_CHARGER_SC8989X) //Leo 20231123
		#if IS_ENABLED(CONFIG_TCPC_HUSB311) 
		if (pval.intval && (prop2.intval == POWER_SUPPLY_TYPE_USB_DCP)) {
			cust_set_bc12_en_sc8989x(true);
		}
		#endif
		#endif

		if (is_typec_src == true) {
			return;
		}

		if (!pval.intval) {
			mtk_usb_extcon_set_role(docking_extcon, USB_ROLE_NONE);
		}
		//irq_set_irq_type(docking_eint_num, IRQF_TRIGGER_LOW);
#endif
	}

	queue_delayed_work(docking_track_wq, &docking_delay_track_work, msecs_to_jiffies(3000)); //delay 3s check again
}
#endif

static irqreturn_t mt_usb_docking_int(int irq, void *dev_id)
{

	__pm_wakeup_event(docking_suspend_lock, 2000); //Leo 20240304

	if (docking_iddig_boot) {
		docking_iddig_debounce = 50;
		docking_iddig_boot = false;
	} else{
		docking_iddig_debounce = 50;
	}

#if defined(M100TBR210_KJ_965) //Leo 20231128
		docking_iddig_debounce = 300;
#endif

	queue_delayed_work(docking_wq, &docking_delay_work, msecs_to_jiffies(docking_iddig_debounce));
	disable_irq_nosync(docking_eint_num);

	return IRQ_HANDLED;
}

#if 0
#define BOOT_COMPLETED_CHAIN    0x10U
extern int register_boot_completed_notifier(struct notifier_block *nb);

int musb_cust_register_eint(void)
{
	int ret;
	ret = request_irq(docking_eint_num, mt_usb_docking_int,
					IRQF_TRIGGER_LOW, "docking_det", NULL);
	if (ret) {
		pr_err("docking : request EINT <%d> fail, ret<%d>\n", docking_eint_num, ret);
		return ret;
	}
	return ret;
}

int boot_completed_event(struct notifier_block *nb, unsigned long event,
	void *v)
{
	switch(event){
		case BOOT_COMPLETED_CHAIN:
			musb_cust_register_eint();
			break;

		default:
			break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block boot_completed_notifier = {
	.notifier_call = boot_completed_event,
};
#endif
#endif

//caozy add for usb-switch
//sys/bus/platform/drivers/mtk-extcon-usb/extcon_usb_mode
#if IS_ENABLED(CONFIG_USB_SWITCH_DEBUG)
static ssize_t extcon_usb_mode_show(struct device_driver *ddri, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "usb_mode:0-none 1-host 2-device, cur_usb_mode=%d\n", cur_usb_mode);
}

static ssize_t extcon_usb_mode_store(struct device_driver *ddri, const char *buf, size_t count)
{
	int error;
	int data;

    error = kstrtouint(buf, 10, &data);
    if (error)
    {
		printk("invalid input");
        return error;
    }
    printk("extcon_usb_mode_store vbus_on=%d\n", usb_switch_extcon->vbus_on);

	if(data > 2 || data < 0){
		cur_usb_mode = 0;
	}else{
		cur_usb_mode = data;
	}

	if(cur_usb_mode == 0){//none
		printk("extcon_usb_mode_store 000000000000000000\n");
		mtk_usb_extcon_set_role(usb_switch_extcon, USB_ROLE_NONE);
	}else if(cur_usb_mode == 1){//host
		printk("extcon_usb_mode_store 111111111111111111\n");

		if(usb_switch_extcon->vbus_on){
			printk("extcon_usb_mode_store close vbus5v\n");
			mtk_usb_extcon_set_vbus_v1(false);
		}

		mtk_usb_extcon_set_role(usb_switch_extcon, USB_ROLE_HOST);
	}else if(cur_usb_mode == 2){//device
		printk("extcon_usb_mode_store 222222222222222222\n");
		mtk_usb_extcon_set_role(usb_switch_extcon, USB_ROLE_DEVICE);
	}

	return count;
}

static DRIVER_ATTR_RW(extcon_usb_mode);

static struct driver_attribute *extcon_attr_list[] = {
	&driver_attr_extcon_usb_mode,
};

static int extcon_usb_switch_create_attr(struct device_driver *driver)
{
	int idx, err = 0;
	int num = (int)(ARRAY_SIZE(extcon_attr_list));

	if (driver == NULL)
		return -EINVAL;

	for (idx = 0; idx < num; idx++) {
		err = driver_create_file(driver, extcon_attr_list[idx]);
		if (err != 0) {
			printk("driver_create_file (%s) = %d\n",
				extcon_attr_list[idx]->attr.name, err);
			break;
		}
	}
	return err;
}

static void do_host_delay_work(struct work_struct *work)
{
	bool host_mode_enable = false;

	if (csci_exist("host_mode_enable")) {
	    if (csci_integer("host_mode_enable", 0) > 0) {
	        host_mode_enable = csci_integer("host_mode_enable", 0);
	    }
	}

	if(host_mode_enable && cust_mid_misc_v_get_boot_mode() == 0){
		printk("----do_host_delay_work on-------\n");
		mtk_usb_extcon_set_vbus_v1(true);
		mtk_usb_extcon_set_role(usb_switch_extcon, USB_ROLE_HOST);
	}
}
#endif
//caozy add for usb-switch

static int mtk_usb_extcon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_extcon_info *extcon;
	const char *tcpc_name;
	int ret;

	extcon = devm_kzalloc(&pdev->dev, sizeof(*extcon), GFP_KERNEL);
	if (!extcon)
		return -ENOMEM;

	extcon->dev = dev;

	/* extcon */
	extcon->edev = devm_extcon_dev_allocate(dev, usb_extcon_cable);
	if (IS_ERR(extcon->edev)) {
		dev_err(dev, "failed to allocate extcon device\n");
		return -ENOMEM;
	}

	ret = devm_extcon_dev_register(dev, extcon->edev);
	if (ret < 0) {
		dev_info(dev, "failed to register extcon device\n");
		return ret;
	}

	/* usb role switch */
	extcon->role_sw = usb_role_switch_get(extcon->dev);
	if (IS_ERR(extcon->role_sw)) {
		dev_err(dev, "failed to get usb role\n");
		return PTR_ERR(extcon->role_sw);
	}

	/* initial usb role */
	if (extcon->role_sw)
		extcon->c_role = USB_ROLE_NONE;

	/* vbus */
	extcon->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(extcon->vbus)) {
		dev_err(dev, "failed to get vbus\n");
		return PTR_ERR(extcon->vbus);
	}

	/* sync vbus state */
	if (extcon->vbus) {
		extcon->vbus_on = regulator_is_enabled(extcon->vbus);
		dev_info(dev, "vbus is %s\n", extcon->vbus_on ? "on" : "off");

		if (!of_property_read_u32(dev->of_node, "vbus-voltage",
					&extcon->vbus_vol))
			dev_info(dev, "vbus-voltage=%d", extcon->vbus_vol);

		if (!of_property_read_u32(dev->of_node, "vbus-current",
					&extcon->vbus_cur))
			dev_info(dev, "vbus-current=%d", extcon->vbus_cur);
#if defined ADAPT_CHARGER_V1  //jnier add 20230517
		extcon->vbus_on = false;	
#endif
	}

	extcon->bypss_typec_sink =
		of_property_read_bool(dev->of_node,
			"mediatek,bypss-typec-sink");

#if IS_ENABLED(CONFIG_TCPC_CLASS)
	ret = of_property_read_string(dev->of_node, "tcpc", &tcpc_name);
	if (of_property_read_bool(dev->of_node, "mediatek,u2") && ret == 0
		&& strcmp(tcpc_name, "type_c_port0") == 0) {
		dev_info(dev, "create %d dir\n", PROC_FILE_SMT);
		mtk_usb_extcon_procfs_init(extcon);
	}
#endif

	extcon->extcon_wq = create_singlethread_workqueue("extcon_usb");
	if (!extcon->extcon_wq)
		return -ENOMEM;

	/* get id resources */
	ret = mtk_usb_extcon_id_pin_init(extcon);
	if (ret < 0)
		dev_info(dev, "failed to init id pin\n");

	/* power psy */
	ret = mtk_usb_extcon_psy_init(extcon);
	if (ret < 0)
		dev_err(dev, "failed to init psy\n");

#if IS_ENABLED(CONFIG_TCPC_CLASS)
	/* tcpc */
	ret = mtk_usb_extcon_tcpc_init(extcon);
	if (ret < 0)
		dev_err(dev, "failed to init tcpc\n");
#endif

#if IS_ENABLED(CONFIG_WB_DOCKING_SUPPORT)
	INIT_DELAYED_WORK(&docking_delay_work, do_docking_delay_work);
	docking_wq = create_singlethread_workqueue("docking_wq");
#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT) //Leo 20231123
	INIT_DELAYED_WORK(&docking_delay_track_work, do_docking_delay_track_work);
	docking_track_wq = create_singlethread_workqueue("docking_track_wq");
#endif

	ret = of_get_named_gpio(dev->of_node, "docking_eint_gpio", 0);
	if (ret < 0) {
		pr_err("MUSB docking :%s no docking_eint_gpio \n", __func__);
		return ret;
	}
	docking_eint_gpio = ret;

	ret = gpio_request(docking_eint_gpio, "docking_eint_gpio");
	if (ret < 0) {
		pr_err("MUSB docking :%s : docking_eint_gpio failed!\n",__func__);
		return ret;
	}

	ret = gpio_set_debounce(docking_eint_gpio, DOCKING_EINT_DEBOUNCE_US);
	if (ret < 0)
		pr_info("MUSB docking :%s : set debounce failed, ret<%d>\n",
			__func__, ret);

#if 1 //Leo add for pull up docking eint pin 20230327
	extcon_pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(extcon_pinctrl)) {
		ret = PTR_ERR(extcon_pinctrl);
		dev_info(&pdev->dev, "Cannot find extcon_pinctrl!\n");
		//return ret;
	} else {
		docking_eint_default = pinctrl_lookup_state(extcon_pinctrl, "default");
		if (IS_ERR(docking_eint_default)) {
			ret = PTR_ERR(docking_eint_default);
			dev_info(&pdev->dev,"Cannot find pinctrl docking_eint_default %d!\n", ret);
		} else {
			pinctrl_select_state(extcon_pinctrl, docking_eint_default);
		}
	}
#endif

	docking_eint_num = irq_of_parse_and_map(dev->of_node, 0);
	printk("docking_eint_num<%d>\n", docking_eint_num);
	if (docking_eint_num < 0)
		return -ENODEV;


	ret = request_irq(docking_eint_num, mt_usb_docking_int,
					IRQF_TRIGGER_LOW, "docking_det", NULL);
	if (ret) {
		pr_err("docking : request EINT <%d> fail, ret<%d>\n", docking_eint_num, ret);
		return ret;
	}
	
	irq_set_irq_wake(docking_eint_num, 1);

	extcon->docking_det_gpio = docking_eint_gpio;


	docking_suspend_lock = wakeup_source_register(NULL, "dock wakelock");
	if (!docking_suspend_lock) {
		pr_err("docking_suspend_lock wakeup source init failed.\n");
		return -ENODEV;
	}


	docking_extcon = extcon;
#endif
	platform_set_drvdata(pdev, extcon);

#ifdef CONFIG_WB_EXTCON_WAKELOCK
	extcon_wakelock = wakeup_source_register(NULL, "extcon_wakelock");
#endif

//caozy add for usb-switch
#if IS_ENABLED(CONFIG_USB_SWITCH_DEBUG)
    ret = extcon_usb_switch_create_attr(extcon->dev->driver);
    if (ret){
        printk("extcon_usb_switch_create_attr to failed : %d\n", ret);
    }
    usb_switch_extcon = extcon;

    INIT_DELAYED_WORK(&host_mode_dwork, do_host_delay_work);
    queue_delayed_work(system_power_efficient_wq, &host_mode_dwork, msecs_to_jiffies(10000));
#endif
//caozy add for usb-switch

	return 0;
}

static int mtk_usb_extcon_remove(struct platform_device *pdev)
{
	return 0;
}

static void mtk_usb_extcon_shutdown(struct platform_device *pdev)
{
	struct mtk_extcon_info *extcon = platform_get_drvdata(pdev);

	dev_info(extcon->dev, "%s\n", __func__);

	mtk_usb_extcon_set_vbus(extcon, false);
}

static int __maybe_unused extcon_usb_suspend(struct device *dev)
{
	pr_info("%s Entry! \n",__func__);

#if IS_ENABLED(CONFIG_WB_DOCKING_SUPPORT) //Leo 20230511
	irq_set_irq_wake(docking_eint_num, 1);
#if defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT)
	cancel_delayed_work_sync(&docking_delay_track_work);
#endif
#endif
	return 0;
}

static int __maybe_unused extcon_usb_resume(struct device *dev)
{
	pr_info("%s Entry! \n",__func__);
	
#if IS_ENABLED(CONFIG_WB_DOCKING_SUPPORT) //Leo 20230511
	irq_set_irq_wake(docking_eint_num, 0);
#if 0//defined(CONFIG_WB_DOCKING_POLLING_TRACK_SUPPORT) // only int trigger queque.
	queue_delayed_work(docking_track_wq, &docking_delay_track_work, msecs_to_jiffies(3000)); 
#endif
#endif
	return 0;
}

static SIMPLE_DEV_PM_OPS(extcon_usb_pm_ops, extcon_usb_suspend, extcon_usb_resume);

static const struct of_device_id mtk_usb_extcon_of_match[] = {
	{ .compatible = "mediatek,extcon-usb", },
	{ },
};
MODULE_DEVICE_TABLE(of, mtk_usb_extcon_of_match);

static struct platform_driver mtk_usb_extcon_driver = {
	.probe		= mtk_usb_extcon_probe,
	.remove		= mtk_usb_extcon_remove,
	.shutdown	= mtk_usb_extcon_shutdown,
	.driver		= {
		.name	= "mtk-extcon-usb",
		.of_match_table = mtk_usb_extcon_of_match,
		.pm	= &extcon_usb_pm_ops,
	},
};

static int __init mtk_usb_extcon_init(void)
{
	return platform_driver_register(&mtk_usb_extcon_driver);
}
late_initcall(mtk_usb_extcon_init);

static void __exit mtk_usb_extcon_exit(void)
{
	platform_driver_unregister(&mtk_usb_extcon_driver);
}
module_exit(mtk_usb_extcon_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek Extcon USB Driver");
