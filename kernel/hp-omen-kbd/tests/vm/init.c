// SPDX-License-Identifier: GPL-2.0-only
/*
 * PID 1 of the VM tests: mounts what the module needs, loads wmi.ko, runs the
 * commands in /scenario and powers off. Every command prints one "VM ..." line
 * (kmsg can print several), which run.sh compares with the scenario's .expected.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/module.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ATTR_DIR "/sys/devices/platform/hp-omen-kbd/"
#define LINE_MAX_LEN 8192

/* One write per line, so a kernel console message cannot land inside it. */
static void say(const char *fmt, ...)
{
	char line[LINE_MAX_LEN];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = sizeof(line) - 1;
	if (write(STDOUT_FILENO, line, (size_t)n) < 0)
		_exit(1);
}

static const char *err_name(int err)
{
	const char *name = strerrorname_np(err);

	return name ? name : "unknown-errno";
}

static const char *result(int err)
{
	return err ? err_name(err) : "ok";
}

static long elapsed_ms(const struct timespec *from, const struct timespec *to)
{
	return (to->tv_sec - from->tv_sec) * 1000 + (to->tv_nsec - from->tv_nsec) / 1000000;
}

static int load_module(const char *path, const char *params, int flags)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int err = 0;

	if (fd < 0)
		return errno;
	if (syscall(SYS_finit_module, fd, params, flags) != 0)
		err = errno;
	close(fd);
	return err;
}

/* Reads a sysfs file into text, without the final newline. Returns 0 or an errno. */
static int read_attr(const char *attr, char *text, size_t size)
{
	char path[256];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), ATTR_DIR "%s", attr);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	n = read(fd, text, size - 1);
	if (n < 0) {
		int err = errno;

		close(fd);
		return err;
	}
	close(fd);
	if (n > 0 && text[n - 1] == '\n')
		n--;
	text[n] = '\0';
	return 0;
}

static int write_file(const char *path, const char *text, size_t len)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;
	int err = 0;

	if (fd < 0)
		return errno;
	n = write(fd, text, len);
	if (n < 0)
		err = errno;
	else if ((size_t)n != len)
		err = EIO;
	close(fd);
	return err;
}

/* The scenario's text with each \n turned into a newline, written with one write(). */
static int write_attr(const char *attr, const char *text)
{
	char path[256];
	char decoded[LINE_MAX_LEN];
	size_t len = 0;

	for (; *text && len < sizeof(decoded); text++) {
		if (text[0] == '\\' && text[1] == 'n') {
			decoded[len++] = '\n';
			text++;
		} else {
			decoded[len++] = *text;
		}
	}
	snprintf(path, sizeof(path), ATTR_DIR "%s", attr);
	return write_file(path, decoded, len);
}

static void cmd_info(const char *key)
{
	char text[4096];
	size_t key_len = strlen(key);
	char *token;
	int err = read_attr("info", text, sizeof(text));

	if (err) {
		say("VM info %s: %s\n", key, err_name(err));
		return;
	}
	for (token = strtok(text, " "); token; token = strtok(NULL, " ")) {
		if (strncmp(token, key, key_len) == 0 && token[key_len] == '=') {
			say("VM info %s\n", token);
			return;
		}
	}
	say("VM info %s: missing\n", key);
}

static void cmd_read(const char *attr)
{
	char text[4096];
	int err = read_attr(attr, text, sizeof(text));

	say("VM read %s: %s\n", attr, err ? err_name(err) : text);
}

/* args is "<attr> <text>", where the text may hold spaces. Returns NULL if there is none. */
static char *split_word(char *args)
{
	char *rest = strchr(args, ' ');

	if (!rest)
		return NULL;
	*rest = '\0';
	return rest + 1;
}

static void cmd_write(char *args)
{
	char *text = split_word(args);

	say("VM write %s: %s\n", args, result(write_attr(args, text ? text : "")));
}

static void cmd_slow_write(char *args)
{
	char *min_text = split_word(args);
	char *text = min_text ? split_word(min_text) : NULL;
	struct timespec from, to;
	long took;
	int err;

	if (!text) {
		say("VM slow-write %s: bad arguments\n", args);
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &from);
	err = write_attr(args, text);
	clock_gettime(CLOCK_MONOTONIC, &to);
	took = elapsed_ms(&from, &to);
	if (err)
		say("VM slow-write %s: %s\n", args, err_name(err));
	else if (took < atol(min_text))
		say("VM slow-write %s: fast %ld ms\n", args, took);
	else
		say("VM slow-write %s: ok\n", args);
}

static void cmd_suspend(void)
{
	int err = write_file("/sys/power/pm_test", "devices", 7);

	if (!err)
		err = write_file("/sys/power/state", "mem", 3);
	say("VM suspend: %s\n", result(err));
}

static void cmd_sleep(const char *ms_text)
{
	long ms = atol(ms_text);
	struct timespec wait = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };

	while (nanosleep(&wait, &wait) != 0 && errno == EINTR)
		;
}

/*
 * Calls fn for every message in the kernel log, oldest first. A record is
 * "prio,seq,ts,flags;message\n" followed by optional " KEY=value" device lines.
 */
static void for_each_kmsg(void (*fn)(const char *message, const void *ctx), const void *ctx)
{
	char record[LINE_MAX_LEN];
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		say("VM kmsg-open: %s\n", err_name(errno));
		return;
	}
	for (;;) {
		ssize_t n = read(fd, record, sizeof(record) - 1);
		char *message, *end;

		if (n < 0 && errno == EPIPE)	/* records were overwritten; go on with the next */
			continue;
		if (n <= 0)
			break;
		record[n] = '\0';
		message = strchr(record, ';');
		if (!message)
			continue;
		message++;
		end = strchr(message, '\n');
		if (end)
			*end = '\0';
		fn(message, ctx);
	}
	close(fd);
}

static void print_if_matching(const char *message, const void *ctx)
{
	if (strstr(message, ctx))
		say("VM kmsg: %s\n", message);
}

static void count_if_dirty(const char *message, const void *ctx)
{
	static const char *const bad[] = {
		"BUG:", "Oops", "WARNING:", "Call Trace", "general protection",
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (strstr(message, bad[i])) {
			say("VM kmsg-dirty: %s\n", message);
			++*(int *)ctx;
			return;
		}
	}
}

static void cmd_kmsg_clean(void)
{
	int dirty = 0;

	for_each_kmsg(count_if_dirty, &dirty);
	if (!dirty)
		say("VM kmsg-clean: yes\n");
}

static void run_command(char *line)
{
	char *args = split_word(line);

	if (strcmp(line, "load") == 0)
		say("VM load: %s\n", result(load_module("/hp_omen_kbd.ko", args ? args : "", 0)));
	else if (strcmp(line, "unload") == 0)
		say("VM unload: %s\n",
		    result(syscall(SYS_delete_module, "hp_omen_kbd", O_NONBLOCK) ? errno : 0));
	else if (strcmp(line, "read") == 0 && args)
		cmd_read(args);
	else if (strcmp(line, "write") == 0 && args)
		cmd_write(args);
	else if (strcmp(line, "info") == 0 && args)
		cmd_info(args);
	else if (strcmp(line, "slow-write") == 0 && args)
		cmd_slow_write(args);
	else if (strcmp(line, "suspend") == 0)
		cmd_suspend();
	else if (strcmp(line, "sleep") == 0 && args)
		cmd_sleep(args);
	else if (strcmp(line, "kmsg") == 0 && args)
		for_each_kmsg(print_if_matching, args);
	else if (strcmp(line, "kmsg-clean") == 0)
		cmd_kmsg_clean();
	else
		say("VM bad-command: %s\n", line);
}

static void run_scenario(void)
{
	char line[LINE_MAX_LEN];
	FILE *scenario = fopen("/scenario", "r");

	if (!scenario) {
		say("VM setup: /scenario: %s\n", err_name(errno));
		return;
	}
	while (fgets(line, sizeof(line), scenario)) {
		line[strcspn(line, "\n")] = '\0';
		if (line[0] != '\0' && line[0] != '#')
			run_command(line);
	}
	fclose(scenario);
}

int main(void)
{
	int console;
	int err;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	console = open("/dev/console", O_RDWR);
	if (console >= 0) {
		dup2(console, STDIN_FILENO);
		dup2(console, STDOUT_FILENO);
		dup2(console, STDERR_FILENO);
	}

	err = load_module("/wmi.ko.zst", "", MODULE_INIT_COMPRESSED_FILE);
	if (err)
		say("VM setup: wmi.ko.zst: %s\n", err_name(err));
	run_scenario();

	/* run.sh needs this line: without it the scenario did not finish. */
	say("HARNESS done\n");
	tcdrain(STDOUT_FILENO);
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
