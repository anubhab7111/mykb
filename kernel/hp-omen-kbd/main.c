// SPDX-License-Identifier: GPL-2.0-only
/*
 * hp_omen_kbd: the four-zone keyboard colors of the HP OMEN 16-k0xxx (board
 * 8A4E) as sysfs attributes, through the HP BIOS WMI interface.
 *
 * The decisions live in core.c. This file adds what needs the kernel: the load
 * gate, the firmware calls and the platform device. It does not claim the WMI
 * GUID and has nothing to do with hp-wmi.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/ktime.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>

#include "core.h"

#define HOK_NAME		"hp-omen-kbd"
#define HOK_BOARD		"8A4E"
#define HOK_WMI_GUID		"5FB7F034-2C63-45E9-BE91-3D44E2C707E4"
#define HOK_BL_GET_SIZE		4
#define HOK_BL_CACHE_MS		200

static bool readonly = true;
module_param(readonly, bool, 0444);
MODULE_PARM_DESC(readonly, "Refuse every write with EPERM (default: 1)");

static unsigned int min_interval_ms = 50;
module_param(min_interval_ms, uint, 0444);
MODULE_PARM_DESC(min_interval_ms, "Least time between two writes, 33 to 1000 ms (default: 50)");

static unsigned int max_writes_per_hour = 40000;
module_param(max_writes_per_hour, uint, 0444);
MODULE_PARM_DESC(max_writes_per_hour,
		 "Writes allowed in a rolling hour, then EBUSY; 0 allows none (default: 40000)");

static bool restore_on_unload = true;
module_param(restore_on_unload, bool, 0444);
MODULE_PARM_DESC(restore_on_unload, "Put the boot colors back at unload (default: 1)");

/* Serializes every firmware call and guards everything in hok. */
static DEFINE_MUTEX(hok_lock);

static struct {
	struct platform_device *pdev;
	u8 kbd_type;
	u8 boot_table[HOK_TABLE_SIZE];
	struct hok_rgb zones[HOK_ZONES];	/* what the zones attribute reads */
	u8 backlight_raw;
	bool backlight_known;
	s64 backlight_read_ms;
	struct hok_budget budget;
	u64 writes_total;
	bool header_changed;
	int last_error;
} hok;

static int hok_call(u32 command, u32 query, const void *in, u32 insize, void *out, u32 outsize)
{
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer input = { HOK_ARGS_SIZE, NULL };
	int method = hok_method_id(outsize);
	union acpi_object *obj;
	acpi_status status;
	int ret;

	lockdep_assert_held(&hok_lock);

	if (method < 0)
		return method;
	input.pointer = kzalloc(HOK_ARGS_SIZE, GFP_KERNEL);
	if (!input.pointer)
		return -ENOMEM;
	hok_build_args(input.pointer, command, query, in, insize);

	status = wmi_evaluate_method(HOK_WMI_GUID, 0, method, &input, &output);
	obj = output.pointer;
	if (status != AE_OK || !obj || obj->type != ACPI_TYPE_BUFFER)
		ret = -EIO;
	else
		ret = hok_parse_reply(obj->buffer.pointer, obj->buffer.length, out, outsize);

	kfree(output.pointer);
	kfree(input.pointer);
	if (ret)
		hok.last_error = ret;
	return ret;
}

static int hok_get_table(u8 *table)
{
	return hok_call(HOK_CMD_BACKLIGHT, HOK_Q_COLOR_GET, NULL, HOK_TABLE_SIZE,
			table, HOK_TABLE_SIZE);
}

/* Reads the backlight from the firmware, unless the last read is still fresh. */
static int hok_refresh_backlight(void)
{
	s64 now_ms = ktime_to_ms(ktime_get_boottime());
	u8 reply[HOK_BL_GET_SIZE];
	int ret;

	lockdep_assert_held(&hok_lock);

	if (hok.backlight_known && now_ms - hok.backlight_read_ms < HOK_BL_CACHE_MS)
		return 0;
	ret = hok_call(HOK_CMD_BACKLIGHT, HOK_Q_BL_GET, NULL, sizeof(reply), reply, sizeof(reply));
	if (ret)
		return ret;
	hok.backlight_raw = reply[0];
	hok.backlight_read_ms = now_ms;
	hok.backlight_known = true;
	return 0;
}

static ssize_t zones_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&hok_lock);
	ret = hok_format_zones(buf, PAGE_SIZE, hok.zones);
	mutex_unlock(&hok_lock);
	return ret;
}
static DEVICE_ATTR_RO(zones);

static ssize_t backlight_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	bool on;
	int ret;

	mutex_lock(&hok_lock);
	ret = hok_refresh_backlight();
	on = hok_backlight_on(hok.backlight_raw);
	mutex_unlock(&hok_lock);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%d\n", on);
}
static DEVICE_ATTR_RO(backlight);

static ssize_t info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hok_info info = {
		.board = HOK_BOARD,
		.readonly = readonly,
		.min_interval_ms = min_interval_ms,
		.max_writes_per_hour = max_writes_per_hour,
	};

	mutex_lock(&hok_lock);
	info.kbd_type = hok.kbd_type;
	info.writes_last_hour = hok_budget_used(&hok.budget, ktime_get_boottime_seconds());
	info.writes_total = hok.writes_total;
	info.budget_exhausted = info.writes_last_hour >= max_writes_per_hour;
	info.last_error = hok.last_error;
	info.backlight_raw = hok.backlight_raw;
	info.header_changed = hok.header_changed;
	memcpy(info.header, hok.boot_table, HOK_HEADER_SIZE);
	mutex_unlock(&hok_lock);

	return hok_format_info(buf, PAGE_SIZE, &info);
}
static DEVICE_ATTR_RO(info);

static struct attribute *hok_attrs[] = {
	&dev_attr_zones.attr,
	&dev_attr_backlight.attr,
	&dev_attr_info.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hok);

static struct platform_driver hok_driver = {
	.driver = {
		.name = HOK_NAME,
		.suppress_bind_attrs = true,
	},
};

/* Adding a board is a code change, never a parameter. */
static const struct dmi_system_id hok_allowlist[] __initconst = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HP"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, HOK_BOARD),
		},
	},
	{ }
};

static int __init hok_read_firmware(void)
{
	u8 type;
	int ret;

	ret = hok_call(HOK_CMD_GAMING, HOK_Q_KBD_TYPE, NULL, 1, &type, 1);
	if (ret) {
		pr_info("keyboard type query failed (%d); not binding\n", ret);
		return -ENODEV;
	}
	if (!hok_kbd_type_supported(type)) {
		pr_info("keyboard type %u is not a four-zone keyboard; not binding\n", type);
		return -ENODEV;
	}
	hok.kbd_type = type;

	ret = hok_get_table(hok.boot_table);
	if (ret)
		return ret;
	hok_table_get_zones(hok.boot_table, hok.zones);
	return hok_refresh_backlight();
}

static void __init hok_log_firmware(void)
{
	char zones[HOK_ZONES_TEXT_LEN + 1];

	hok_format_zones(zones, sizeof(zones), hok.zones);
	zones[HOK_ZONES_TEXT_LEN - 1] = '\0';	/* drop the newline */
	pr_info("HP " HOK_BOARD " keyboard type %u, backlight 0x%02x, zones %s, readonly=%d\n",
		hok.kbd_type, hok.backlight_raw, zones, readonly);
	print_hex_dump(KERN_INFO, KBUILD_MODNAME ": table ", DUMP_PREFIX_OFFSET, 16, 1,
		       hok.boot_table, HOK_TABLE_SIZE, false);
}

static int __init hok_init(void)
{
	struct platform_device *pdev;
	int ret;

	if (!dmi_check_system(hok_allowlist)) {
		pr_info("board not on the allowlist (only HP " HOK_BOARD ")\n");
		return -ENODEV;
	}
	if (!wmi_has_guid(HOK_WMI_GUID)) {
		pr_info("HP BIOS WMI interface " HOK_WMI_GUID " not found\n");
		return -ENODEV;
	}

	min_interval_ms = hok_clamp_interval(min_interval_ms);
	hok_budget_init(&hok.budget);

	mutex_lock(&hok_lock);
	ret = hok_read_firmware();
	mutex_unlock(&hok_lock);
	if (ret)
		return ret;
	hok_log_firmware();

	ret = platform_driver_register(&hok_driver);
	if (ret)
		return ret;

	pdev = platform_device_alloc(HOK_NAME, PLATFORM_DEVID_NONE);
	if (!pdev) {
		ret = -ENOMEM;
		goto err_driver;
	}
	/* In place before the add uevent fires, so the udev rule finds the files. */
	pdev->dev.groups = hok_groups;
	ret = platform_device_add(pdev);
	if (ret)
		goto err_put;
	hok.pdev = pdev;
	return 0;

err_put:
	platform_device_put(pdev);
err_driver:
	platform_driver_unregister(&hok_driver);
	return ret;
}

static void __exit hok_exit(void)
{
	platform_device_unregister(hok.pdev);
	platform_driver_unregister(&hok_driver);
}

module_init(hok_init);
module_exit(hok_exit);

MODULE_DESCRIPTION("Four-zone keyboard colors through HP BIOS WMI (board 8A4E only)");
MODULE_LICENSE("GPL");
