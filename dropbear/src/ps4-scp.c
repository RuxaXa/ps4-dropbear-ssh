#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ps4-scp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct scp_args {
	char storage[2048];
	char *argv[16];
	int argc;
};

static ssize_t scp_write(int fd, const void *data, size_t len) {
	size_t chunk = len > 512 ? 512 : len;
#ifdef MSG_NOSIGNAL
	return send(fd, data, chunk, MSG_NOSIGNAL);
#else
	return write(fd, data, chunk);
#endif
}

static int retry_pause(unsigned int *retries) {
	if (++*retries > 30000) {
		errno = ETIMEDOUT;
		return -1;
	}
#ifdef __ORBIS__
	(void)sched_yield();
#else
	{
		struct timespec delay = { 0, 1000000 };
		struct timespec remaining;
		while (nanosleep(&delay, &remaining) != 0) {
			if (errno != EINTR) return -1;
			delay = remaining;
		}
	}
#endif
	return 0;
}

static int write_all(int fd, const void *data, size_t len) {
	const unsigned char *bytes = data;
	unsigned int retries = 0;
	while (len > 0) {
		ssize_t sent = scp_write(fd, bytes, len);
		if (sent < 0 && errno == EINTR) continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (retry_pause(&retries) == 0) continue;
			return -1;
		}
		if (sent <= 0) return -1;
		retries = 0;
		bytes += sent;
		len -= (size_t)sent;
	}
	return 0;
}

static int read_all(int fd, void *data, size_t len) {
	unsigned char *bytes = data;
	unsigned int retries = 0;
	while (len > 0) {
		ssize_t got = read(fd, bytes, len);
		if (got < 0 && errno == EINTR) continue;
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (retry_pause(&retries) == 0) continue;
			return -1;
		}
		if (got <= 0) return -1;
		retries = 0;
		bytes += got;
		len -= (size_t)got;
	}
	return 0;
}

static int read_line(int fd, char *line, size_t size) {
	size_t used = 0;
	unsigned int retries = 0;
	while (used + 1 < size) {
		ssize_t got = read(fd, line + used, 1);
		if (got < 0 && errno == EINTR) continue;
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (retry_pause(&retries) == 0) continue;
			return -1;
		}
		if (got == 0) return used == 0 ? 0 : -1;
		if (got < 0) return -1;
		retries = 0;
		if (line[used++] == '\n') {
			line[used] = '\0';
			return 1;
		}
	}
	errno = EMSGSIZE;
	return -1;
}

static int send_status(int fd, unsigned char status, const char *message) {
	if (write_all(fd, &status, 1) != 0) return -1;
	if (status != 0 && message != NULL) {
		if (write_all(fd, message, strlen(message)) != 0 ||
				write_all(fd, "\n", 1) != 0) return -1;
	}
	return 0;
}

static int read_status(int fd) {
	unsigned char status;
	char line[1024];
	if (read_all(fd, &status, 1) != 0) return -1;
	if (status == 0) return 0;
	if (status == 1 || status == 2) (void)read_line(fd, line, sizeof(line));
	errno = EPROTO;
	return -1;
}

static int split_command(const char *command, struct scp_args *args) {
	const char *in = command;
	char *out = args->storage;
	size_t remaining = sizeof(args->storage);
	args->argc = 0;
	while (*in != '\0') {
		char quote = '\0';
		while (*in == ' ' || *in == '\t') in++;
		if (*in == '\0') break;
		if (args->argc == 16 || remaining < 2) return -1;
		args->argv[args->argc++] = out;
		while (*in != '\0') {
			char ch = *in++;
			if (quote == '\0' && (ch == ' ' || ch == '\t')) break;
			if (ch == '\\' && quote != '\'') {
				if (*in == '\0') return -1;
				ch = *in++;
			} else if (ch == '\'' || ch == '"') {
				if (quote == '\0') { quote = ch; continue; }
				if (quote == ch) { quote = '\0'; continue; }
			}
			if (remaining < 2) return -1;
			*out++ = ch; remaining--;
		}
		if (quote != '\0') return -1;
		*out++ = '\0'; remaining--;
	}
	return 0;
}

int ps4_scp_parse_command(const char *command, const char *cwd,
		struct ps4_scp_request *request) {
	struct scp_args args;
	int mode = -1;
	int index;
	memset(request, 0, sizeof(*request));
	if (split_command(command, &args) != 0 || args.argc < 3 ||
			strcmp(args.argv[0], "scp") != 0) return -1;
	for (index = 1; index < args.argc - 1; index++) {
		const char *option = args.argv[index];
		if (strcmp(option, "--") == 0) {
			if (index != args.argc - 2) return -1;
			continue;
		}
		if (option[0] != '-' || option[1] == '\0') return -1;
		for (option++; *option != '\0'; option++) {
			switch (*option) {
			case 't': if (mode != -1) return -1; mode = PS4_SCP_RECEIVE; break;
			case 'f': if (mode != -1) return -1; mode = PS4_SCP_SEND; break;
			case 'p': request->preserve = 1; break;
			case 'd': request->target_should_be_directory = 1; break;
			case 'v': case 'q': case 'T': break;
			default: return -1;
			}
		}
	}
	if (mode == -1 || ps4_shell_resolve(cwd, args.argv[args.argc - 1],
			request->path, sizeof(request->path)) != 0) return -1;
	request->mode = (enum ps4_scp_mode)mode;
	return 0;
}

static int safe_name(const char *name) {
	return *name != '\0' && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
			strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static int receive_file(int fd, const char *path, unsigned int mode,
		uint64_t size) {
	unsigned char buffer[4096];
	char temporary[PS4_SCP_PATH_MAX];
	uint64_t remaining = size;
	unsigned char end;
	unsigned int retries = 0;
	int output;
	int failed = 0;

	if (snprintf(temporary, sizeof(temporary), "%s.ps4scp-part", path) >=
			(int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	(void)unlink(temporary);
	output = open(temporary, O_WRONLY | O_CREAT | O_EXCL, mode & 0777);
	if (output < 0) return -1;
	if (send_status(fd, 0, NULL) != 0) failed = 1;
	while (!failed && remaining > 0) {
		size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
		ssize_t got = read(fd, buffer, wanted);
		if (got < 0 && errno == EINTR) continue;
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (retry_pause(&retries) == 0) continue;
			failed = 1;
			break;
		}
		if (got <= 0) { failed = 1; break; }
		retries = 0;
		for (size_t offset = 0; offset < (size_t)got;) {
			ssize_t written = write(output, buffer + offset, (size_t)got - offset);
			if (written < 0 && errno == EINTR) continue;
			if (written <= 0) { failed = 1; break; }
			offset += (size_t)written;
		}
		remaining -= (uint64_t)got;
	}
	if (!failed && read_all(fd, &end, 1) != 0) failed = 1;
	if (!failed && end != 0) failed = 1;
	if (close(output) != 0) failed = 1;
	if (failed) {
		(void)unlink(temporary);
		return -1;
	}
	if (rename(temporary, path) != 0) {
		(void)unlink(temporary);
		return -1;
	}
	return send_status(fd, 0, NULL);
}

static int serve_receive(int fd, const struct ps4_scp_request *request) {
	char line[1024];
	char destination[PS4_SCP_PATH_MAX];
	struct stat target;
	int target_is_directory = stat(request->path, &target) == 0 && S_ISDIR(target.st_mode);
	int received = 0;
	if (request->target_should_be_directory && !target_is_directory) {
		(void)send_status(fd, 1, "scp: target is not a directory");
		return 1;
	}
	if (send_status(fd, 0, NULL) != 0) return 1;
	for (;;) {
		char *end;
		char *size_text;
		char *name;
		unsigned long mode;
		unsigned long long size;
		int line_status = read_line(fd, line, sizeof(line));
		if (line_status == 0) return 0;
		if (line_status < 0) return 1;
		if (line[0] == 'T') {
			if (send_status(fd, 0, NULL) != 0) return 1;
			continue;
		}
		if (line[0] != 'C') {
			(void)send_status(fd, 1, "scp: only regular files are supported");
			return 1;
		}
		errno = 0;
		mode = strtoul(line + 1, &end, 8);
		if (errno != 0 || end == line + 1 || *end != ' ') goto bad_control;
		size_text = end + 1;
		size = strtoull(size_text, &end, 10);
		if (errno != 0 || end == size_text || *end != ' ' || size > PS4_SCP_MAX_FILE_SIZE) goto bad_control;
		name = end + 1;
		end = strchr(name, '\n');
		if (end == NULL) goto bad_control;
		*end = '\0';
		if (!safe_name(name)) goto bad_control;
		if (target_is_directory) {
			if (snprintf(destination, sizeof(destination), "%s%s%s", request->path,
					strcmp(request->path, "/") == 0 ? "" : "/", name) >= (int)sizeof(destination)) goto bad_control;
		} else {
			if (received != 0) goto bad_control;
			(void)snprintf(destination, sizeof(destination), "%s", request->path);
		}
		if (receive_file(fd, destination, (unsigned int)mode, (uint64_t)size) != 0) return 1;
		received++;
		continue;
bad_control:
		(void)send_status(fd, 1, "scp: invalid control record");
		return 1;
	}
}

static int serve_send(int fd, const struct ps4_scp_request *request) {
	unsigned char buffer[4096];
	char control[PS4_SCP_PATH_MAX + 64];
	const char *name;
	struct stat st;
	uint64_t remaining;
	int input;
	unsigned char zero = 0;
	if (read_status(fd) != 0) return 1;
	if (stat(request->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
		(void)send_status(fd, 1, "scp: source is not a regular file");
		return 1;
	}
	if ((uint64_t)st.st_size > PS4_SCP_SAFE_SEND_MAX) {
		(void)send_status(fd, 1, "scp: downloads are limited to 8192 bytes; use GoldHEN FTP for larger files");
		return 1;
	}
	name = strrchr(request->path, '/');
	name = name == NULL ? request->path : name + 1;
	if (!safe_name(name)) return 1;
	if (snprintf(control, sizeof(control), "C%04o %llu %s\n",
			(unsigned int)(st.st_mode & 0777), (unsigned long long)st.st_size, name) >= (int)sizeof(control) ||
			write_all(fd, control, strlen(control)) != 0 || read_status(fd) != 0) return 1;
	input = open(request->path, O_RDONLY);
	if (input < 0) return 1;
	remaining = (uint64_t)st.st_size;
	while (remaining > 0) {
		size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
		ssize_t got = read(input, buffer, wanted);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0 || write_all(fd, buffer, (size_t)got) != 0) {
			close(input);
			return 1;
		}
		remaining -= (uint64_t)got;
	}
	if (close(input) != 0 || write_all(fd, &zero, 1) != 0 || read_status(fd) != 0) return 1;
	return 0;
}

int ps4_scp_serve(int fd, const struct ps4_scp_request *request) {
	return request->mode == PS4_SCP_RECEIVE ? serve_receive(fd, request) : serve_send(fd, request);
}
