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

/* HP BIOS WMI method (GUID 5FB7F034-2C63-45E9-BE91-3D44E2C707E4) */
#define HOK_SIGNATURE		0x55434553	/* "SECU" in little-endian */
#define HOK_CMD_GAMING		0x20008
#define HOK_CMD_BACKLIGHT	0x20009
#define HOK_Q_KBD_TYPE		0x2B
#define HOK_Q_COLOR_GET		0x02
#define HOK_Q_COLOR_SET		0x03
#define HOK_Q_BL_GET		0x04
#define HOK_Q_BL_SET		0x05
#define HOK_BL_ON		0xE4
#define HOK_BL_OFF		0x64
#define HOK_ARGS_SIZE		144	/* 16-byte header + 128-byte data area */
#define HOK_REPLY_HEADER	8	/* u32 sigpass, u32 return code */

/* Write guards */
#define HOK_INTERVAL_MIN_MS	33
#define HOK_INTERVAL_MAX_MS	1000
#define HOK_BUDGET_SLOTS	60	/* one per minute of the rolling hour */

size_t hok_build_args(u8 dst[HOK_ARGS_SIZE], u32 command, u32 query,
		      const u8 *data, u32 datasize);
int hok_method_id(size_t outsize);
int hok_bios_errno(u32 code);
int hok_parse_reply(const u8 *reply, size_t len, u8 *out, size_t outsize);
bool hok_backlight_on(u8 raw);
bool hok_kbd_type_supported(u8 type);

unsigned int hok_clamp_interval(unsigned int ms);
u64 hok_wait_ms(bool written, s64 last_ms, s64 now_ms, unsigned int interval_ms);

/*
 * Writes per rolling hour, one slot per minute: slot (minute % 60) counts the
 * writes of its absolute minute. Slots stamped with a later minute than now
 * still count, so a clock that steps backwards cannot let more writes through.
 */
struct hok_budget {
	u32 count[HOK_BUDGET_SLOTS];
	s64 minute[HOK_BUDGET_SLOTS];
};

void hok_budget_init(struct hok_budget *b);
u32 hok_budget_used(const struct hok_budget *b, s64 now_s);
bool hok_budget_take(struct hok_budget *b, s64 now_s, u32 max_per_hour);

#ifdef __cplusplus
}
#endif

#endif /* HOK_CORE_H */
