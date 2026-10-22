/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hp_omen_kbd core: every decision the module makes, without kernel calls, so
 * it is unit-tested in userspace (tests/core_test.c) and built into the module.
 */
#ifndef HOK_CORE_H
#define HOK_CORE_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include "userspace.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HOK_ZONES		4
#define HOK_TABLE_SIZE		128
#define HOK_COLOR_OFFSET	25	/* zone i = bytes 25 + 3i .. 27 + 3i (R, G, B) */
#define HOK_HEADER_SIZE		25	/* table bytes 0..24 */
#define HOK_ZONES_TEXT_LEN	28	/* "RRGGBB RRGGBB RRGGBB RRGGBB\n" */

struct hok_rgb {
	u8 r, g, b;
};

/* 4 colors, or 1 for all zones; strict, see the plan's Global Constraints. */
int hok_parse_zones(const char *buf, size_t len, struct hok_rgb out[HOK_ZONES]);
int hok_format_zones(char *buf, size_t size, const struct hok_rgb zones[HOK_ZONES]);
/* "0" or "1", with one optional trailing newline. */
int hok_parse_bool(const char *buf, size_t len);

void hok_table_get_zones(const u8 *table, struct hok_rgb out[HOK_ZONES]);
void hok_table_set_zones(u8 *table, const struct hok_rgb zones[HOK_ZONES]);
bool hok_header_equal(const u8 *a, const u8 *b);

/* What the sysfs `info` file shows. */
struct hok_info {
	const char *board;
	u8 kbd_type;
	bool readonly;
	unsigned int min_interval_ms;
	u32 max_writes_per_hour;
	u32 writes_last_hour;
	u64 writes_total;
	bool budget_exhausted;
	int last_error;
	u8 backlight_raw;
	bool header_changed;
	u8 header[HOK_HEADER_SIZE];
};

int hok_format_info(char *buf, size_t size, const struct hok_info *info);

#ifdef __cplusplus
}
#endif

#endif /* HOK_CORE_H */
