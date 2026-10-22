// SPDX-License-Identifier: GPL-2.0-only
/*
 * hp_omen_kbd core: sysfs text formats, the color table and the info line.
 * Only snprintf, memcpy and memcmp are used, which exist in the kernel and in
 * userspace alike, so tests/core_test.c runs this file unchanged.
 */
#include "core.h"

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Six hex digits at @buf into @out; false if any of them is not a hex digit. */
static bool parse_color(const char *buf, struct hok_rgb *out)
{
	u8 v[3];
	int i;

	for (i = 0; i < 3; i++) {
		int hi = hex_value(buf[2 * i]);
		int lo = hex_value(buf[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return false;
		v[i] = (u8)(hi << 4 | lo);
	}
	out->r = v[0];
	out->g = v[1];
	out->b = v[2];
	return true;
}

int hok_parse_zones(const char *buf, size_t len, struct hok_rgb out[HOK_ZONES])
{
	struct hok_rgb colors[HOK_ZONES];
	int count, i;

	if (len > 0 && buf[len - 1] == '\n')
		len--;
	/* One color (6 characters), or four with one space between (27). */
	if (len == 6)
		count = 1;
	else if (len == 7 * HOK_ZONES - 1)
		count = HOK_ZONES;
	else
		return -EINVAL;

	for (i = 0; i < count; i++) {
		const char *p = buf + 7 * i;

		if (i > 0 && p[-1] != ' ')
			return -EINVAL;
		if (!parse_color(p, &colors[i]))
			return -EINVAL;
	}
	for (i = 0; i < HOK_ZONES; i++)
		out[i] = colors[count == 1 ? 0 : i];
	return 0;
}

int hok_format_zones(char *buf, size_t size, const struct hok_rgb zones[HOK_ZONES])
{
	if (size < HOK_ZONES_TEXT_LEN + 1)
		return -EINVAL;
	snprintf(buf, size, "%02X%02X%02X %02X%02X%02X %02X%02X%02X %02X%02X%02X\n",
		 zones[0].r, zones[0].g, zones[0].b, zones[1].r, zones[1].g, zones[1].b,
		 zones[2].r, zones[2].g, zones[2].b, zones[3].r, zones[3].g, zones[3].b);
	return HOK_ZONES_TEXT_LEN;
}

int hok_parse_bool(const char *buf, size_t len)
{
	if (len == 2 && buf[1] == '\n')
		len = 1;
	if (len != 1)
		return -EINVAL;
	if (buf[0] == '0')
		return 0;
	if (buf[0] == '1')
		return 1;
	return -EINVAL;
}

void hok_table_get_zones(const u8 *table, struct hok_rgb out[HOK_ZONES])
{
	int i;

	for (i = 0; i < HOK_ZONES; i++) {
		const u8 *p = table + HOK_COLOR_OFFSET + 3 * i;

		out[i].r = p[0];
		out[i].g = p[1];
		out[i].b = p[2];
	}
}

void hok_table_set_zones(u8 *table, const struct hok_rgb zones[HOK_ZONES])
{
	int i;

	for (i = 0; i < HOK_ZONES; i++) {
		u8 *p = table + HOK_COLOR_OFFSET + 3 * i;

		p[0] = zones[i].r;
		p[1] = zones[i].g;
		p[2] = zones[i].b;
	}
}

bool hok_header_equal(const u8 *a, const u8 *b)
{
	return memcmp(a, b, HOK_HEADER_SIZE) == 0;
}

int hok_format_info(char *buf, size_t size, const struct hok_info *info)
{
	char header[2 * HOK_HEADER_SIZE + 1];
	int i, n;

	for (i = 0; i < HOK_HEADER_SIZE; i++)
		snprintf(header + 2 * i, 3, "%02x", info->header[i]);

	n = snprintf(buf, size,
		     "board=%s kbd_type=%u readonly=%d min_interval_ms=%u max_writes_per_hour=%u writes_last_hour=%u writes_total=%llu budget_exhausted=%d last_error=%d backlight_raw=0x%02x header_changed=%d header=%s\n",
		     info->board, info->kbd_type, info->readonly, info->min_interval_ms,
		     info->max_writes_per_hour, info->writes_last_hour,
		     (unsigned long long)info->writes_total, info->budget_exhausted,
		     info->last_error, info->backlight_raw, info->header_changed, header);
	if (n < 0 || (size_t)n >= size)
		return -EINVAL;
	return n;
}

static void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static u32 get_le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

size_t hok_build_args(u8 dst[HOK_ARGS_SIZE], u32 command, u32 query,
		      const u8 *data, u32 datasize)
{
	if (datasize > HOK_TABLE_SIZE)
		datasize = HOK_TABLE_SIZE;
	memset(dst, 0, HOK_ARGS_SIZE);
	put_le32(dst, HOK_SIGNATURE);
	put_le32(dst + 4, command);
	put_le32(dst + 8, query);
	put_le32(dst + 12, datasize);
	if (data && datasize)
		memcpy(dst + 16, data, datasize);
	return HOK_ARGS_SIZE;
}

/* The BIOS picks its method by the size of the reply it is asked for. */
int hok_method_id(size_t outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

int hok_bios_errno(u32 code)
{
	if (code == 0)
		return 0;
	if (code == 3 || code == 4)	/* unknown command / command type */
		return -EOPNOTSUPP;
	return -EIO;
}

int hok_parse_reply(const u8 *reply, size_t len, u8 *out, size_t outsize)
{
	u32 code;

	if (len < HOK_REPLY_HEADER)
		return -EIO;
	code = get_le32(reply + 4);
	if (code)
		return hok_bios_errno(code);
	if (len - HOK_REPLY_HEADER < outsize)
		return -EIO;
	if (outsize)
		memcpy(out, reply + HOK_REPLY_HEADER, outsize);
	return 0;
}

bool hok_backlight_on(u8 raw)
{
	return (raw & 0x80) != 0;
}

bool hok_kbd_type_supported(u8 type)
{
	return type == 1 || type == 2;	/* four-zone, with or without numpad */
}

unsigned int hok_clamp_interval(unsigned int ms)
{
	if (ms < HOK_INTERVAL_MIN_MS)
		return HOK_INTERVAL_MIN_MS;
	if (ms > HOK_INTERVAL_MAX_MS)
		return HOK_INTERVAL_MAX_MS;
	return ms;
}

u64 hok_wait_ms(bool written, s64 last_ms, s64 now_ms, unsigned int interval_ms)
{
	s64 elapsed;

	if (!written)
		return 0;
	elapsed = now_ms - last_ms;
	if (elapsed < 0)
		return interval_ms;
	if (elapsed >= (s64)interval_ms)
		return 0;
	return (u64)((s64)interval_ms - elapsed);
}

static s64 budget_minute(s64 now_s)
{
	return now_s < 0 ? 0 : now_s / 60;
}

void hok_budget_init(struct hok_budget *b)
{
	memset(b, 0, sizeof(*b));
}

u32 hok_budget_used(const struct hok_budget *b, s64 now_s)
{
	s64 minute = budget_minute(now_s);
	u64 sum = 0;
	int i;

	for (i = 0; i < HOK_BUDGET_SLOTS; i++)
		if (b->minute[i] > minute - HOK_BUDGET_SLOTS)
			sum += b->count[i];
	return sum > 0xffffffffu ? 0xffffffffu : (u32)sum;
}

bool hok_budget_take(struct hok_budget *b, s64 now_s, u32 max_per_hour)
{
	s64 minute = budget_minute(now_s);
	int slot = (int)(minute % HOK_BUDGET_SLOTS);

	if (hok_budget_used(b, now_s) >= max_per_hour)
		return false;
	/* A stale slot starts over; one stamped later (clock went back) keeps counting. */
	if (b->minute[slot] < minute) {
		b->minute[slot] = minute;
		b->count[slot] = 0;
	}
	b->count[slot]++;
	return true;
}
