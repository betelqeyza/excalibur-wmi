// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Casper Excalibur WMI driver
 *
 * Copyright (C) 2025, betelqeyza <avsarusta4422@hotmail.com>
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/wmi.h>

/*
	Both NLAK 001 and NLAI 001 uses same GUID

	0x91, 0x57, 0x4C, 0x64, 0xB0, 0xB7, 0x23, 0x41,  // .WLd..
	0xA9, 0x0B, 0xE9, 0x38, 0x76, 0xE0, 0xDA, 0xAD,  // ...8v...
*/
#define EXCALIBUR_WMI_GUID "644C5791-B7B0-4123-A90B-E93876E0DAAD"

#define EXCALIBUR_READ 0xFA00
#define EXCALIBUR_WRITE 0xFB00

#define EXCALIBUR_GET_HARDWAREINFO 0x0200
#define EXCALIBUR_SET_LED 0x0100
#define EXCALIBUR_POWERPLAN 0x0300
#define EXCALIBUR_SET_WINKEY 0x0200
#define EXCALIBUR_GET_BIOSINFO 0x0201
#define EXCALIBUR_MAX_ZONES 8

struct excalibur_wmi_args {
	u16 a0, a1;
	u32 a2, a3, a4, a5, a6;
};

struct excalibur_led {
	struct led_classdev cdev;
	struct wmi_device *wdev;
	u8 zone_id;
	u32 color;
	char name[64];
};

struct excalibur_wmi_priv {
	struct wmi_device *wdev;
	struct mutex lock;
	struct device *hwmon_dev;
	struct device *platform_dev;
	struct platform_device *pdev;
	struct excalibur_led leds[EXCALIBUR_MAX_ZONES];
	struct input_dev *input_dev;
	bool winkey_enabled;
};

/*
	This function is needed to call ACPI methods to control the excalibur
	laptop's LEDs, Windows Key and power settings.
*/
static int excalibur_set(struct wmi_device *wdev, u16 a1, u32 a2, u32 a3,
			 u32 a6)
{
	struct excalibur_wmi_priv *priv = dev_get_drvdata(&wdev->dev);
	struct excalibur_wmi_args wmi_args = { .a0 = EXCALIBUR_WRITE,
					       .a1 = a1,
					       .a2 = a2,
					       .a3 = a3,
					       .a6 = a6 };
	struct acpi_buffer input = {
		(acpi_size)sizeof(struct excalibur_wmi_args), &wmi_args
	};
	acpi_status ret;

	mutex_lock(&priv->lock);
	ret = wmidev_block_set(wdev, 0, &input);
	mutex_unlock(&priv->lock);

	if (ACPI_FAILURE(ret)) {
		dev_err(&wdev->dev, "WMI set failed: %s\n",
			acpi_format_exception(ret));
		return -EIO;
	}

	return 0;
}

static int excalibur_query(struct wmi_device *wdev, u16 a0, u16 a1,
			   struct excalibur_wmi_args *out)
{
	struct excalibur_wmi_priv *priv = dev_get_drvdata(&wdev->dev);
	struct excalibur_wmi_args in = { .a0 = a0, .a1 = a1 };
	struct acpi_buffer input = { (acpi_size)sizeof(in), &in };
	union acpi_object *obj;
	acpi_status ret;

	/* We're locking this because read/write is acting up */
	mutex_lock(&priv->lock);

	/* Send the requested a0/a1 as input via Set first */
	ret = wmidev_block_set(wdev, 0, &input);
	if (ACPI_FAILURE(ret)) {
		mutex_unlock(&priv->lock);
		return -EIO;
	}

	obj = wmidev_block_query(wdev, 0);
	mutex_unlock(&priv->lock);

	if (!obj)
		return -EIO;

	if (obj->type == ACPI_TYPE_BUFFER &&
	    obj->buffer.length >= sizeof(struct excalibur_wmi_args)) {
		memcpy(out, obj->buffer.pointer,
		       sizeof(struct excalibur_wmi_args));
		ret = 0;
	} else {
		ret = -EIO;
		dev_err(&wdev->dev, "Invalid WMI query response\n");
	}

	kfree(obj);
	return ret;
}

/* Model-specific board names */
enum excalibur_model_type {
	EXCALIBUR_MODEL_NLAI_001, /* G770 10. Generation */
	EXCALIBUR_MODEL_NLAK_001, /* G770 12. Generation */
	EXCALIBUR_MODEL_UNKNOWN
};

static enum excalibur_model_type detected_model = EXCALIBUR_MODEL_UNKNOWN;

enum excalibur_led_zone_nlai_001 {
	NLAI_001_LED1 = 0x01,
	NLAI_001_LED2 = 0x02,
	NLAI_001_LED3 = 0x03,
	NLAI_001_LED4 = 0x04,
	NLAI_001_LED5 = 0x05,
	NLAI_001_ALL_LEDS = 0x06,
	NLAI_001_AMBIENT = 0x07
};

enum excalibur_led_zone_nlak_001 {
	NLAK_001_LED1 = 0x05,
	NLAK_001_LED2 = 0x04,
	NLAK_001_LED3 = 0x03,
	NLAK_001_ALL_LEDS = 0x06
};

/* LED configuration structure per model */
struct excalibur_led_config {
	const char *name;
	u8 zone_count;
	const u8 *zone_map;
};

static const u8 nlak_001_zone_map[] = {
	[0] = NLAK_001_LED1,
	[1] = NLAK_001_LED2,
	[2] = NLAK_001_LED3,
};

static const u8 nlai_001_zone_map[] = {
	[0] = 0,
	[1] = 0,
	[2] = NLAI_001_LED1,
	[3] = NLAI_001_LED2,
	[4] = NLAI_001_LED3,
	[5] = NLAI_001_LED4,
	[6] = NLAI_001_LED5,
	[7] = NLAI_001_AMBIENT,
};

static const struct excalibur_led_config nlak_001_led_config = {
	.name = "NLAK 001",
	.zone_count = ARRAY_SIZE(nlak_001_zone_map),
	.zone_map = nlak_001_zone_map,
};

static const struct excalibur_led_config nlai_001_led_config = {
	.name = "NLAI 001",
	.zone_count = ARRAY_SIZE(nlai_001_zone_map),
	.zone_map = nlai_001_zone_map,
};

static const struct excalibur_led_config *get_led_config(void)
{
	switch (detected_model) {
	case EXCALIBUR_MODEL_NLAK_001:
		return &nlak_001_led_config;
	case EXCALIBUR_MODEL_NLAI_001:
		return &nlai_001_led_config;
	default:
		return NULL;
	}
}

static u8 get_hardware_led_id(struct wmi_device *wdev, u8 logical_zone)
{
	const struct excalibur_led_config *config = get_led_config();

	if (!config)
		return 0;

	/* Bounds check using per-model zone_count */
	if (logical_zone >= EXCALIBUR_MAX_ZONES ||
	    logical_zone >= config->zone_count)
		return 0;

	if (!config->zone_map[logical_zone]) {
		dev_err(&wdev->dev, "Invalid logical LED zone: %u\n",
			logical_zone);
		return 0;
	}

	return config->zone_map[logical_zone];
}

static int excalibur_set_led_zone(struct wmi_device *wdev, u8 logical_zone,
				  u32 color, u8 brightness)
{
	u8 hw_led_id = get_hardware_led_id(wdev, logical_zone);
	u32 led_data;
	u32 r, g, b;

	if (!hw_led_id)
		return -EINVAL;

	/*
	 * Based on C# driver analysis:
	 * The top byte (bits 24-31) is LED Mode, not brightness level.
	 * Bits 28-31: Mode (1 = Static).
	 * Bits 24-27: Alpha (Brightness).
	 *
	 * The Windows app uses 3 brightness levels: 0, 1, 2.
	 * 0 = Low/Audio, 1 = Medium/Gaming, 2 = High/Performance (idk why they did this).
	 * We map Linux brightness (0-255) to Alpha (0-2).
	 */
	u8 alpha = brightness * 3 / 256;
	r = (color >> 16) & 0xFF;
	g = (color >> 8) & 0xFF;
	b = color & 0xFF;

	/* Mode 1 (Static) | Alpha */
	led_data = ((0x10 | alpha) << 24) | (r << 16) | (g << 8) | b;

	return excalibur_set(wdev, EXCALIBUR_SET_LED, hw_led_id, led_data, 0);
}

enum excalibur_profile_nlai_001 {
	EXCALIBUR_BALANCED_NLAI_001 = 3,
	EXCALIBUR_LOW_POWER_NLAI_001 = 4,
	EXCALIBUR_GAME_MODE_NLAI_001 = 2,
	EXCALIBUR_PERFORMANCE_NLAI_001 = 1,
};

enum excalibur_profile_nlak_001 {
	EXCALIBUR_AUDIO_NLAK_001 = 2, /* I can say as low power mode */
	EXCALIBUR_HIGH_PERFORMANCE_NLAK_001 = 0, /* Performance mode */
	EXCALIBUR_GAME_MODE_NLAK_001 = 1, /* Balanced mode */
};

struct excalibur_platform_profile_config {
	const char *name;
	u8 platform_profile_modes;
	const u8 *zone_map;
};

static const u8 nlai_001_platform_profile_zone_map[] = {
	[0] = EXCALIBUR_LOW_POWER_NLAI_001,
	[1] = EXCALIBUR_BALANCED_NLAI_001,
	[2] = EXCALIBUR_PERFORMANCE_NLAI_001,
	[3] = EXCALIBUR_GAME_MODE_NLAI_001,
};

static const u8 nlak_001_platform_profile_zone_map[] = {
	[0] = EXCALIBUR_AUDIO_NLAK_001,
	[1] = EXCALIBUR_GAME_MODE_NLAK_001,
	[2] = EXCALIBUR_HIGH_PERFORMANCE_NLAK_001,
	[3] = 0,
};

static const struct excalibur_platform_profile_config
	nlai_001_platform_profile_config = {
		.name = "NLAI 001",
		.platform_profile_modes = 4,
		.zone_map = nlai_001_platform_profile_zone_map,
	};

static const struct excalibur_platform_profile_config
	nlak_001_platform_profile_config = {
		.name = "NLAK 001",
		.platform_profile_modes = 3,
		.zone_map = nlak_001_platform_profile_zone_map,
	};

static const struct excalibur_platform_profile_config *
get_platform_profile_config(void)
{
	switch (detected_model) {
	case EXCALIBUR_MODEL_NLAK_001:
		return &nlak_001_platform_profile_config;
	case EXCALIBUR_MODEL_NLAI_001:
		return &nlai_001_platform_profile_config;
	default:
		return NULL;
	}
}

/* Supported board names by model */
static const char *const nlai_boards[] = { "NLAI 001" };
static const char *const nlak_boards[] = { "NLAK 001" };

static enum excalibur_model_type detect_excalibur_model(void)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board_name)
		return EXCALIBUR_MODEL_UNKNOWN;

	if (match_string(nlai_boards, ARRAY_SIZE(nlai_boards), board_name) >= 0)
		return EXCALIBUR_MODEL_NLAI_001;

	if (match_string(nlak_boards, ARRAY_SIZE(nlak_boards), board_name) >= 0)
		return EXCALIBUR_MODEL_NLAK_001;

	return EXCALIBUR_MODEL_UNKNOWN;
}

static bool is_excalibur_dmi_board(void)
{
	if (detected_model == EXCALIBUR_MODEL_UNKNOWN)
		detected_model = detect_excalibur_model();

	return detected_model != EXCALIBUR_MODEL_UNKNOWN;
}

static ssize_t win_key_enabled_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct excalibur_wmi_priv *priv = dev_get_drvdata(dev);
	struct excalibur_wmi_args out = { 0 };
	int ret;

	ret = excalibur_query(priv->wdev, EXCALIBUR_READ, EXCALIBUR_SET_WINKEY,
			      &out);
	if (ret)
		return -EIO;

	priv->winkey_enabled = out.a6 ? 0 : 1;
	return sysfs_emit(buf, "%u\n", priv->winkey_enabled);
}

static ssize_t win_key_enabled_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct excalibur_wmi_priv *priv = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	ret = excalibur_set(priv->wdev, EXCALIBUR_SET_WINKEY, 0, 0,
			    val ? 0 : 1);
	if (ret)
		return -EIO;

	priv->winkey_enabled = val;

	if (priv->input_dev) {
		input_report_switch(priv->input_dev, SW_KEYPAD_SLIDE, !val);
		input_sync(priv->input_dev);
	}

	return count;
}

static DEVICE_ATTR_RW(win_key_enabled);

static ssize_t model_name_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	const char *model_name;

	switch (detected_model) {
	case EXCALIBUR_MODEL_NLAI_001:
		model_name = "NLAI 001";
		break;
	case EXCALIBUR_MODEL_NLAK_001:
		model_name = "NLAK 001";
		break;
	default:
		model_name = "Unknown";
		break;
	}

	return sysfs_emit(buf, "%s\n", model_name);
}

static DEVICE_ATTR_RO(model_name);

static int excalibur_wmi_hwmon_read(struct device *dev,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel, long *val)
{
	struct platform_device *pdev = dev_get_drvdata(dev);
	struct excalibur_wmi_priv *priv = platform_get_drvdata(pdev);
	struct wmi_device *wdev = priv->wdev;
	struct excalibur_wmi_args out = { 0 };
	int ret;

	ret = excalibur_query(wdev, EXCALIBUR_READ, EXCALIBUR_GET_HARDWAREINFO,
			      &out);
	if (ret)
		return -EIO;

	switch (type) {
	case hwmon_fan: {
		if (channel != 0 && channel != 1)
			return -EOPNOTSUPP;

		switch (detected_model) {
		case EXCALIBUR_MODEL_NLAK_001:
			/* Directly read */
			*val = (long)((channel == 0 ? out.a4 : out.a5) &
				      0xFFFF);
			break;
		case EXCALIBUR_MODEL_NLAI_001:
		default:
			/* Byte swap */
			{
				u32 raw = channel == 0 ? out.a4 : out.a5;
				*val = (long)((raw & 0xFF) * 256 +
					      ((raw >> 8) & 0xFF));
			}
			break;
		}

		return 0;
	}
	case hwmon_temp: {
		if (channel != 0 && channel != 1)
			return -EOPNOTSUPP;
		*val = (long)(((channel == 0 ? out.a2 : out.a3) & 0xFF) * 1000);
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int excalibur_wmi_hwmon_read_string(struct device *dev,
					   enum hwmon_sensor_types type,
					   u32 attr, int channel,
					   const char **str)
{
	switch (type) {
	case hwmon_fan:
		switch (channel) {
		case 0:
			*str = "cpu_fan_speed";
			break;
		case 1:
			*str = "gpu_fan_speed";
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;

	case hwmon_temp:
		switch (channel) {
		case 0:
			*str = "cpu_temp";
			break;
		case 1:
			*str = "gpu_temp";
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static umode_t excalibur_wmi_hwmon_is_visible(const void *drvdata,
					      enum hwmon_sensor_types type,
					      u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
	case hwmon_temp:
		return 0444;
	default:
		return 0;
	}
}

static const struct hwmon_channel_info *const excalibur_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops excalibur_wmi_hwmon_ops = {
	.is_visible = excalibur_wmi_hwmon_is_visible,
	.read = excalibur_wmi_hwmon_read,
	.read_string = excalibur_wmi_hwmon_read_string,
};

static struct hwmon_chip_info excalibur_wmi_hwmon_chip_info = {
	.ops = &excalibur_wmi_hwmon_ops,
	.info = excalibur_wmi_hwmon_info,
};

static int excalibur_led_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct excalibur_led *led =
		container_of(cdev, struct excalibur_led, cdev);

	if (brightness == 0)
		return excalibur_set_led_zone(led->wdev, led->zone_id, 0x000000,
					      0);

	return excalibur_set_led_zone(led->wdev, led->zone_id, led->color,
				      brightness);
}

static ssize_t excalibur_led_color_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct excalibur_led *led =
		container_of(cdev, struct excalibur_led, cdev);

	return sysfs_emit(buf, "%06x\n", led->color);
}

static ssize_t excalibur_led_color_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct excalibur_led *led =
		container_of(cdev, struct excalibur_led, cdev);
	u32 color;
	int ret;

	ret = kstrtou32(buf, 16, &color);
	if (ret)
		return ret;

	led->color = color;

	/* Update immediately if LED is on */
	if (cdev->brightness > 0)
		ret = excalibur_set_led_zone(led->wdev, led->zone_id,
					     led->color, cdev->brightness);

	return ret ? ret : count;
}

static DEVICE_ATTR(color, 0644, excalibur_led_color_show,
		   excalibur_led_color_store);

static struct attribute *excalibur_led_attrs[] = { &dev_attr_color.attr, NULL };

static const struct attribute_group excalibur_led_group = {
	.attrs = excalibur_led_attrs,
};

static const struct attribute_group *excalibur_led_groups[] = {
	&excalibur_led_group, NULL
};

static void excalibur_input_device_unregister(void *data)
{
	input_unregister_device(data);
}

static int excalibur_input_setup(struct wmi_device *wdev)
{
	struct excalibur_wmi_priv *priv = dev_get_drvdata(&wdev->dev);
	struct input_dev *input;
	int ret;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input->name = "Excalibur WMI hotkeys";
	input->phys = "excalibur-wmi/input0";
	input->id.bustype = BUS_HOST;
	input->dev.parent = &wdev->dev;

	input_set_capability(input, EV_SW, SW_KEYPAD_SLIDE);

	ret = input_register_device(input);
	if (ret) {
		input_free_device(input);
		dev_err(&wdev->dev, "Failed to register input device: %d\n",
			ret);
		return ret;
	}

	priv->input_dev = input;
	priv->winkey_enabled = true;

	ret = devm_add_action_or_reset(
		&wdev->dev, excalibur_input_device_unregister, input);
	if (ret)
		return ret;

	return 0;
}

static int excalibur_platform_profile_set(struct device *dev,
					  enum platform_profile_option profile)
{
	struct excalibur_wmi_priv *priv = dev_get_drvdata(dev);
	struct wmi_device *wdev = priv->wdev;
	int ret;
	int status = -1;
	const struct excalibur_platform_profile_config *profile_config =
		get_platform_profile_config();

	if (!profile_config)
		return -EOPNOTSUPP;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		if (profile_config->platform_profile_modes > 0)
			status = profile_config->zone_map[0];
		break;
	case PLATFORM_PROFILE_BALANCED:
		if (profile_config->platform_profile_modes > 1)
			status = profile_config->zone_map[1];
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		if (profile_config->platform_profile_modes > 2)
			status = profile_config->zone_map[2];
		break;
	case PLATFORM_PROFILE_CUSTOM:
		if (profile_config->platform_profile_modes > 3)
			status = profile_config->zone_map[3];
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (status < 0)
		return -EOPNOTSUPP;

	ret = excalibur_set(wdev, EXCALIBUR_POWERPLAN, status, 0, 0);
	if (ret)
		return -EIO;

	return 0;
}

static int excalibur_platform_profile_get(struct device *dev,
					  enum platform_profile_option *profile)
{
	struct excalibur_wmi_priv *priv = dev_get_drvdata(dev);
	struct wmi_device *wdev = priv->wdev;
	struct excalibur_wmi_args out = { 0 };
	const struct excalibur_platform_profile_config *profile_config;
	int ret;
	int i;

	profile_config = get_platform_profile_config();
	if (!profile_config)
		return -EOPNOTSUPP;

	ret = excalibur_query(wdev, EXCALIBUR_READ, EXCALIBUR_POWERPLAN, &out);
	if (ret)
		return -EIO;

	for (i = 0; i < profile_config->platform_profile_modes; i++) {
		if (profile_config->zone_map[i] == out.a2) {
			switch (i) {
			case 0:
				*profile = PLATFORM_PROFILE_LOW_POWER;
				return 0;
			case 1:
				*profile = PLATFORM_PROFILE_BALANCED;
				return 0;
			case 2:
				*profile = PLATFORM_PROFILE_PERFORMANCE;
				return 0;
			case 3:
				*profile = PLATFORM_PROFILE_CUSTOM;
				return 0;
			default:
				return -EINVAL;
			}
		}
	}

	return -EINVAL;
}

static int excalibur_platform_profile_probe(void *drvdata,
					    unsigned long *choices)
{
	const struct excalibur_platform_profile_config *cfg =
		get_platform_profile_config();

	if (!cfg)
		return -EOPNOTSUPP;

	if (cfg->platform_profile_modes > 0)
		set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	if (cfg->platform_profile_modes > 1)
		set_bit(PLATFORM_PROFILE_BALANCED, choices);
	if (cfg->platform_profile_modes > 2)
		set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	if (cfg->platform_profile_modes > 3)
		set_bit(PLATFORM_PROFILE_CUSTOM, choices);

	return 0;
}
static const struct platform_profile_ops excalibur_platform_profile_ops = {
	.probe = excalibur_platform_profile_probe,
	.profile_get = excalibur_platform_profile_get,
	.profile_set = excalibur_platform_profile_set,
};

static struct attribute *excalibur_attrs[] = { &dev_attr_win_key_enabled.attr,
					       &dev_attr_model_name.attr,
					       NULL };

static const struct attribute_group excalibur_group = {
	.attrs = excalibur_attrs,
};

static const char *excalibur_get_led_name(int model, int zone)
{
	if (model == EXCALIBUR_MODEL_NLAK_001) {
		switch (zone) {
		case 0:
			return "kbd_zone1";
		case 1:
			return "kbd_zone2";
		case 2:
			return "kbd_zone3";
		}
	} else if (model == EXCALIBUR_MODEL_NLAI_001) {
		switch (zone) {
		case 2:
			return "kbd_zone1";
		case 3:
			return "kbd_zone2";
		case 4:
			return "kbd_zone3";
		case 5:
			return "kbd_zone4";
		case 6:
			return "kbd_zone5";
		case 7:
			return "ambient";
		}
	}
	return NULL;
}

static void excalibur_platform_device_unregister(void *data)
{
	platform_device_unregister(data);
}

static int excalibur_init(struct wmi_device *wdev)
{
	int ret;
	int i;
	const struct excalibur_led_config *cfg = get_led_config();
	struct excalibur_wmi_priv *priv = dev_get_drvdata(&wdev->dev);

	priv->pdev =
		platform_device_alloc("excalibur-wmi", PLATFORM_DEVID_NONE);
	if (!priv->pdev)
		return -ENOMEM;

	priv->pdev->dev.parent = &wdev->dev;
	ret = platform_device_add(priv->pdev);
	if (ret) {
		platform_device_put(priv->pdev);
		return ret;
	}

	platform_set_drvdata(priv->pdev, priv);

	ret = devm_add_action_or_reset(
		&wdev->dev, excalibur_platform_device_unregister, priv->pdev);
	if (ret)
		return ret;

	ret = devm_device_add_group(&priv->pdev->dev, &excalibur_group);
	if (ret) {
		dev_err(&wdev->dev, "Failed to add sysfs group: %d\n", ret);
		return ret;
	}

	/* Hwmon */
	priv->hwmon_dev = devm_hwmon_device_register_with_info(
		&priv->pdev->dev, "excalibur", priv->pdev,
		&excalibur_wmi_hwmon_chip_info, NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		dev_err(&wdev->dev, "Failed to register hwmon device: %d\n",
			ret);
		return ret;
	}

	priv->platform_dev = devm_platform_profile_register(
		&priv->pdev->dev, "excalibur_wmi", priv,
		&excalibur_platform_profile_ops);
	if (IS_ERR(priv->platform_dev)) {
		ret = PTR_ERR(priv->platform_dev);
		dev_err(&wdev->dev, "Failed to register platform profile: %d\n",
			ret);
		return ret;
	}

	ret = excalibur_input_setup(wdev);
	if (ret) {
		dev_err(&wdev->dev, "Failed to setup input device: %d\n", ret);
		return ret;
	}

	if (cfg) {
		for (i = 0; i < cfg->zone_count; i++) {
			const char *func_name;
			struct excalibur_led *led;

			if (!cfg->zone_map[i])
				continue;

			led = &priv->leds[i];
			led->wdev = wdev;
			led->zone_id = i;
			led->color = 0xFFFFFF;

			func_name = excalibur_get_led_name(detected_model, i);
			if (func_name)
				snprintf(led->name, sizeof(led->name),
					 "excalibur:rgb:%s", func_name);
			else
				snprintf(led->name, sizeof(led->name),
					 "excalibur:rgb:zone%d", i);

			led->cdev.name = led->name;
			led->cdev.brightness_set_blocking =
				excalibur_led_brightness_set;
			led->cdev.max_brightness = 255;
			led->cdev.groups = excalibur_led_groups;

			ret = devm_led_classdev_register(&wdev->dev,
							 &led->cdev);
			if (ret) {
				dev_warn(&wdev->dev,
					 "Failed to register LED %s: %d\n",
					 led->name, ret);
				continue;
			}
		}
	}

	return 0;
}

static void excalibur_check_bios_version(struct wmi_device *wdev)
{
	struct excalibur_wmi_args out = { 0 };
	int ret;

	switch (detected_model) {
	case EXCALIBUR_MODEL_NLAI_001:
	case EXCALIBUR_MODEL_NLAK_001:
		break;
	default:
		return;
	}

	ret = excalibur_query(wdev, EXCALIBUR_READ, EXCALIBUR_GET_BIOSINFO,
			      &out);

	if (ret == 0 && out.a0 < 260)
		dev_warn(&wdev->dev,
			 "BIOS version (%u) is older than 260. Warned.\n",
			 out.a0);
}

static int excalibur_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct excalibur_wmi_priv *priv;
	const struct excalibur_led_config *cfg;
	int ret;

	if (!is_excalibur_dmi_board()) {
		dev_err(&wdev->dev,
			"Unsupported Excalibur model or non-Excalibur device detected.\n");
		return -ENODEV;
	}

	/* Allocate private data and initialize it */
	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	priv->wdev = wdev;
	dev_set_drvdata(&wdev->dev, priv);

	excalibur_check_bios_version(wdev);

	ret = excalibur_init(wdev);
	if (ret) {
		dev_err(&wdev->dev, "Initialization failed: %d\n", ret);
		return ret;
	}

	cfg = get_led_config();
	dev_info(&wdev->dev, "Loaded successfully for model: %s\n",
		 cfg ? cfg->name : "Unknown");

	return 0;
}

static void excalibur_wmi_remove(struct wmi_device *wdev)
{
	/* Cleanup handled by devm_* functions */
}

static const struct wmi_device_id excalibur_wmi_id_table[] = {
	{ .guid_string = EXCALIBUR_WMI_GUID },
	{}
};

static struct wmi_driver excalibur_wmi_driver = {
	.driver = {
		.name = "excalibur_wmi",
		.owner = THIS_MODULE,
	},
	.id_table = excalibur_wmi_id_table,
	.probe = excalibur_wmi_probe,
	.remove = excalibur_wmi_remove,
};

static int __init excalibur_wmi_init(void)
{
	if (!is_excalibur_dmi_board()) {
		pr_info("excalibur-wmi: This device is not a recognized excalibur model (board: %s)\n",
			dmi_get_system_info(DMI_BOARD_NAME) ?: "Unknown");
	}

	return wmi_driver_register(&excalibur_wmi_driver);
}

static void __exit excalibur_wmi_exit(void)
{
	wmi_driver_unregister(&excalibur_wmi_driver);
}

module_init(excalibur_wmi_init);
module_exit(excalibur_wmi_exit);

MODULE_DEVICE_TABLE(wmi, excalibur_wmi_id_table);
MODULE_AUTHOR("betelqeyza <avsarusta4422@hotmail.com>");
MODULE_DESCRIPTION("Excalibur laptop WMI driver");
MODULE_LICENSE("GPL");