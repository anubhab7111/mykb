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

static void ArgsLayout(void)
{
	static const u8 set_header[16] = { 0x53, 0x45, 0x43, 0x55, 0x09, 0x00,
					   0x02, 0x00, 0x03, 0x00, 0x00, 0x00,
					   0x80, 0x00, 0x00, 0x00 };
	const u8 zero = 0;
	u8 table[HOK_TABLE_SIZE];
	u8 dst[HOK_ARGS_SIZE];

	for (size_t i = 0; i < HOK_TABLE_SIZE; i++)
		table[i] = (u8)(255 - i);
	memset(dst, 0xCC, sizeof(dst));
	CHECK(hok_build_args(dst, 0x20009, 0x03, table, 128) == 144);
	CHECK(memcmp(dst, set_header, 16) == 0);
	CHECK(memcmp(dst + 16, table, 128) == 0);

	memset(dst, 0xCC, sizeof(dst));
	CHECK(hok_build_args(dst, 0x20008, 0x2B, &zero, 1) == 144);
	CHECK(memcmp(dst, set_header, 4) == 0);
	CHECK(dst[4] == 0x08 && dst[5] == 0x00 && dst[6] == 0x02 && dst[7] == 0x00);
	CHECK(dst[8] == 0x2B && dst[9] == 0x00 && dst[10] == 0x00 && dst[11] == 0x00);
	CHECK(dst[12] == 0x01 && dst[13] == 0x00 && dst[14] == 0x00 && dst[15] == 0x00);
	for (size_t i = 16; i < HOK_ARGS_SIZE; i++)
		CHECK(dst[i] == 0);
}

static void MethodIdFollowsReplySize(void)
{
	CHECK(hok_method_id(0) == 1);
	CHECK(hok_method_id(1) == 2);
	CHECK(hok_method_id(4) == 2);
	CHECK(hok_method_id(5) == 3);
	CHECK(hok_method_id(128) == 3);
	CHECK(hok_method_id(129) == 4);
	CHECK(hok_method_id(1024) == 4);
	CHECK(hok_method_id(1025) == 5);
	CHECK(hok_method_id(4096) == 5);
	CHECK(hok_method_id(4097) == -EINVAL);
}

static void ReplyParsingNeverReturnsPositive(void)
{
	static const u32 codes[] = { 2, 3, 4, 5, 0xFF };
	const u8 ok[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xE4 };
	u8 big[136];
	u8 out[128];
	u8 one = 0xAA;

	CHECK(hok_parse_reply(ok, sizeof(ok), &one, 1) == 0);
	CHECK(one == 0xE4);

	memset(big, 0, HOK_REPLY_HEADER);
	for (size_t i = HOK_REPLY_HEADER; i < sizeof(big); i++)
		big[i] = (u8)i;
	memset(out, 0xAA, sizeof(out));
	CHECK(hok_parse_reply(big, sizeof(big), out, 128) == 0);
	CHECK(memcmp(out, big + HOK_REPLY_HEADER, 128) == 0);

	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		const u8 reply[9] = { 0, 0, 0, 0, (u8)codes[i], 0, 0, 0, 0x11 };
		int want = codes[i] == 3 || codes[i] == 4 ? -EOPNOTSUPP : -EIO;
		int ret;

		one = 0xAA;
		ret = hok_parse_reply(reply, sizeof(reply), &one, 1);
		CHECK(ret == want);
		CHECK(ret <= 0);
		CHECK(one == 0xAA);
	}
	one = 0xAA;
	CHECK(hok_parse_reply(ok, 7, &one, 1) == -EIO);
	CHECK(hok_parse_reply(ok, 8, &one, 1) == -EIO);
	CHECK(one == 0xAA);
}

static void BacklightDecodesBitSeven(void)
{
	CHECK(hok_backlight_on(0xE4));
	CHECK(hok_backlight_on(0xB2));
	CHECK(hok_backlight_on(0x80));
	CHECK(!hok_backlight_on(0x64));
	CHECK(!hok_backlight_on(0x32));
	CHECK(!hok_backlight_on(0x00));
}

static void KeyboardTypeGate(void)
{
	CHECK(hok_kbd_type_supported(1));
	CHECK(hok_kbd_type_supported(2));
	CHECK(!hok_kbd_type_supported(0));
	CHECK(!hok_kbd_type_supported(3));
	CHECK(!hok_kbd_type_supported(4));
	CHECK(!hok_kbd_type_supported(5));
	CHECK(!hok_kbd_type_supported(255));
}

static void IntervalClampAndWait(void)
{
	CHECK(hok_clamp_interval(0) == 33);
	CHECK(hok_clamp_interval(33) == 33);
	CHECK(hok_clamp_interval(50) == 50);
	CHECK(hok_clamp_interval(1000) == 1000);
	CHECK(hok_clamp_interval(100000) == 1000);

	CHECK(hok_wait_ms(false, 1000, 1020, 50) == 0);
	CHECK(hok_wait_ms(true, 1000, 1020, 50) == 30);
	CHECK(hok_wait_ms(true, 1000, 1050, 50) == 0);
	CHECK(hok_wait_ms(true, 1000, 990, 50) == 50);
}

static void BudgetIsARollingHour(void)
{
	struct hok_budget b;

	/* max 3: the fourth write of the hour is refused until minute 0 leaves the window */
	hok_budget_init(&b);
	for (int i = 0; i < 3; i++)
		CHECK(hok_budget_take(&b, 0, 3));
	CHECK(!hok_budget_take(&b, 0, 3));
	CHECK(hok_budget_used(&b, 0) == 3);
	CHECK(!hok_budget_take(&b, 3599, 3));
	CHECK(hok_budget_take(&b, 3600, 3));
	CHECK(hok_budget_used(&b, 3600) == 1);

	/* one write per minute fills a max-60 hour exactly */
	hok_budget_init(&b);
	for (s64 m = 0; m < 60; m++)
		CHECK(hok_budget_take(&b, m * 60, 60));
	CHECK(!hok_budget_take(&b, 59 * 60 + 30, 60));
	CHECK(hok_budget_take(&b, 60 * 60, 60));
	CHECK(hok_budget_used(&b, 1000000000) == 0);

	/* max 0: nothing is ever written */
	hok_budget_init(&b);
	CHECK(!hok_budget_take(&b, 0, 0));
	CHECK(!hok_budget_take(&b, 100000, 0));

	/* the clock steps back from minute 60 to minute 0 and forward again */
	hok_budget_init(&b);
	for (int i = 0; i < 3; i++)
		CHECK(hok_budget_take(&b, 3600, 5));
	CHECK(hok_budget_used(&b, 0) == 3);
	CHECK(hok_budget_take(&b, 0, 5));
	CHECK(hok_budget_take(&b, 0, 5));
	CHECK(!hok_budget_take(&b, 0, 5));
	CHECK(!hok_budget_take(&b, 3600, 5));
	CHECK(hok_budget_used(&b, 3600) == 5);
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
	ArgsLayout();
	MethodIdFollowsReplySize();
	ReplyParsingNeverReturnsPositive();
	BacklightDecodesBitSeven();
	KeyboardTypeGate();
	IntervalClampAndWait();
	BudgetIsARollingHour();
	if (failures) {
		fprintf(stderr, "hok-core: %d check(s) failed\n", failures);
		return 1;
	}
	printf("hok-core: all checks passed\n");
	return 0;
}
