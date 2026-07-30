#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ps4-shell.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct capture {
	unsigned char data[65536];
	size_t len;
};

static int capture_write(void *ctx, const void *data, size_t len) {
	struct capture *capture = ctx;
	if (capture->len + len > sizeof(capture->data)) {
		errno = ENOSPC;
		return -1;
	}
	memcpy(capture->data + capture->len, data, len);
	capture->len += len;
	return 0;
}

static int run(struct ps4_shell_state *state, struct capture *capture,
		const char *command) {
	capture->len = 0;
	return ps4_shell_execute(state, command, capture_write, capture);
}

static void expect_text(const struct capture *capture, const char *expected) {
	assert(capture->len == strlen(expected));
	assert(memcmp(capture->data, expected, capture->len) == 0);
}

static void write_fixture(const char *path, const void *data, size_t len) {
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, data, len) == (ssize_t)len);
	assert(close(fd) == 0);
}

int main(void) {
	struct ps4_shell_state state;
	struct capture capture;
	char tmp[] = "/tmp/ps4-shell-test-XXXXXX";
	char path[PS4_SHELL_PATH_MAX];
	char resolved[PS4_SHELL_PATH_MAX];
	const unsigned char binary[] = {'A', '\0', 'B', '\n'};
	struct stat st;

	assert(ps4_shell_handles("cat sample.bin") == 1);
	assert(ps4_shell_handles("ls -la") == 1);
	assert(ps4_shell_handles("id") == 0);
	assert(ps4_shell_handles("") == 0);

	assert(mkdtemp(tmp) != NULL);
	ps4_shell_init(&state, tmp);

	assert(ps4_shell_resolve("/data/dropbear", "../pkg/./x", resolved,
			sizeof(resolved)) == 0);
	assert(strcmp(resolved, "/data/pkg/x") == 0);
	assert(ps4_shell_resolve("/", "../../../../data", resolved,
			sizeof(resolved)) == 0);
	assert(strcmp(resolved, "/data") == 0);

	assert(run(&state, &capture, "pwd") == 0);
	snprintf(path, sizeof(path), "%s\n", tmp);
	expect_text(&capture, path);

	assert(run(&state, &capture, "mkdir alpha") == 0);
	assert(run(&state, &capture, "cd alpha") == 0);
	assert(run(&state, &capture, "pwd") == 0);
	snprintf(path, sizeof(path), "%s/alpha\n", tmp);
	expect_text(&capture, path);
	assert(run(&state, &capture, "cd ..") == 0);

	snprintf(path, sizeof(path), "%s/sample.bin", tmp);
	write_fixture(path, binary, sizeof(binary));
	assert(run(&state, &capture, "cat sample.bin") == 0);
	assert(capture.len == sizeof(binary));
	assert(memcmp(capture.data, binary, sizeof(binary)) == 0);

	write_fixture(path, "one\ntwo\nthree\nfour\n", 19);
	assert(run(&state, &capture, "head -n 2 sample.bin") == 0);
	expect_text(&capture, "one\ntwo\n");
	assert(run(&state, &capture, "tail -n 2 sample.bin") == 0);
	expect_text(&capture, "three\nfour\n");

	assert(run(&state, &capture, "cp sample.bin copy.bin") == 0);
	snprintf(path, sizeof(path), "%s/copy.bin", tmp);
	assert(stat(path, &st) == 0 && st.st_size == 19);
	assert(run(&state, &capture, "mv copy.bin moved.bin") == 0);
	assert(stat(path, &st) != 0 && errno == ENOENT);
	snprintf(path, sizeof(path), "%s/moved.bin", tmp);
	assert(stat(path, &st) == 0 && st.st_size == 19);

	assert(run(&state, &capture, "touch empty") == 0);
	snprintf(path, sizeof(path), "%s/empty", tmp);
	assert(stat(path, &st) == 0 && st.st_size == 0);
	assert(run(&state, &capture, "stat empty") == 0);
	assert(memmem(capture.data, capture.len, "size: 0", 7) != NULL);
	assert(run(&state, &capture, "chmod 0600 empty") == 0);
	assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

	assert(run(&state, &capture, "grep two sample.bin") == 0);
	expect_text(&capture, "two\n");
	assert(run(&state, &capture, "grep absent sample.bin") == 1);
	expect_text(&capture, "");
	assert(run(&state, &capture, "hexdump -n 4 sample.bin") == 0);
	assert(memmem(capture.data, capture.len, "00000000", 8) != NULL);
	assert(memmem(capture.data, capture.len, "6f 6e 65 0a", 11) != NULL);
	assert(run(&state, &capture, "find .") == 0);
	assert(memmem(capture.data, capture.len, "sample.bin", 10) != NULL);
	assert(memmem(capture.data, capture.len, "alpha", 5) != NULL);

	assert(run(&state, &capture, "ls -la") == 0);
	assert(memmem(capture.data, capture.len, "alpha/", 6) != NULL);
	assert(memmem(capture.data, capture.len, "moved.bin", 9) != NULL);

	assert(run(&state, &capture, "rm empty") == 0);
	assert(stat(path, &st) != 0 && errno == ENOENT);
	assert(run(&state, &capture, "rm moved.bin") == 0);
	assert(run(&state, &capture, "rm sample.bin") == 0);
	assert(run(&state, &capture, "rmdir alpha") == 0);

	assert(run(&state, &capture, "cat missing") == 1);
	assert(memmem(capture.data, capture.len, "No such file", 12) != NULL);
	assert(run(&state, &capture, "rm /") == 1);
	assert(run(&state, &capture, "unknown") == 127);

	assert(rmdir(tmp) == 0);
	puts("PASS test_ps4_shell_core");
	return 0;
}
