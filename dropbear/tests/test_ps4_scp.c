#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ps4-scp.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

struct server_ctx {
	int fd;
	struct ps4_scp_request request;
	int result;
};

static void *run_server(void *opaque) {
	struct server_ctx *ctx = opaque;
	ctx->result = ps4_scp_serve(ctx->fd, &ctx->request);
	close(ctx->fd);
	return NULL;
}

static void write_all(int fd, const void *data, size_t len) {
	const unsigned char *bytes = data;
	while (len > 0) {
		ssize_t sent = write(fd, bytes, len);
		assert(sent > 0);
		bytes += sent;
		len -= (size_t)sent;
	}
}

static void read_all(int fd, void *data, size_t len) {
	unsigned char *bytes = data;
	while (len > 0) {
		ssize_t got = read(fd, bytes, len);
		assert(got > 0);
		bytes += got;
		len -= (size_t)got;
	}
}

static void read_line(int fd, char *line, size_t size) {
	size_t used = 0;
	while (used + 1 < size) {
		assert(read(fd, line + used, 1) == 1);
		if (line[used++] == '\n') break;
	}
	line[used] = '\0';
}

int main(void) {
	char tmp[] = "/tmp/ps4-scp-test-XXXXXX";
	char path[PS4_SCP_PATH_MAX];
	struct ps4_scp_request request;
	struct server_ctx server;
	pthread_t thread;
	int pair[2];
	unsigned char ack;
	char line[256];
	char data[6] = {0};
	struct stat st;

	assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
	assert(mkdtemp(tmp) != NULL);
	assert(ps4_scp_parse_command("scp -t 'upload dir'", tmp, &request) == 0);
	assert(request.mode == PS4_SCP_RECEIVE);
	snprintf(path, sizeof(path), "%s/upload dir", tmp);
	assert(strcmp(request.path, path) == 0);
	assert(ps4_scp_parse_command("scp -f file.txt", tmp, &request) == 0);
	assert(request.mode == PS4_SCP_SEND);
	assert(ps4_scp_parse_command("scp -vT -f -- file.txt", tmp, &request) == 0);
	assert(request.mode == PS4_SCP_SEND);
	assert(ps4_scp_parse_command("scp file.txt", tmp, &request) != 0);
	assert(ps4_scp_parse_command("rm file.txt", tmp, &request) != 0);

	snprintf(path, sizeof(path), "%s/upload", tmp);
	assert(mkdir(path, 0755) == 0);
	assert(ps4_scp_parse_command("scp -t upload", tmp, &server.request) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	server.fd = pair[1]; server.result = -99;
	assert(pthread_create(&thread, NULL, run_server, &server) == 0);
	read_all(pair[0], &ack, 1); assert(ack == 0);
	write_all(pair[0], "C0644 5 hello.txt\n", 18);
	read_all(pair[0], &ack, 1); assert(ack == 0);
	write_all(pair[0], "hello\0", 6);
	read_all(pair[0], &ack, 1); assert(ack == 0);
	shutdown(pair[0], SHUT_WR); close(pair[0]);
	assert(pthread_join(thread, NULL) == 0 && server.result == 0);
	snprintf(path, sizeof(path), "%s/upload/hello.txt", tmp);
	assert(stat(path, &st) == 0 && st.st_size == 5);
	assert(unlink(path) == 0);

	assert(ps4_scp_parse_command("scp -t upload", tmp, &server.request) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	server.fd = pair[1]; server.result = -99;
	assert(pthread_create(&thread, NULL, run_server, &server) == 0);
	read_all(pair[0], &ack, 1); assert(ack == 0);
	write_all(pair[0], "C0644 3 ../bad\n", 15);
	read_all(pair[0], &ack, 1); assert(ack != 0);
	close(pair[0]);
	assert(pthread_join(thread, NULL) == 0 && server.result != 0);
	snprintf(path, sizeof(path), "%s/bad", tmp);
	assert(stat(path, &st) != 0);

	snprintf(path, sizeof(path), "%s/upload/partial.bin", tmp);
	{ int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); assert(fd >= 0); write_all(fd, "safe", 4); close(fd); }
	assert(ps4_scp_parse_command("scp -t upload", tmp, &server.request) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	server.fd = pair[1]; server.result = -99;
	assert(pthread_create(&thread, NULL, run_server, &server) == 0);
	read_all(pair[0], &ack, 1); assert(ack == 0);
	write_all(pair[0], "C0644 10 partial.bin\n", 21);
	read_all(pair[0], &ack, 1); assert(ack == 0);
	write_all(pair[0], "abc", 3);
	close(pair[0]);
	assert(pthread_join(thread, NULL) == 0 && server.result != 0);
	{ int fd = open(path, O_RDONLY); assert(fd >= 0); memset(data, 0, sizeof(data)); assert(read(fd, data, 4) == 4); close(fd); assert(memcmp(data, "safe", 4) == 0); }
	assert(unlink(path) == 0);

	snprintf(path, sizeof(path), "%s/source.txt", tmp);
	{ int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); assert(fd >= 0); write_all(fd, "world", 5); close(fd); }
	assert(ps4_scp_parse_command("scp -f source.txt", tmp, &server.request) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	server.fd = pair[1]; server.result = -99;
	assert(pthread_create(&thread, NULL, run_server, &server) == 0);
	ack = 0; write_all(pair[0], &ack, 1);
	read_line(pair[0], line, sizeof(line));
	assert(strncmp(line, "C0644 5 source.txt\n", 20) == 0);
	write_all(pair[0], &ack, 1);
	read_all(pair[0], data, 5); assert(memcmp(data, "world", 5) == 0);
	read_all(pair[0], &ack, 1); assert(ack == 0);
	write_all(pair[0], &ack, 1);
	close(pair[0]);
	assert(pthread_join(thread, NULL) == 0 && server.result == 0);

	snprintf(path, sizeof(path), "%s/large.bin", tmp);
	{ unsigned char block[4096]; int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); assert(fd >= 0); for (size_t i = 0; i < sizeof(block); i++) block[i] = (unsigned char)i; for (int i = 0; i < 64; i++) write_all(fd, block, sizeof(block)); close(fd); }
	assert(ps4_scp_parse_command("scp -f large.bin", tmp, &server.request) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	server.fd = pair[1]; server.result = -99;
	assert(pthread_create(&thread, NULL, run_server, &server) == 0);
	ack = 0; write_all(pair[0], &ack, 1);
	read_all(pair[0], &ack, 1); assert(ack == 1);
	read_line(pair[0], line, sizeof(line)); assert(strstr(line, "8192") != NULL);
	close(pair[0]);
	assert(pthread_join(thread, NULL) == 0 && server.result != 0);
	assert(unlink(path) == 0);

	snprintf(path, sizeof(path), "%s/shrink.txt", tmp);
	{ int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); assert(fd >= 0); write_all(fd, "abcde", 5); close(fd); }
	assert(ps4_scp_parse_command("scp -f shrink.txt", tmp, &server.request) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	server.fd = pair[1]; server.result = -99;
	assert(pthread_create(&thread, NULL, run_server, &server) == 0);
	ack = 0; write_all(pair[0], &ack, 1);
	read_line(pair[0], line, sizeof(line)); assert(strncmp(line, "C0644 5 shrink.txt\n", 20) == 0);
	assert(truncate(path, 2) == 0);
	write_all(pair[0], &ack, 1); shutdown(pair[0], SHUT_WR);
	{ ssize_t total = 0, got; while ((got = read(pair[0], data + total, sizeof(data) - (size_t)total)) > 0) total += got; assert(total == 2); assert(memcmp(data, "ab", 2) == 0); }
	close(pair[0]);
	assert(pthread_join(thread, NULL) == 0 && server.result != 0);
	assert(unlink(path) == 0);

	assert(unlink((snprintf(path, sizeof(path), "%s/source.txt", tmp), path)) == 0);
	assert(rmdir((snprintf(path, sizeof(path), "%s/upload", tmp), path)) == 0);
	assert(rmdir(tmp) == 0);
	puts("PASS test_ps4_scp");
	return 0;
}
