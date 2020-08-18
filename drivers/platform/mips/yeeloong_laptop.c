/* SPDX-License-Identifier: GPL */
/*
 * Driver for YeeLoong laptop extras
 *
 *  Copyright (C) 2017 Jiaxun Yang <jiaxun.yang@flygoat.com>
 *  Copyright (C) 2009 Lemote Inc.
 *  Author: Wu Zhangjin <wuzhangjin@gmail.com>, Liu Junliang <liujl@lemote.com>
 *  Fixes: Petr Pisar <petr.pisar@atlas.cz>, 2012, 2013, 2014, 2015.
 *
 */

#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/backlight.h>	/* for backlight subdriver */
#include <linux/fb.h>
#include <linux/hwmon.h>	/* for hwmon subdriver */
#include <linux/hwmon-sysfs.h>
#include <linux/kernel.h>   /* for clamp_val() */
#include <linux/input.h>	/* for hotkey subdriver */
#include <linux/input/sparse-keymap.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/power_supply.h>	/* for AC & Battery subdriver */
#include <linux/module.h>   /* For MODULE_DEVICE_TABLE() */

#include <asm/bootinfo.h>

#include <cs5536/cs5536.h>

#include <asm/setup.h>		/* for arcs_cmdline */
#include "../../../arch/mips/loongson64/lemote-2f/ec_kb3310b.h"

#define ON	1
#define OFF	0

/* backlight subdriver */
#define MAX_BRIGHTNESS	8

static int ec_version_before(char *version)
{
	return (strncasecmp(ec_kb3310b_ver, version, 64) < 0);
}

static int yeeloong_set_brightness(struct backlight_device *bd)
{
	unsigned int level, current_level;
	static unsigned int old_level;

	level = (bd->props.fb_blank == FB_BLANK_UNBLANK &&
		 bd->props.power == FB_BLANK_UNBLANK) ?
	    bd->props.brightness : 0;

	level = clamp_val(level, 0, MAX_BRIGHTNESS);

	/* Avoid to modify the brightness when EC is tuning it */
	if (old_level != level) {
		current_level = ec_read(REG_DISPLAY_BRIGHTNESS);
		if (old_level == current_level)
			ec_write(REG_DISPLAY_BRIGHTNESS, level);
		old_level = level;
	}

	return 0;
}

static int yeeloong_get_brightness(struct backlight_device *bd)
{
	return ec_read(REG_DISPLAY_BRIGHTNESS);
}

const struct backlight_ops backlight_ops = {
	.get_brightness = yeeloong_get_brightness,
	.update_status = yeeloong_set_brightness,
};

static struct backlight_device *yeeloong_backlight_dev;

static int yeeloong_backlight_init(void)
{
	int ret;
	struct backlight_properties props;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = MAX_BRIGHTNESS;
	yeeloong_backlight_dev = backlight_device_register("backlight0", NULL,
			NULL, &backlight_ops, &props);

	if (IS_ERR(yeeloong_backlight_dev)) {
		ret = PTR_ERR(yeeloong_backlight_dev);
		yeeloong_backlight_dev = NULL;
		return ret;
	}

	yeeloong_backlight_dev->props.brightness =
		yeeloong_get_brightness(yeeloong_backlight_dev);
	backlight_update_status(yeeloong_backlight_dev);

	return 0;
}

static void yeeloong_backlight_exit(void)
{
	if (yeeloong_backlight_dev) {
		backlight_device_unregister(yeeloong_backlight_dev);
		yeeloong_backlight_dev = NULL;
	}
}

/* AC & Battery subdriver */

static struct power_supply *yeeloong_ac, *yeeloong_bat;

#define RET (val->intval)

static inline bool is_ac_in(void)
{
	return !!(ec_read(REG_BAT_POWER) & BIT_BAT_POWER_ACIN);
}

static int yeeloong_get_ac_props(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		RET = is_ac_in();
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property yeeloong_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc yeeloong_ac_desc = {
	.name = "yeeloong-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = yeeloong_ac_props,
	.num_properties = ARRAY_SIZE(yeeloong_ac_props),
	.get_property = yeeloong_get_ac_props,
};

#define BAT_CAP_CRITICAL 5
#define BAT_CAP_HIGH     95

#define get_bat_info(type) \
	((ec_read(REG_BAT_##type##_HIGH) << 8) | \
	 (ec_read(REG_BAT_##type##_LOW)))

static inline bool is_bat_in(void)
{
	return !!(ec_read(REG_BAT_STATUS) & BIT_BAT_STATUS_IN);
}

static inline int get_bat_status(void)
{
	return ec_read(REG_BAT_STATUS);
}

static int get_battery_temp(void)
{
	int value;

	value = get_bat_info(TEMPERATURE);

	return value * 1000;
}

static int get_battery_current(void)
{
	s16 value;

	value = get_bat_info(CURRENT);

	return -value;
}

static int get_battery_voltage(void)
{
	int value;

	value = get_bat_info(VOLTAGE);

	return value;
}

static inline char *get_manufacturer(void)
{
	return (ec_read(REG_BAT_VENDOR) == FLAG_BAT_VENDOR_SANYO) ? "SANYO" :
		"SIMPLO";
}

static int yeeloong_get_bat_props(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	switch (psp) {
	/* Fixed information */
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		/* mV -> µV */
		RET = get_bat_info(DESIGN_VOL) * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		/* mAh->µAh */
		RET = get_bat_info(DESIGN_CAP) * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		/* µAh */
		RET = get_bat_info(FULLCHG_CAP) * 1000;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = get_manufacturer();
		break;
	/* Dynamic information */
	case POWER_SUPPLY_PROP_PRESENT:
		RET = is_bat_in();
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* mA -> µA */
		RET = is_bat_in() ? get_battery_current() * 1000 : 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* mV -> µV */
		RET = is_bat_in() ? get_battery_voltage() * 1000 : 0;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		/* Celcius */
		RET = is_bat_in() ? get_battery_temp() : 0;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		RET = is_bat_in() ? get_bat_info(RELATIVE_CAP) : 0;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		{
		int status;

		if (!is_bat_in()) {
			RET = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
			break;
		}

		status = get_bat_status();
		RET = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

		if (unlikely(status & BIT_BAT_STATUS_DESTROY)) {
			RET = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
			break;
		}

		if (status & BIT_BAT_STATUS_LOW)
			RET = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else if (status & BIT_BAT_STATUS_FULL)
			RET = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else {
			int curr_cap;

			curr_cap = get_bat_info(RELATIVE_CAP);

			if (curr_cap >= BAT_CAP_HIGH)
				RET = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
			else if (curr_cap <= BAT_CAP_CRITICAL)
				RET = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		}

		} break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		/* seconds */
		RET = is_bat_in() ?
			(get_bat_info(RELATIVE_CAP) - 3) * 54 + 142
			: 0;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		{
			int charge = ec_read(REG_BAT_CHARGE);

			if (charge & FLAG_BAT_CHARGE_DISCHARGE)
				RET = POWER_SUPPLY_STATUS_DISCHARGING;
			else if (charge & FLAG_BAT_CHARGE_CHARGE)
				RET = POWER_SUPPLY_STATUS_CHARGING;
			else
				RET = POWER_SUPPLY_STATUS_NOT_CHARGING;
		}
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		{
			int status;

			if (!is_bat_in()) {
				RET = POWER_SUPPLY_HEALTH_UNKNOWN;
				break;
			}

			status = get_bat_status();

			RET = POWER_SUPPLY_HEALTH_GOOD;
			if (status & (BIT_BAT_STATUS_DESTROY |
						BIT_BAT_STATUS_LOW))
				RET = POWER_SUPPLY_HEALTH_DEAD;
			if (ec_read(REG_BAT_CHARGE_STATUS) &
					BIT_BAT_CHARGE_STATUS_OVERTEMP)
				RET = POWER_SUPPLY_HEALTH_OVERHEAT;
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:	/* 1/100(%)*1000 µAh */
		RET = get_bat_info(RELATIVE_CAP) *
			get_bat_info(FULLCHG_CAP) * 10;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
#undef RET

static enum power_supply_property yeeloong_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static const struct power_supply_desc yeeloong_bat_desc = {
	.name = "yeeloongbattery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = yeeloong_bat_props,
	.num_properties = ARRAY_SIZE(yeeloong_bat_props),
	.get_property = yeeloong_get_bat_props,
};

static int ac_bat_initialized;

static int yeeloong_bat_init(void)
{
	yeeloong_ac = power_supply_register(NULL, &yeeloong_ac_desc, NULL);
	if (IS_ERR(yeeloong_ac))
		return PTR_ERR(yeeloong_ac);
	yeeloong_bat = power_supply_register(NULL, &yeeloong_bat_desc, NULL);
	if (IS_ERR(yeeloong_bat)) {
		power_supply_unregister(yeeloong_ac);
		return PTR_ERR(yeeloong_bat);
	}
	ac_bat_initialized = 1;

	return 0;
}

static void yeeloong_bat_exit(void)
{
	if (ac_bat_initialized) {
		ac_bat_initialized = 0;

		power_supply_unregister(yeeloong_ac);
		power_supply_unregister(yeeloong_bat);
	}
}
/* hwmon subdriver */

#define MIN_FAN_SPEED 0
#define MAX_FAN_SPEED 3

static int get_fan_pwm_enable(void)
{
	int level, mode;

	level = ec_read(REG_FAN_SPEED_LEVEL);
	mode = ec_read(REG_FAN_AUTO_MAN_SWITCH);

	if (level == MAX_FAN_SPEED && mode == BIT_FAN_MANUAL)
		mode = 0;
	else if (mode == BIT_FAN_MANUAL)
		mode = 1;
	else
		mode = 2;

	return mode;
}

static void set_fan_pwm_enable(int mode)
{
	switch (mode) {
	case 0:
		/* fullspeed */
		ec_write(REG_FAN_AUTO_MAN_SWITCH, BIT_FAN_MANUAL);
		ec_write(REG_FAN_SPEED_LEVEL, MAX_FAN_SPEED);
		break;
	case 1:
		ec_write(REG_FAN_AUTO_MAN_SWITCH, BIT_FAN_MANUAL);
		break;
	case 2:
		ec_write(REG_FAN_AUTO_MAN_SWITCH, BIT_FAN_AUTO);
		break;
	default:
		break;
	}
}

static int get_fan_pwm(void)
{
	return ec_read(REG_FAN_SPEED_LEVEL);
}

static void set_fan_pwm(int value)
{
	int mode;

	mode = ec_read(REG_FAN_AUTO_MAN_SWITCH);
	if (mode != BIT_FAN_MANUAL)
		return;

	value = clamp_val(value, 0, 3);

	/* We must ensure the fan is on */
	if (value > 0)
		ec_write(REG_FAN_CONTROL, ON);

	ec_write(REG_FAN_SPEED_LEVEL, value);
}

static int get_fan_rpm(void)
{
	int value;

	value = FAN_SPEED_DIVIDER /
	    (((ec_read(REG_FAN_SPEED_HIGH) & 0x0f) << 8) |
	     ec_read(REG_FAN_SPEED_LOW));

	return value;
}

static int get_cpu_temp(void)
{
	s8 value;

	value = ec_read(REG_TEMPERATURE_VALUE);

	return value * 1000;
}

static int get_cpu_temp_max(void)
{
	return 60 * 1000;
}

static int get_battery_temp_alarm(void)
{
	int status;

	status = (ec_read(REG_BAT_CHARGE_STATUS) &
			BIT_BAT_CHARGE_STATUS_OVERTEMP);

	return !!status;
}

static ssize_t store_sys_hwmon(void (*set) (int), const char *buf, size_t count)
{
	int ret;
	unsigned long value;

	if (!count)
		return 0;

	ret = kstrtoul(buf, 10, &value);
	if (ret)
		return ret;

	set(value);

	return count;
}

static ssize_t show_sys_hwmon(int (*get) (void), char *buf)
{
	return sprintf(buf, "%d\n", get());
}

#define CREATE_SENSOR_ATTR(_name, _mode, _set, _get)		\
	static ssize_t show_##_name(struct device *dev,			\
				    struct device_attribute *attr,	\
				    char *buf)				\
	{								\
		return show_sys_hwmon(_set, buf);			\
	}								\
	static ssize_t store_##_name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     const char *buf, size_t count)	\
	{								\
		return store_sys_hwmon(_get, buf, count);		\
	}								\
	static SENSOR_DEVICE_ATTR(_name, _mode, show_##_name, store_##_name, 0)

CREATE_SENSOR_ATTR(fan1_input, 0444, get_fan_rpm, NULL);
CREATE_SENSOR_ATTR(pwm1, 0444 | 0644, get_fan_pwm, set_fan_pwm);
CREATE_SENSOR_ATTR(pwm1_enable, 0444 | 0644, get_fan_pwm_enable,
		set_fan_pwm_enable);
CREATE_SENSOR_ATTR(temp1_input, 0444, get_cpu_temp, NULL);
CREATE_SENSOR_ATTR(temp1_max, 0444, get_cpu_temp_max, NULL);
CREATE_SENSOR_ATTR(temp2_input, 0444, get_battery_temp, NULL);
CREATE_SENSOR_ATTR(temp2_max_alarm, 0444, get_battery_temp_alarm, NULL);
CREATE_SENSOR_ATTR(curr1_input, 0444, get_battery_current, NULL);
CREATE_SENSOR_ATTR(in1_input, 0444, get_battery_voltage, NULL);

static ssize_t
show_name(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "yeeloong\n");
}

static SENSOR_DEVICE_ATTR(name, 0444, show_name, NULL, 0);

static struct attribute *hwmon_attributes[] = {
	&sensor_dev_attr_pwm1.dev_attr.attr,
	&sensor_dev_attr_pwm1_enable.dev_attr.attr,
	&sensor_dev_attr_fan1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_max.dev_attr.attr,
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_temp2_max_alarm.dev_attr.attr,
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_name.dev_attr.attr,
	NULL
};

static struct attribute_group hwmon_attribute_group = {
	.attrs = hwmon_attributes
};

static struct device *yeeloong_hwmon_dev;

static int yeeloong_hwmon_init(void)
{
	int ret;

	yeeloong_hwmon_dev = hwmon_device_register(NULL);
	if (IS_ERR(yeeloong_hwmon_dev)) {
		pr_err("Fail to register yeeloong hwmon device\n");
		yeeloong_hwmon_dev = NULL;
		return PTR_ERR(yeeloong_hwmon_dev);
	}
	ret = sysfs_create_group(&yeeloong_hwmon_dev->kobj,
				 &hwmon_attribute_group);
	if (ret) {
		hwmon_device_unregister(yeeloong_hwmon_dev);
		yeeloong_hwmon_dev = NULL;
		return ret;
	}
	/* ensure fan is set to auto mode */
	set_fan_pwm_enable(2);

	return 0;
}

static void yeeloong_hwmon_exit(void)
{
	if (yeeloong_hwmon_dev) {
		sysfs_remove_group(&yeeloong_hwmon_dev->kobj,
				   &hwmon_attribute_group);
		hwmon_device_unregister(yeeloong_hwmon_dev);
		yeeloong_hwmon_dev = NULL;
	}
}

/* video output controller */


#define LCD	0
#define CRT	1

static void display_vo_set(int display, int on)
{
	int addr;
	unsigned long value;

	addr = (display == LCD) ? 0x31 : 0x21;

	outb(addr, 0x3c4);
	value = inb(0x3c5);

	if (display == LCD)
		value |= (on ? 0x03 : 0x02);
	else {
		if (on)
			clear_bit(7, &value);
		else
			set_bit(7, &value);
	}

	outb(addr, 0x3c4);
	outb(value, 0x3c5);
}



/* hotkey subdriver */

static struct input_dev *yeeloong_hotkey_dev;

static const struct key_entry yeeloong_keymap[] = {
	{KE_SW, EVENT_LID, {SW_LID} },
	/* Fn + ESC */
	{KE_KEY, EVENT_CAMERA, {KEY_CAMERA} },
	/* Fn + F1 */
	{KE_KEY, EVENT_SLEEP, {KEY_SLEEP} },
	/* Fn + F2 */
	{KE_KEY, EVENT_DISPLAY_TOGGLE, {KEY_DISPLAYTOGGLE} },
	/* Fn + F3 */
	{KE_KEY, EVENT_SWITCHVIDEOMODE, {KEY_SWITCHVIDEOMODE} },
	/* Fn + F4 */
	{KE_KEY, EVENT_AUDIO_MUTE, {KEY_MUTE} },
	/* Fn + F5 */
	{KE_KEY, EVENT_WLAN, {KEY_WLAN} },
	/* Fn + up */
	{KE_KEY, EVENT_DISPLAY_BRIGHTNESS, {KEY_BRIGHTNESSUP} },
	/* Fn + down */
	{KE_KEY, EVENT_DISPLAY_BRIGHTNESS, {KEY_BRIGHTNESSDOWN} },
	/* Fn + right */
	{KE_KEY, EVENT_AUDIO_VOLUME, {KEY_VOLUMEUP} },
	/* Fn + left */
	{KE_KEY, EVENT_AUDIO_VOLUME, {KEY_VOLUMEDOWN} },
	{KE_END, 0} };

static struct key_entry *get_event_key_entry(int event, int status)
{
	struct key_entry *ke;
	static int old_brightness_status = -1;
	static int old_volume_status = -1;

	ke = sparse_keymap_entry_from_scancode(yeeloong_hotkey_dev, event);
	if (!ke)
		return NULL;

	switch (event) {
	case EVENT_DISPLAY_BRIGHTNESS:
		/* current status > old one, means up */
		if ((status == 0) || (status < old_brightness_status))
			ke++;
		old_brightness_status = status;
		break;
	case EVENT_AUDIO_VOLUME:
		if ((status == 0) || (status < old_volume_status))
			ke++;
		old_volume_status = status;
		break;
	default:
		break;
	}

	return ke;
}

static int report_lid_switch(int status)
{
	input_report_switch(yeeloong_hotkey_dev, SW_LID, !status);
	input_sync(yeeloong_hotkey_dev);

	return status;
}

static void yeeloong_vo_set(int lcd_status, int crt_status)
{
	display_vo_set(LCD, lcd_status);
	display_vo_set(CRT, crt_status);
}

static int crt_detect_handler(int status)
{
	if (status)
		yeeloong_vo_set(ON, ON);
	else
		yeeloong_vo_set(ON, OFF);

	return status;
}

static int displaytoggle_handler(int status)
{
	/* EC(>=PQ1D26) does this job for us, we can not do it again,
	 * otherwise, the brightness will not resume to the normal level!
	 */
	if (ec_version_before("EC_VER=PQ1D26"))
		display_vo_set(LCD, status);

	return status;
}

static int switchvideomode_handler(int status)
{
	static int video_output_status;

	/* Only enable switch video output button
	 * when CRT is connected
	 */
	if (ec_read(REG_CRT_DETECT) == OFF)
		return 0;
	/* 0. no CRT connected: LCD on, CRT off
	 * 1. BOTH on
	 * 2. LCD off, CRT on
	 * 3. BOTH off
	 * 4. LCD on, CRT off
	 */
	video_output_status++;
	if (video_output_status > 4)
		video_output_status = 1;

	switch (video_output_status) {
	case 1:
		yeeloong_vo_set(ON, ON);
		break;
	case 2:
		yeeloong_vo_set(OFF, ON);
		break;
	case 3:
		yeeloong_vo_set(OFF, OFF);
		break;
	case 4:
		yeeloong_vo_set(ON, OFF);
		break;
	default:
		/* Ensure LCD is on */
		display_vo_set(LCD, ON);
		break;
	}
	return video_output_status;
}

static int camera_handler(int status)
{
	int value;

	value = ec_read(REG_CAMERA_CONTROL);
	ec_write(REG_CAMERA_CONTROL, value | (1 << 1));

	return status;
}

static int usb2_handler(int status)
{
	pr_emerg("USB2 Over Current occurred\n");

	return status;
}

static int usb0_handler(int status)
{
	pr_emerg("USB0 Over Current occurred\n");

	return status;
}

static int ac_bat_handler(int status)
{
	if (ac_bat_initialized) {
		power_supply_changed(yeeloong_ac);
		power_supply_changed(yeeloong_bat);
	}
	return status;
}

static void do_event_action(int event)
{
	sci_handler handler;
	int reg, status;
	struct key_entry *ke;

	reg = 0;
	handler = NULL;
	status = 0;

	switch (event) {
	case EVENT_LID:
		reg = REG_LID_DETECT;
		break;
	case EVENT_SWITCHVIDEOMODE:
		handler = switchvideomode_handler;
		break;
	case EVENT_CRT_DETECT:
		reg = REG_CRT_DETECT;
		handler = crt_detect_handler;
		break;
	case EVENT_CAMERA:
		reg = REG_CAMERA_STATUS;
		handler = camera_handler;
		break;
	case EVENT_USB_OC2:
		reg = REG_USB2_FLAG;
		handler = usb2_handler;
		break;
	case EVENT_USB_OC0:
		reg = REG_USB0_FLAG;
		handler = usb0_handler;
		break;
	case EVENT_DISPLAY_TOGGLE:
		reg = REG_DISPLAY_LCD;
		handler = displaytoggle_handler;
		break;
	case EVENT_AUDIO_MUTE:
		reg = REG_AUDIO_MUTE;
		break;
	case EVENT_DISPLAY_BRIGHTNESS:
		reg = REG_DISPLAY_BRIGHTNESS;
		break;
	case EVENT_AUDIO_VOLUME:
		reg = REG_AUDIO_VOLUME;
		break;
	case EVENT_AC_BAT:
		handler = ac_bat_handler;
		break;
	default:
		break;
	}

	if (reg != 0)
		status = ec_read(reg);

	if (handler != NULL)
		status = handler(status);

	pr_debug("%s: event: %d status: %d\n", __func__, event, status);

	/* Report current key to user-space */
	ke = get_event_key_entry(event, status);
	if (ke) {
		if (ke->keycode == SW_LID)
			report_lid_switch(status);
		else
			sparse_keymap_report_entry(yeeloong_hotkey_dev, ke, 1,
					true);
	}
}

/*
 * SCI(system control interrupt) main interrupt routine
 *
 * We will do the query and get event number together so the interrupt routine
 * should be longer than 120us now at least 3ms elpase for it.
 */
static irqreturn_t sci_irq_handler(int irq, void *dev_id)
{
	int ret, event;

	if (irq != SCI_IRQ_NUM)
		return IRQ_NONE;

	/* Query the event number */
	ret = ec_query_event_num();
	if (ret < 0)
		return IRQ_NONE;

	event = ec_get_event_num();
	if (event < EVENT_START || event > EVENT_END)
		return IRQ_NONE;

	/* Execute corresponding actions */
	do_event_action(event);

	return IRQ_HANDLED;
}

/*
 * Config and init some msr and gpio register properly.
 */
static int sci_irq_init(void)
{
	u32 hi, lo;
	u32 gpio_base;
	unsigned long flags;
	int ret;

	/* Get gpio base */
	_rdmsr(DIVIL_MSR_REG(DIVIL_LBAR_GPIO), &hi, &lo);
	gpio_base = lo & 0xff00;

	/* Filter the former kb3310 interrupt for security */
	ret = ec_query_event_num();
	if (ret)
		return ret;

	/* For filtering next number interrupt */
	mdelay(10000);

	/* Set gpio native registers and msrs for GPIO27 SCI EVENT PIN
	 * gpio :
	 *      input, pull-up, no-invert, event-count and value 0,
	 *      no-filter, no edge mode
	 *      gpio27 map to Virtual gpio0
	 * msr :
	 *      no primary and lpc
	 *      Unrestricted Z input to IG10 from Virtual gpio 0.
	 */
	local_irq_save(flags);
	_rdmsr(0x80000024, &hi, &lo);
	lo &= ~(1 << 10);
	_wrmsr(0x80000024, hi, lo);
	_rdmsr(0x80000025, &hi, &lo);
	lo &= ~(1 << 10);
	_wrmsr(0x80000025, hi, lo);
	_rdmsr(0x80000023, &hi, &lo);
	lo |= (0x0a << 0);
	_wrmsr(0x80000023, hi, lo);
	local_irq_restore(flags);

	/* Set gpio27 as sci interrupt
	 *
	 * input, pull-up, no-fliter, no-negedge, invert
	 * the sci event is just about 120us
	 */
	asm(".set noreorder\n");
	/*  input enable */
	outl(0x00000800, (gpio_base | 0xA0));
	/*  revert the input */
	outl(0x00000800, (gpio_base | 0xA4));
	/*  event-int enable */
	outl(0x00000800, (gpio_base | 0xB8));
	asm(".set reorder\n");

	return 0;
}

static void wlan_set(int status)
{
	/* 
	 * Jiaxun: Deal with users complain about WLAN not being enabled
	 * by default.
	 */
	if (status)
		ec_write(REG_WLAN, BIT_WLAN_ON);
	else
		ec_write(REG_WLAN, BIT_WLAN_OFF);
}

static int yeeloong_hotkey_init(void)
{
	int ret;

	wlan_set(ON);

	ret = sci_irq_init();
	if (ret)
		return -EFAULT;

	ret = request_threaded_irq(SCI_IRQ_NUM, NULL, &sci_irq_handler,
			IRQF_ONESHOT, "sci", NULL);
	if (ret)
		return -EFAULT;

	yeeloong_hotkey_dev = input_allocate_device();

	if (!yeeloong_hotkey_dev) {
		free_irq(SCI_IRQ_NUM, NULL);
		return -ENOMEM;
	}

	yeeloong_hotkey_dev->name = "HotKeys";
	yeeloong_hotkey_dev->phys = "button/input0";
	yeeloong_hotkey_dev->id.bustype = BUS_HOST;
	yeeloong_hotkey_dev->dev.parent = NULL;

	ret = sparse_keymap_setup(yeeloong_hotkey_dev, yeeloong_keymap, NULL);
	if (ret) {
		pr_err("Fail to setup input device keymap\n");
		input_free_device(yeeloong_hotkey_dev);
		return ret;
	}

	ret = input_register_device(yeeloong_hotkey_dev);
	if (ret) {
		input_free_device(yeeloong_hotkey_dev);
		return ret;
	}

	/* Update the current status of LID */
	report_lid_switch(ON);

#ifdef CONFIG_PM
	/* Install the real yeeloong_report_lid_status for pm.c */
	yeeloong_report_lid_status = report_lid_switch;
#endif

	return 0;
}

static void yeeloong_hotkey_exit(void)
{
	/* Free irq */
	free_irq(SCI_IRQ_NUM, NULL);

#ifdef CONFIG_PM
	/* Uninstall yeeloong_report_lid_status for pm.c */
	if (yeeloong_report_lid_status == report_lid_switch)
		yeeloong_report_lid_status = NULL;
#endif

	if (yeeloong_hotkey_dev) {
		input_unregister_device(yeeloong_hotkey_dev);
		yeeloong_hotkey_dev = NULL;
	}
}

#ifdef CONFIG_PM
static void usb_ports_set(int status)
{
	status = !!status;

	ec_write(REG_USB0_FLAG, status);
	ec_write(REG_USB1_FLAG, status);
	ec_write(REG_USB2_FLAG, status);
}

static int yeeloong_suspend(struct device *dev)

{
	if (ec_version_before("EC_VER=PQ1D27"))
		display_vo_set(LCD, OFF);
	display_vo_set(CRT, OFF);
	usb_ports_set(OFF);
	wlan_set(OFF);

	return 0;
}

static int yeeloong_resume(struct device *dev)
{
	int ret;

	if (ec_version_before("EC_VER=PQ1D27"))
		display_vo_set(LCD, ON);
	display_vo_set(CRT, ON);
	usb_ports_set(ON);
	wlan_set(ON);

	ret = sci_irq_init();
	if (ret)
		return -EFAULT;

	return 0;
}

static const SIMPLE_DEV_PM_OPS(yeeloong_pm_ops, yeeloong_suspend,
	yeeloong_resume);
#endif

static struct platform_device_id platform_device_ids[] = {
	{
		.name = "yeeloong_laptop",
	},
	{}
};

MODULE_DEVICE_TABLE(platform, platform_device_ids);

static struct platform_driver platform_driver = {
	.driver = {
		.name = "yeeloong_laptop",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &yeeloong_pm_ops,
#endif
	},
	.id_table = platform_device_ids,
};

static int __init yeeloong_init(void)
{
	int ret;

	if (mips_machtype != MACH_LEMOTE_YL2F89) {
		pr_err("YeeLoong: Unsupported system.\n");
		return -ENODEV;
	}

	pr_info("Load YeeLoong Laptop Platform Specific Driver.\n");

	/* Register platform stuff */
	ret = platform_driver_register(&platform_driver);
	if (ret) {
		pr_err("Fail to register yeeloong platform driver.\n");
		return ret;
	}

	ret = yeeloong_backlight_init();
	if (ret) {
		pr_err("Fail to register yeeloong backlight driver.\n");
		yeeloong_backlight_exit();
		return ret;
	}

	ret = yeeloong_bat_init();
	if (ret) {
		pr_err("Fail to register yeeloong battery driver.\n");
		yeeloong_bat_exit();
		return ret;
	}

	ret = yeeloong_hwmon_init();
	if (ret) {
		pr_err("Fail to register yeeloong hwmon driver.\n");
		yeeloong_hwmon_exit();
		return ret;
	}

	ret = yeeloong_hotkey_init();
	if (ret) {
		pr_err("Fail to register yeeloong hotkey driver.\n");
		yeeloong_hotkey_exit();
		return ret;
	}

	return 0;
}

static void __exit yeeloong_exit(void)
{
	yeeloong_hotkey_exit();
	yeeloong_hwmon_exit();
	yeeloong_bat_exit();
	yeeloong_backlight_exit();
	platform_driver_unregister(&platform_driver);

	pr_info("Unload YeeLoong Platform Specific Driver.\n");
}

module_init(yeeloong_init);
module_exit(yeeloong_exit);

MODULE_AUTHOR("Wu Zhangjin <wuzhangjin@gmail.com>; Liu Junliang <liujl@lemote.com>");
MODULE_DESCRIPTION("YeeLoong laptop driver");
MODULE_LICENSE("GPL-2.0");
