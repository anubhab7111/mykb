// SPDX-License-Identifier: GPL-2.0-only
/* Userspace unit tests for the hp_omen_kbd core, run by CTest (hok-core). */
#include "core.h"

static int failures;

#define CHECK(expr)                                                            \
	do {                                                                   \
		if (!(expr)) {                                                 \
			fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n",       \
				__FILE__, __LINE__, __func__, #expr);          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* A literal with its exact length, embedded NULs included. */
#define LIT(s) (s), (sizeof(s) - 1)

static bool rgb_eq(struct hok_rgb a, struct hok_rgb b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

static void ZonesParserAcceptsTheDaemonsLine(void)
{
	const struct hok_rgb want[HOK_ZONES] = {
		{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 }
	};
	const char *lines[] = { "FF0000 00FF00 0000FF FFFFFF\n",
				"FF0000 00FF00 0000FF FFFFFF",
				"ff0000 00ff00 0000ff ffffff\n" };
	const struct hok_rgb teal = { 0x40, 0xE0, 0xD0 };
	struct hok_rgb out[HOK_ZONES];

	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		memset(out, 0, sizeof(out));
		CHECK(hok_parse_zones(lines[i], strlen(lines[i]), out) == 0);
		for (int z = 0; z < HOK_ZONES; z++)
			CHECK(rgb_eq(out[z], want[z]));
	}
	memset(out, 0, sizeof(out));
	CHECK(hok_parse_zones(LIT("40E0D0\n"), out) == 0);
	for (int z = 0; z < HOK_ZONES; z++)
		CHECK(rgb_eq(out[z], teal));
}

static void ZonesParserIsStrict(void)
{
	static const struct {
		const char *s;
		size_t len;
	} cases[] = {
		{ LIT("") },
		{ LIT("\n") },
		{ LIT("FF0000\n\n") },
		{ LIT("FF0000\r\n") },
		{ LIT("FF0000 00FF00\n") },
		{ LIT("FF0000 00FF00 0000FF\n") },
		{ LIT("FF0000 00FF00 0000FF FFFFFF 000000\n") },
		{ LIT("FF0000  00FF00 0000FF FFFFFF\n") },
		{ LIT("FF0000 00FF00 0000FF FFFFFF \n") },
		{ LIT(" FF0000\n") },
		{ LIT("#FF0000\n") },
		{ LIT("FF000\n") },
		{ LIT("FF00000\n") },
		{ LIT("GG0000\n") },
		{ LIT("FF\0" "000\n") },
	};
	static char big[4096];
	const struct hok_rgb sentinel = { 1, 2, 3 };
	struct hok_rgb out[HOK_ZONES];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int ret;

		for (int z = 0; z < HOK_ZONES; z++)
			out[z] = sentinel;
		ret = hok_parse_zones(cases[i].s, cases[i].len, out);
		if (ret != -EINVAL)
			fprintf(stderr, "  case %zu returned %d\n", i, ret);
		CHECK(ret == -EINVAL);
		for (int z = 0; z < HOK_ZONES; z++)
			CHECK(rgb_eq(out[z], sentinel));
	}
	memset(big, 'F', sizeof(big));
	CHECK(hok_parse_zones(big, sizeof(big), out) == -EINVAL);
}

static void ZonesFormatIsTheDaemonsLine(void)
{
	const struct hok_rgb zones[HOK_ZONES] = { { 0x40, 0xE0, 0xD0 },
						  { 0x40, 0xE0, 0xD0 },
						  { 0x40, 0xE0, 0xD0 },
						  { 0x00, 0x99, 0xED } };
	char buf[64];
	char small[28];

	CHECK(hok_format_zones(buf, sizeof(buf), zones) == 28);
	CHECK(strcmp(buf, "40E0D0 40E0D0 40E0D0 0099ED\n") == 0);
	CHECK(hok_format_zones(small, sizeof(small), zones) == -EINVAL);
}

static void BoolParser(void)
{
	static const char *const bad[] = { "2", "10", " 1", "1 ", "", "true" };

	CHECK(hok_parse_bool(LIT("0")) == 0);
	CHECK(hok_parse_bool(LIT("1")) == 1);
	CHECK(hok_parse_bool(LIT("0\n")) == 0);
	CHECK(hok_parse_bool(LIT("1\n")) == 1);
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(hok_parse_bool(bad[i], strlen(bad[i])) == -EINVAL);
}

static void PatchTouchesOnlyZoneBytes(void)
{
	const struct hok_rgb zones[HOK_ZONES] = {
		{ 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { 10, 11, 12 }
	};
	u8 table[HOK_TABLE_SIZE];
	u8 orig[HOK_TABLE_SIZE];
	struct hok_rgb back[HOK_ZONES];

	for (size_t i = 0; i < HOK_TABLE_SIZE; i++)
		table[i] = (u8)(i * 7 + 3);
	memcpy(orig, table, sizeof(table));
	hok_table_set_zones(table, zones);
	CHECK(memcmp(table, orig, 25) == 0);
	CHECK(memcmp(table + 37, orig + 37, HOK_TABLE_SIZE - 37) == 0);
	for (int i = 0; i < 12; i++)
		CHECK(table[25 + i] == i + 1);
	hok_table_get_zones(table, back);
	for (int z = 0; z < HOK_ZONES; z++)
		CHECK(rgb_eq(back[z], zones[z]));
}

static void HeaderCompare(void)
{
	u8 a[HOK_TABLE_SIZE];
	u8 b[HOK_TABLE_SIZE];

	for (size_t i = 0; i < HOK_TABLE_SIZE; i++)
		a[i] = (u8)(i * 3 + 1);
	memcpy(b, a, sizeof(a));
	CHECK(hok_header_equal(a, b));
	b[24] = (u8)(b[24] ^ 1);
	CHECK(!hok_header_equal(a, b));
	b[24] = a[24];
	b[25] = (u8)(b[25] ^ 1);
	CHECK(hok_header_equal(a, b));
}

static void InfoLineIsExact(void)
{
	static const char want[] =
		"board=8A4E kbd_type=2 readonly=1 min_interval_ms=50 "
		"max_writes_per_hour=40000 writes_last_hour=3 writes_total=12 "
		"budget_exhausted=0 last_error=0 backlight_raw=0xe4 "
		"header_changed=0 "
		"header=000102030405060708090a0b0c0d0e0f101112131415161718\n";
	struct hok_info info = {
		.board = "8A4E",
		.kbd_type = 2,
		.readonly = true,
		.min_interval_ms = 50,
		.max_writes_per_hour = 40000,
		.writes_last_hour = 3,
		.writes_total = 12,
		.budget_exhausted = false,
		.last_error = 0,
		.backlight_raw = 0xE4,
		.header_changed = false,
	};
	char buf[512];
	char small[64];

	for (int i = 0; i < HOK_HEADER_SIZE; i++)
		info.header[i] = (u8)i;
	CHECK(hok_format_info(buf, sizeof(buf), &info) == (int)(sizeof(want) - 1));
	CHECK(strcmp(buf, want) == 0);
	info.last_error = -5;
	CHECK(hok_format_info(buf, sizeof(buf), &info) > 0);
	CHECK(strstr(buf, " last_error=-5 ") != NULL);
	CHECK(hok_format_info(small, sizeof(small), &info) == -EINVAL);
}

int main(void)
{
	ZonesParserAcceptsTheDaemonsLine();
	ZonesParserIsStrict();
	ZonesFormatIsTheDaemonsLine();
	BoolParser();
	PatchTouchesOnlyZoneBytes();
	HeaderCompare();
	InfoLineIsExact();
	if (failures) {
		fprintf(stderr, "hok-core: %d check(s) failed\n", failures);
		return 1;
	}
	printf("hok-core: all checks passed\n");
	return 0;
}
