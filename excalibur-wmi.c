// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Casper Excalibur WMI driver
 *
 * Copyright (C) 2025, betelqeyza <avsarusta4422@hotmail.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/hwmon.h>
#include <linux/leds.h>
#include <linux/device.h>
#include <linux/wmi.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/dmi.h>

/*
	0x91, 0x57, 0x4C, 0x64, 0xB0, 0xB7, 0x23, 0x41,  // .WLd..
	0xA9, 0x0B, 0xE9, 0x38, 0x76, 0xE0, 0xDA, 0xAD,  // ...8v...
*/
#define EXCALIBUR_WMI_GUID "91574C64-B0B7-4123-A90B-E93876E0DAAD"

#define EXCALIBUR_READ 0xFA00
#define EXCALIBUR_WRITE 0xFB00
#define EXCALIBUR_GET_HARDWAREINFO 0x0200
#define EXCALIBUR_SET_LED 0x0100
#define EXCALIBUR_POWERPLAN 0x0300
#define EXCALIBUR_SET_WINKEY 0x0200

/*
   These are currently unused or incomplete. Feel free to add more.
*/
enum excalibur_keyboard_led
{
	EXCALIBUR_KEYBOARD_LED_1 = 0x01,
	EXCALIBUR_KEYBOARD_LED_2 = 0x02,
	EXCALIBUR_KEYBOARD_LED_3 = 0x03,
	EXCALIBUR_KEYBOARD_LED_4 = 0x04,
	EXCALIBUR_KEYBOARD_LED_5 = 0x05,
	EXCALIBUR_ALL_KEYBOARD_LEDS = 0x06,
	EXCALIBUR_AMBIENT_LED = 0x07
};

/*
	According to Windows Control Panel, the following are the available power profiles with ACPI queries:
*/
enum excalibur_tmp_profile
{
	EXCALIBUR_BALANCED = 3,
	EXCALIBUR_LOW_POWER = 4,
	EXCALIBUR_GAME_MODE = 2,
	EXCALIBUR_PERFORMANCE = 1,
};

/*
public struct SMI_STRUCT_S
{
	public ushort a0;
	public ushort a1;
	public uint a2;
	public uint a3;
	public uint a4;
	public uint a5;
	public uint a6;
}
*/
struct excalibur_wmi_args
{
	u16 a0, a1;
	u32 a2, a3, a4, a5, a6;
};

struct excalibur_wmi_priv
{
	struct wmi_device *wdev;
	struct device *hwmon_dev;
	struct device *platform_dev;
};

/*
	Probably just the 10th generation G770 model.
	This might need to be updated for other models.
*/
static const char *const g770_thermal_profile_boards[] = {
	"NLAI 001"};

static bool is_excalibur_dmi_board(void)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board_name)
		return false;

	return match_string(g770_thermal_profile_boards,
						ARRAY_SIZE(g770_thermal_profile_boards),
						board_name) >= 0;
}

static u32 excalibur_last_color;
static u8 excalibur_last_led;

/*
	This function is needed to call ACPI methods to control the excalibur laptop's LEDs, Windows Key and power settings.
*/
static acpi_status excalibur_set(struct wmi_device *wdev, u16 a1, u32 a2, u32 a3, u32 a6)
{
	struct excalibur_wmi_args wmi_args = {
		.a0 = EXCALIBUR_WRITE,
		.a1 = a1,
		.a2 = a2,
		.a3 = a3,
		.a6 = a6};
	struct acpi_buffer input = {
		(acpi_size)sizeof(struct excalibur_wmi_args),
		&wmi_args};
	acpi_status ret = wmidev_block_set(wdev, 0, &input);
	if (ACPI_FAILURE(ret))
		dev_err(&wdev->dev, "WMI set failed: %s\n", acpi_format_exception(ret));
	return ret;
}

static acpi_status excalibur_query(struct wmi_device *wdev, u16 a0, u16 a1, struct excalibur_wmi_args *out)
{
	union acpi_object *obj;
	acpi_status ret;

	obj = wmidev_block_query(wdev, 0);
	if (!obj)
		return AE_ERROR;

	/*
		Parse the WMI query response
	*/
	if (obj->type == ACPI_TYPE_BUFFER && obj->buffer.length >= sizeof(struct excalibur_wmi_args))
	{
		memcpy(out, obj->buffer.pointer, sizeof(struct excalibur_wmi_args));
		ret = AE_OK;
	}
	else
	{
		ret = AE_ERROR;
		dev_err(&wdev->dev, "Invalid WMI query response\n");
	}

	kfree(obj);
	return ret;
}

static ssize_t winkey_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct excalibur_wmi_args out = {0};
	struct wmi_device *wdev = to_wmi_device(dev->parent);
	acpi_status ret = excalibur_query(wdev, EXCALIBUR_READ, EXCALIBUR_SET_WINKEY, &out);
	if (ACPI_FAILURE(ret))
		return -EIO;
	return sysfs_emit(buf, "%u\n", out.a6 ? 0 : 1);
}

static ssize_t winkey_control_store(struct device *dev, struct device_attribute *attr,
									const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wmi_device(dev->parent);
	unsigned long val;
	acpi_status ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	ret = excalibur_set(wdev, EXCALIBUR_SET_WINKEY, 0, 0, val ? 0 : 1);
	if (ACPI_FAILURE(ret))
		return -EIO;
	return count;
}

static DEVICE_ATTR_RW(winkey_control);

/*
	These aren't tested yet!!
*/
static ssize_t led_control_store(struct device *dev, struct device_attribute *attr,
								 const char *buf, size_t count)
{
	u64 tmp;
	int ret = kstrtou64(buf, 16, &tmp);
	if (ret)
		return ret;
	u8 led_id = (tmp >> 32) & 0xFF;
	struct wmi_device *wdev = to_wmi_device(dev->parent);
	ret = excalibur_set(wdev, EXCALIBUR_SET_LED, led_id, (u32)tmp, 0);
	if (ACPI_FAILURE(ret))
		return -EIO;
	excalibur_last_color = (u32)tmp;
	excalibur_last_led = led_id;
	return count;
}

static ssize_t led_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%02x%08x\n", excalibur_last_led, excalibur_last_color);
}

static DEVICE_ATTR_RW(led_control);

static int excalibur_wmi_hwmon_read(struct device *dev,
									enum hwmon_sensor_types type, u32 attr,
									int channel, long *val)
{
	struct excalibur_wmi_args out = {0};
	struct wmi_device *wdev = to_wmi_device(dev->parent);
	acpi_status ret = excalibur_query(wdev, EXCALIBUR_READ, EXCALIBUR_GET_HARDWAREINFO, &out);
	if (ACPI_FAILURE(ret))
		return -EIO;

	switch (type)
	{
	case hwmon_fan:
		/*
			CPUFanSpeed = (byte)_WMI.data.a4;
			CPUFanSpeed *= 256;
			CPUFanSpeed += (byte)(_WMI.data.a4 >> 8);
			GPUFanSpeed = (byte)_WMI.data.a5;
			GPUFanSpeed *= 256;
			GPUFanSpeed += (byte)(_WMI.data.a5 >> 8);
		*/
		if (channel == 0)
			*val = (long)((out.a4 & 0xFF) * 256 + ((out.a4 >> 8) & 0xFF));
		else if (channel == 1)
			*val = (long)((out.a5 & 0xFF) * 256 + ((out.a5 >> 8) & 0xFF));
		else
			return -EOPNOTSUPP;
		return 0;
	case hwmon_temp:
		/*
			Not tested yet!!
		*/
		if (channel == 0)
			*val = (long)(out.a2 * 1000);
		else if (channel == 1)
			*val = (long)(out.a3 * 1000);
		else
			return -EOPNOTSUPP;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int excalibur_wmi_hwmon_read_string(struct device *dev,
										   enum hwmon_sensor_types type, u32 attr,
										   int channel, const char **str)
{
	switch (type)
	{
	case hwmon_fan:
		switch (channel)
		{
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
		switch (channel)
		{
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
	switch (type)
	{
	case hwmon_fan:
	case hwmon_temp:
		return 0444;
	default:
		return 0;
	}
}

static const struct hwmon_channel_info *const excalibur_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
					   HWMON_F_INPUT | HWMON_F_LABEL,
					   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(temp,
					   HWMON_T_INPUT | HWMON_T_LABEL,
					   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL};

static const struct hwmon_ops excalibur_wmi_hwmon_ops = {
	.is_visible = excalibur_wmi_hwmon_is_visible,
	.read = excalibur_wmi_hwmon_read,
	.read_string = excalibur_wmi_hwmon_read_string,
};

static struct hwmon_chip_info excalibur_wmi_hwmon_chip_info = {
	.ops = &excalibur_wmi_hwmon_ops,
	.info = excalibur_wmi_hwmon_info,
};

static void excalibur_kbd_led_set(struct led_classdev *led_cdev, enum led_brightness brightness)
{
	u8 level = brightness > 0 ? (brightness > 127 ? 2 : 1) : 0;
	struct wmi_device *wdev = to_wmi_device(led_cdev->dev->parent);
	excalibur_set(wdev, EXCALIBUR_SET_LED, excalibur_last_led,
				  (excalibur_last_color & 0x00FFFFFF) | (level << 24), 0);
}

static int excalibur_platform_profile_set(struct device *dev,
										  enum platform_profile_option profile)
{
	acpi_status ret;
	int status;

	switch (profile)
	{
	case PLATFORM_PROFILE_BALANCED:
		status = EXCALIBUR_BALANCED;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		status = EXCALIBUR_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		status = EXCALIBUR_LOW_POWER;
		break;
	case PLATFORM_PROFILE_CUSTOM:
		status = EXCALIBUR_GAME_MODE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = excalibur_set(to_wmi_device(dev->parent), EXCALIBUR_POWERPLAN, status, 0, 0);
	if (ACPI_FAILURE(ret))
		return -EIO;

	return 0;
}

static int excalibur_platform_profile_get(struct device *dev,
										  enum platform_profile_option *profile)
{
	struct excalibur_wmi_args out = {0};
	struct wmi_device *wdev = to_wmi_device(dev->parent);
	acpi_status ret = excalibur_query(wdev, EXCALIBUR_READ, EXCALIBUR_POWERPLAN, &out);
	if (ACPI_FAILURE(ret))
		return -EIO;

	switch (out.a2)
	{
	case EXCALIBUR_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case EXCALIBUR_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case EXCALIBUR_LOW_POWER:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	case EXCALIBUR_GAME_MODE:
		*profile = PLATFORM_PROFILE_CUSTOM;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int excalibur_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_CUSTOM, choices);
	return 0;
}

static const struct platform_profile_ops excalibur_platform_profile_ops = {
	.probe = excalibur_platform_profile_probe,
	.profile_get = excalibur_platform_profile_get,
	.profile_set = excalibur_platform_profile_set,
};

static struct led_classdev excalibur_kbd_led = {
	.name = "excalibur::kbd_backlight",
	.brightness_set = excalibur_kbd_led_set,
	.max_brightness = 2,
};

static struct attribute *excalibur_attrs[] = {
	&dev_attr_winkey_control.attr,
	&dev_attr_led_control.attr,
	NULL};

static const struct attribute_group excalibur_group = {
	.attrs = excalibur_attrs,
};

static int excalibur_init(struct wmi_device *wdev, struct excalibur_wmi_priv *priv)
{
	int ret;

	priv->hwmon_dev = devm_hwmon_device_register_with_info(&wdev->dev, "excalibur-wmi", wdev,
														   &excalibur_wmi_hwmon_chip_info, NULL);
	if (IS_ERR(priv->hwmon_dev))
	{
		ret = PTR_ERR(priv->hwmon_dev);
		dev_err(&wdev->dev, "Failed to register hwmon device: %d\n", ret);
		return ret;
	}

	priv->platform_dev = devm_platform_profile_register(&wdev->dev, "excalibur-wmi", priv,
														&excalibur_platform_profile_ops);
	if (IS_ERR(priv->platform_dev))
	{
		ret = PTR_ERR(priv->platform_dev);
		dev_err(&wdev->dev, "Failed to register platform profile: %d\n", ret);
		return ret;
	}

	ret = devm_device_add_group(&wdev->dev, &excalibur_group);
	if (ret)
	{
		dev_err(&wdev->dev, "Failed to add sysfs group: %d\n", ret);
		return ret;
	}

	ret = devm_led_classdev_register(&wdev->dev, &excalibur_kbd_led);
	if (ret)
	{
		dev_err(&wdev->dev, "Failed to register LED class device: %d\n", ret);
		return ret;
	}

	return 0;
}

static int excalibur_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct excalibur_wmi_priv *priv;
	int ret;

	if (!is_excalibur_dmi_board())
		return -ENODEV;

	/*
		Allocate private data and initialize it.
	*/
	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	dev_set_drvdata(&wdev->dev, priv);

	ret = excalibur_init(wdev, priv);
	if (ret)
	{
		dev_err(&wdev->dev, "Initialization failed: %d\n", ret);
		return ret;
	}

	dev_info(&wdev->dev, "Loaded!\n");

	return 0;
}

static void excalibur_wmi_remove(struct wmi_device *wdev)
{
}

static const struct wmi_device_id excalibur_wmi_id_table[] = {
	{.guid_string = EXCALIBUR_WMI_GUID},
	{}};

static struct wmi_driver excalibur_wmi_driver = {
	.driver = {
		.name = "excalibur-wmi",
		.owner = THIS_MODULE,
	},
	.id_table = excalibur_wmi_id_table,
	.probe = excalibur_wmi_probe,
	.remove = excalibur_wmi_remove,
};

module_wmi_driver(excalibur_wmi_driver);

MODULE_DEVICE_TABLE(wmi, excalibur_wmi_id_table);
MODULE_AUTHOR("betelqeyza <avsarusta4422@hotmail.com>");
MODULE_DESCRIPTION("Excalibur laptop WMI driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("wmi:91574C64-B0B7-4123-A90B-E93876E0DAAD");