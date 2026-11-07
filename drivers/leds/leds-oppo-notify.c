// SPDX-License-Identifier: GPL-2.0
/*
 * OPPO A57 (MSM8940) 通知灯 / 呼吸灯
 *
 * 这台机器上唯一注册出来的 LED 是 qpnp-smbcharger 建的 "red"
 * (/sys/class/leds/red)，而 vendor 的 lights HAL
 * (android.hardware.light@2.0-service.oppo_thortanium) 找的是
 *   /sys/class/leds/white/brightness
 *   /sys/class/leds/white/device/grpfreq
 *   /sys/class/leds/white/device/grppwm
 *   /sys/class/leds/white/device/blink
 * 节点不存在 → 通知灯完全不工作。
 *
 * 这里把 red 镜像成一个 white LED，并在 charger 设备上补出
 * grpfreq / grppwm / blink 三个属性；blink=1 用定时器做闪烁（呼吸）。
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/leds.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>

static struct led_classdev *red_led;
static struct led_classdev white_led;
static struct platform_device *notify_parent;
static struct power_supply *batt_psy;
static struct power_supply *usb_psy;
static struct notifier_block psy_nb;

static bool blinking;
static bool charge_breathing;	/* 内核按充电状态强制呼吸中 */
static bool blink_phase_on;
static unsigned int blink_freq_ms = 1000;
static unsigned int blink_duty = 50;

static struct timer_list blink_timer;
static struct work_struct blink_work;
static struct work_struct charge_work;
static struct delayed_work notify_setup_work;
static int setup_retries;

static void red_set(enum led_brightness value)
{
	if (!red_led)
		return;
	if (red_led->brightness_set)
		red_led->brightness_set(red_led, value);
	else if (red_led->brightness_set_blocking)
		red_led->brightness_set_blocking(red_led, value);
}

static void white_brightness_set(struct led_classdev *cdev,
				 enum led_brightness value)
{
	pr_info("oppo_notify: white brightness <- %d\n", value);
	/*
	 * 充电呼吸由内核自己驱动。框架/HAL 在电池状态变化后还会补一次
	 * "灭灯"(brightness=0 / blink=0)，实测 62ms 后就把刚点亮的呼吸
	 * 掐掉。充电期间忽略这类灭灯请求；拔掉后 charge_breathing 清零，
	 * HAL 恢复正常控制。
	 */
	if (charge_breathing && value == LED_OFF)
		return;
	red_set(value);
}

static void blink_work_fn(struct work_struct *w)
{
	/* 定时器内部的明暗切换不走 HAL 入口，避免被上面的忽略逻辑吃掉 */
	red_set(blink_phase_on ? white_led.max_brightness : LED_OFF);
}

static void blink_timer_fn(struct timer_list *t)
{
	unsigned int on_ms;

	if (!blinking)
		return;

	blink_phase_on = !blink_phase_on;
	schedule_work(&blink_work);

	on_ms = (blink_freq_ms * blink_duty) / 100;
	if (on_ms < 20)
		on_ms = 20;
	if (on_ms > blink_freq_ms - 20)
		on_ms = blink_freq_ms - 20;

	mod_timer(&blink_timer, jiffies + msecs_to_jiffies(
			(blink_phase_on ? on_ms : blink_freq_ms - on_ms)));
}

static ssize_t blink_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return scnprintf(buf, 32, "%d\n", blinking ? 1 : 0);
}

static ssize_t blink_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	long val;

	if (kstrtol(buf, 0, &val))
		return -EINVAL;

	if (val) {
		if (!blinking) {
			blinking = true;
			blink_phase_on = true;
			white_brightness_set(&white_led, white_led.max_brightness);
			mod_timer(&blink_timer, jiffies + msecs_to_jiffies(200));
		}
	} else {
		if (charge_breathing) {
			pr_info("oppo_notify: ignore HAL blink off while charging\n");
			return count;
		}
		blinking = false;
		del_timer_sync(&blink_timer);
		white_brightness_set(&white_led, LED_OFF);
	}
	pr_info("oppo_notify: blink <- %ld\n", val);
	return count;
}

static ssize_t grpfreq_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, 32, "%u\n", blink_freq_ms);
}

static ssize_t grpfreq_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;
	if (val < 100)
		val = 100;
	if (val > 10000)
		val = 10000;
	blink_freq_ms = val;
	pr_info("oppo_notify: grpfreq <- %u\n", val);
	if (blinking)
		mod_timer(&blink_timer, jiffies + msecs_to_jiffies(10));
	return count;
}

static ssize_t grppwm_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, 32, "%u\n", blink_duty);
}

static ssize_t grppwm_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;
	if (val > 100)
		val = 100;
	blink_duty = val;
	pr_info("oppo_notify: grppwm <- %u (duty %u%%)\n",
		blink_duty, val);
	return count;
}

static DEVICE_ATTR_RW(blink);
static DEVICE_ATTR_RW(grpfreq);
static DEVICE_ATTR_RW(grppwm);

struct notify_lookup {
	const char *name;
	struct led_classdev *found;
};

static int notify_match_child(struct device *dev, void *data)
{
	struct notify_lookup *lk = data;
	struct led_classdev *cdev = dev_get_drvdata(dev);

	if (!cdev || !cdev->name)
		return 0;
	if (strcmp(cdev->name, lk->name))
		return 0;
	lk->found = cdev;
	return 1;
}

static void charging_led_update(void)
{
	union power_supply_propval val = { 0 };
	int online = 0, capacity = -1;

	if (!red_led)
		return;

	/*
	 * 这台 4.19 移植的 battery psy 在插 USB 时仍报 Discharging
	 * (POWER_SUPPLY_STATUS 不更新、current_now 为负)，用 STATUS 判断
	 * 充电永远不成立，所以充电呼吸从不启动。改用 usb psy 的 ONLINE
	 * 加 battery 的 CAPACITY：
	 *   online=1, cap<100  -> 充电中，呼吸 (1.2s)
	 *   online=1, cap>=100 -> 充满，常亮
	 *   online=0           -> 未充电，灭灯并交还 HAL 控制
	 * usb psy 可能晚于本驱动注册，拿不到就每次回调再试。
	 */
	if (!usb_psy)
		usb_psy = power_supply_get_by_name("usb");
	if (usb_psy &&
	    !power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_ONLINE, &val))
		online = val.intval;
	if (!batt_psy)
		batt_psy = power_supply_get_by_name("battery");
	if (batt_psy &&
	    !power_supply_get_property(batt_psy, POWER_SUPPLY_PROP_CAPACITY,
				       &val))
		capacity = val.intval;

	if (!online) {
		charge_breathing = false;
		blinking = false;
		del_timer_sync(&blink_timer);
		white_brightness_set(&white_led, LED_OFF);
	} else if (capacity >= 100) {
		charge_breathing = true;
		blinking = false;
		del_timer_sync(&blink_timer);
		white_brightness_set(&white_led, white_led.max_brightness);
	} else {
		charge_breathing = true;
		blink_freq_ms = 1200;
		blink_duty = 50;
		if (!blinking) {
			blinking = true;
			blink_phase_on = true;
			white_brightness_set(&white_led,
					     white_led.max_brightness);
			mod_timer(&blink_timer,
				  jiffies + msecs_to_jiffies(600));
		}
	}

	pr_info("oppo_notify: chg online=%d cap=%d -> %s\n", online, capacity,
		blinking ? "breathing" :
		(charge_breathing ? "steady on" : "off"));
}

static void charge_work_fn(struct work_struct *w)
{
	charging_led_update();
}

static int psy_notify(struct notifier_block *nb, unsigned long event, void *ptr)
{
	/* power_supply notifier 可能在原子上下文；真正的 LED 操作放 work 里 */
	schedule_work(&charge_work);
	return NOTIFY_OK;
}

static void notify_setup_work_fn(struct work_struct *work)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct notify_lookup lk = { .name = "red" };
	int rc;

	np = of_find_compatible_node(NULL, NULL, "qcom,qpnp-smbcharger");
	if (!np)
		goto retry;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		goto retry;

	device_for_each_child(&pdev->dev, &lk, notify_match_child);
	if (!lk.found) {
		put_device(&pdev->dev);
		goto retry;
	}

	red_led = lk.found;
	notify_parent = pdev;

	memset(&white_led, 0, sizeof(white_led));
	white_led.name = "white";
	white_led.max_brightness = red_led->max_brightness ? : 255;
	white_led.brightness_set = white_brightness_set;
	white_led.brightness = LED_OFF;

	rc = led_classdev_register(&pdev->dev, &white_led);
	if (rc) {
		pr_err("oppo_notify: register white led failed rc=%d\n", rc);
		return;
	}
	device_create_file(&pdev->dev, &dev_attr_blink);
	device_create_file(&pdev->dev, &dev_attr_grpfreq);
	device_create_file(&pdev->dev, &dev_attr_grppwm);

	timer_setup(&blink_timer, blink_timer_fn, 0);
	INIT_WORK(&blink_work, blink_work_fn);
	INIT_WORK(&charge_work, charge_work_fn);

	batt_psy = power_supply_get_by_name("battery");
	usb_psy = power_supply_get_by_name("usb");
	psy_nb.notifier_call = psy_notify;
	power_supply_reg_notifier(&psy_nb);
	charging_led_update();

	pr_info("oppo_notify: white LED ready (mirrors red, max %d)\n",
		white_led.max_brightness);
	return;

retry:
	if (++setup_retries > 60)
		return;
	schedule_delayed_work(&notify_setup_work, msecs_to_jiffies(500));
}

static int __init oppo_notify_init(void)
{
	INIT_DELAYED_WORK(&notify_setup_work, notify_setup_work_fn);
	schedule_delayed_work(&notify_setup_work, msecs_to_jiffies(1000));
	return 0;
}

static void __exit oppo_notify_exit(void)
{
	cancel_delayed_work_sync(&notify_setup_work);
	cancel_work_sync(&charge_work);
	cancel_work_sync(&blink_work);
	blinking = false;
	del_timer_sync(&blink_timer);
	if (batt_psy)
		power_supply_unreg_notifier(&psy_nb);
	if (notify_parent) {
		device_remove_file(&notify_parent->dev, &dev_attr_blink);
		device_remove_file(&notify_parent->dev, &dev_attr_grpfreq);
		device_remove_file(&notify_parent->dev, &dev_attr_grppwm);
		led_classdev_unregister(&white_led);
	}
}

module_init(oppo_notify_init);
module_exit(oppo_notify_exit);
MODULE_DESCRIPTION("OPPO A57 notification (white) LED bridge");
MODULE_LICENSE("GPL v2");
