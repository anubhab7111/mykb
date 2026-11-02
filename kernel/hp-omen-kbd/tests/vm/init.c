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
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ATTR_DIR "/sys/devices/platform/hp-omen-kbd/"
#define LINE_MAX_LEN 8192
#define RACE_MAX_CHILDREN 16

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
	if (err && took < atol(min_text))
		say("VM slow-write %s: %s, fast %ld ms\n", args, err_name(err), took);
	else if (err)
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
		"ACPI Error", "ACPI BIOS Error", "UBSAN:", "INFO: task",
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

static void cmd_slow_unload(const char *min_text)
{
	struct timespec from, to;
	long took;
	int err = 0;

	clock_gettime(CLOCK_MONOTONIC, &from);
	if (syscall(SYS_delete_module, "hp_omen_kbd", O_NONBLOCK))
		err = errno;
	clock_gettime(CLOCK_MONOTONIC, &to);
	took = elapsed_ms(&from, &to);
	if (err)
		say("VM slow-unload: %s\n", err_name(err));
	else if (took < atol(min_text))
		say("VM slow-unload: fast %ld ms\n", took);
	else
		say("VM slow-unload: ok\n");
}

/*
 * Writers and readers in child processes. The children count into shared memory
 * and print nothing, because their order on the console is not deterministic.
 */
struct race_counts {
	int ok;		/* writes that succeeded */
	int gone;	/* the device had been removed: expected only when racing unload */
	int other;	/* any other error */
	int torn;	/* a zones line that no writer wrote */
};

static struct race_counts *race;
static pid_t race_pids[RACE_MAX_CHILDREN];
static int race_children;

static void race_count_error(int err)
{
	__atomic_add_fetch(err == ENOENT || err == ENODEV ? &race->gone : &race->other, 1,
			   __ATOMIC_RELAXED);
}

/* Writes one color of its own to all four zones, so a mixed line can be told apart. */
static void race_writer(int id, int iters)
{
	unsigned int color = 0x111111u * (unsigned int)(id + 1);
	char line[64];
	int n = snprintf(line, sizeof(line), "%06X %06X %06X %06X\n", color, color, color, color);
	int i;

	for (i = 0; i < iters; i++) {
		int err = write_file(ATTR_DIR "zones", line, (size_t)n);

		if (err)
			race_count_error(err);
		else
			__atomic_add_fetch(&race->ok, 1, __ATOMIC_RELAXED);
	}
}

static int zones_line_is_torn(const char *line)
{
	if (strcmp(line, "40E0D0 40E0D0 40E0D0 0099ED") == 0)	/* the emulator's boot colors */
		return 0;
	return strlen(line) != 27 || strncmp(line, line + 7, 6) != 0 ||
	       strncmp(line, line + 14, 6) != 0 || strncmp(line, line + 21, 6) != 0;
}

static void race_reader(int iters)
{
	static const char *const attrs[] = { "zones", "backlight", "info" };
	char text[4096];
	int i;

	for (i = 0; i < iters; i++) {
		int err = read_attr(attrs[i % 3], text, sizeof(text));

		if (err)
			race_count_error(err);
		else if (i % 3 == 0 && zones_line_is_torn(text))
			__atomic_add_fetch(&race->torn, 1, __ATOMIC_RELAXED);
	}
}

static void race_spawn(void (*writer)(int, int), void (*reader)(int), int id, int iters)
{
	pid_t pid = fork();

	if (pid == 0) {
		if (writer)
			writer(id, iters);
		else
			reader(iters);
		_exit(0);
	}
	if (pid > 0 && race_children < RACE_MAX_CHILDREN)
		race_pids[race_children++] = pid;
}

/* "<writers> <writes each> <readers> <reads each>" */
static void cmd_race_start(const char *args)
{
	int writers = 0, writes = 0, readers = 0, reads = 0;
	int i;

	sscanf(args, "%d %d %d %d", &writers, &writes, &readers, &reads);
	if (writers + readers > RACE_MAX_CHILDREN) {
		say("VM race: too many children\n");
		return;
	}
	race = mmap(NULL, sizeof(*race), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (race == MAP_FAILED) {
		say("VM race: mmap: %s\n", err_name(errno));
		return;
	}
	for (i = 0; i < writers; i++)
		race_spawn(race_writer, NULL, i, writes);
	for (i = 0; i < readers; i++)
		race_spawn(NULL, race_reader, 0, reads);
}

static void race_wait_all(void)
{
	int i;

	for (i = 0; i < race_children; i++)
		waitpid(race_pids[i], NULL, 0);
	race_children = 0;
}

static void cmd_race_wait(void)
{
	race_wait_all();
	say("VM race: ok=%d gone=%d other=%d torn=%d\n", race->ok, race->gone, race->other,
	    race->torn);
}

/* After racing unload: how many writes got through depends on timing, so it is not printed. */
static void cmd_race_wait_gone(void)
{
	race_wait_all();
	say("VM race: other=%d torn=%d\n", race->other, race->torn);
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
	else if (strcmp(line, "slow-unload") == 0 && args)
		cmd_slow_unload(args);
	else if (strcmp(line, "race-start") == 0 && args)
		cmd_race_start(args);
	else if (strcmp(line, "race-wait") == 0)
		cmd_race_wait();
	else if (strcmp(line, "race-wait-gone") == 0)
		cmd_race_wait_gone();
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
