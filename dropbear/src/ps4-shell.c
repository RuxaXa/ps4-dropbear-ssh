#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "ps4-shell.h"
#include "ps4-runtime.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct ps4_shell_args {
	char storage[2048];
	char *argv[PS4_SHELL_MAX_ARGS];
	int argc;
};

struct ps4_shell_entry {
	char *name;
};

static char *shell_strdup(const char *value) {
	size_t len = strlen(value) + 1;
	char *copy = malloc(len);
	if (copy != NULL) {
		memcpy(copy, value, len);
	}
	return copy;
}

static int write_all(ps4_shell_write_fn writer, void *ctx,
		const void *data, size_t len) {
	return len == 0 ? 0 : writer(ctx, data, len);
}

static int write_fmt(ps4_shell_write_fn writer, void *ctx,
		const char *format, ...) {
	char buffer[2048];
	va_list args;
	int len;

	va_start(args, format);
	len = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (len < 0) {
		return -1;
	}
	return write_all(writer, ctx, buffer,
			(size_t)len < sizeof(buffer) ? (size_t)len : sizeof(buffer) - 1);
}

static int shell_error(ps4_shell_write_fn writer, void *ctx,
		const char *command, const char *path, int error) {
	if (write_fmt(writer, ctx, "%s: %s: %s\n", command, path,
			strerror(error)) != 0) {
		return -1;
	}
	return 1;
}

void ps4_shell_init(struct ps4_shell_state *state, const char *home) {
	if (home == NULL || *home == '\0') {
		home = "/";
	}
	(void)snprintf(state->home, sizeof(state->home), "%s", home);
	(void)snprintf(state->cwd, sizeof(state->cwd), "%s", home);
}

int ps4_shell_resolve(const char *cwd, const char *path,
		char *resolved, size_t resolved_size) {
	char combined[PS4_SHELL_PATH_MAX * 2];
	size_t combined_len;
	size_t input_pos = 0;
	size_t output_len = 1;

	if (path == NULL || *path == '\0') {
		path = cwd;
	}
	if (*path == '/') {
		combined_len = (size_t)snprintf(combined, sizeof(combined), "%s", path);
	} else {
		combined_len = (size_t)snprintf(combined, sizeof(combined), "%s/%s", cwd, path);
	}
	if (combined_len >= sizeof(combined) || resolved_size < 2) {
		errno = ENAMETOOLONG;
		return -1;
	}
	resolved[0] = '/';
	resolved[1] = '\0';
	while (combined[input_pos] != '\0') {
		size_t start;
		size_t length;
		while (combined[input_pos] == '/') {
			input_pos++;
		}
		if (combined[input_pos] == '\0') {
			break;
		}
		start = input_pos;
		while (combined[input_pos] != '\0' && combined[input_pos] != '/') {
			input_pos++;
		}
		length = input_pos - start;
		if (length == 1 && combined[start] == '.') {
			continue;
		}
		if (length == 2 && combined[start] == '.' && combined[start + 1] == '.') {
			while (output_len > 1 && resolved[output_len - 1] != '/') {
				output_len--;
			}
			if (output_len > 1) {
				output_len--;
			}
			resolved[output_len] = '\0';
			continue;
		}
		if (output_len > 1) {
			if (output_len + 1 >= resolved_size) {
				errno = ENAMETOOLONG;
				return -1;
			}
			resolved[output_len++] = '/';
		}
		if (output_len + length >= resolved_size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(resolved + output_len, combined + start, length);
		output_len += length;
		resolved[output_len] = '\0';
	}
	return 0;
}

static int split_command(const char *command, struct ps4_shell_args *args) {
	char *out = args->storage;
	const char *in = command;
	size_t remaining = sizeof(args->storage);
	args->argc = 0;

	while (*in != '\0') {
		char quote = '\0';
		while (*in == ' ' || *in == '\t') {
			in++;
		}
		if (*in == '\0') {
			break;
		}
		if (args->argc == PS4_SHELL_MAX_ARGS || remaining < 2) {
			errno = E2BIG;
			return -1;
		}
		args->argv[args->argc++] = out;
		while (*in != '\0') {
			char ch = *in++;
			if (quote == '\0' && (ch == ' ' || ch == '\t')) {
				break;
			}
			if (ch == '\\' && quote != '\'') {
				if (*in == '\0') {
					errno = EINVAL;
					return -1;
				}
				ch = *in++;
			} else if (ch == '\'' || ch == '"') {
				if (quote == '\0') {
					quote = ch;
					continue;
				}
				if (quote == ch) {
					quote = '\0';
					continue;
				}
			}
			if (remaining < 2) {
				errno = E2BIG;
				return -1;
			}
			*out++ = ch;
			remaining--;
		}
		if (quote != '\0') {
			errno = EINVAL;
			return -1;
		}
		*out++ = '\0';
		remaining--;
	}
	return 0;
}

static int resolve_arg(const struct ps4_shell_state *state, const char *arg,
		char path[PS4_SHELL_PATH_MAX]) {
	return ps4_shell_resolve(state->cwd, arg, path, PS4_SHELL_PATH_MAX);
}

static int command_run(struct ps4_shell_state *state,
		const struct ps4_shell_args *args, ps4_shell_write_fn writer, void *ctx) {
	char path[PS4_SHELL_PATH_MAX];
	char *child_argv[PS4_SHELL_MAX_ARGS + 1];
	pid_t pid;
	int status = 0;
	int count;
	size_t index;

	if (args->argc < 2) {
		return shell_error(writer, ctx, "run", "(no file)", EINVAL);
	}
	if (resolve_arg(state, args->argv[1], path) != 0) {
		return shell_error(writer, ctx, "run", args->argv[1], errno);
	}
	count = args->argc - 1;
	if (count > PS4_SHELL_MAX_ARGS) {
		count = PS4_SHELL_MAX_ARGS;
	}
	for (index = 1; index <= (size_t)count; index++) {
		child_argv[index - 1] = args->argv[index];
	}
	child_argv[count] = NULL;

	pid = ps4_fork();
	if (pid < 0) {
		return shell_error(writer, ctx, "run", args->argv[1], errno);
	}
	if (pid == 0) {
		ps4_execv(path, child_argv);
		ps4_exit(127);
	}
	if (ps4_waitpid(pid, &status) < 0) {
		return shell_error(writer, ctx, "run", args->argv[1], errno);
	}
	return write_fmt(writer, ctx, "run: %s exit=%d\n", args->argv[1],
			(status >> 8) & 0xff) == 0 ? 0 : -1;
}

int ps4_shell_handles(const char *command) {
	static const char *const commands[] = {
		"pwd", "cd", "ls", "cat", "head", "tail", "stat",
		"touch", "mkdir", "rmdir", "rm", "cp", "mv", "chmod",
		"grep", "hexdump", "find", "run"
	};
	struct ps4_shell_args args;
	size_t index;

	if (split_command(command, &args) != 0 || args.argc == 0) {
		return 0;
	}
	for (index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
		if (strcmp(args.argv[0], commands[index]) == 0) {
			return 1;
		}
	}
	return 0;
}

static int entry_compare(const void *left, const void *right) {
	const struct ps4_shell_entry *a = left;
	const struct ps4_shell_entry *b = right;
	return strcmp(a->name, b->name);
}

static int command_ls(struct ps4_shell_state *state,
		const struct ps4_shell_args *args, ps4_shell_write_fn writer, void *ctx) {
	struct ps4_shell_entry *entries = NULL;
	struct dirent *entry;
	char path[PS4_SHELL_PATH_MAX];
	const char *target = ".";
	size_t count = 0;
	size_t capacity = 0;
	int show_hidden = 0;
	int long_format = 0;
	int index;
	DIR *directory;
	int result = 0;

	for (index = 1; index < args->argc; index++) {
		if (args->argv[index][0] != '-') {
			target = args->argv[index];
			continue;
		}
		if (strchr(args->argv[index] + 1, 'a') != NULL) {
			show_hidden = 1;
		}
		if (strchr(args->argv[index] + 1, 'l') != NULL) {
			long_format = 1;
		}
		if (strspn(args->argv[index] + 1, "al") != strlen(args->argv[index] + 1)) {
			return write_fmt(writer, ctx, "ls: unsupported option: %s\n",
					args->argv[index]) == 0 ? 1 : -1;
		}
	}
	if (resolve_arg(state, target, path) != 0) {
		return shell_error(writer, ctx, "ls", target, errno);
	}
	directory = opendir(path);
	if (directory == NULL) {
		return shell_error(writer, ctx, "ls", target, errno);
	}
	while ((entry = readdir(directory)) != NULL) {
		struct ps4_shell_entry *grown;
		if (!show_hidden && entry->d_name[0] == '.') {
			continue;
		}
		if (count == capacity) {
			capacity = capacity == 0 ? 32 : capacity * 2;
			grown = realloc(entries, capacity * sizeof(*entries));
			if (grown == NULL) {
				result = -1;
				goto out;
			}
			entries = grown;
		}
		entries[count].name = shell_strdup(entry->d_name);
		if (entries[count].name == NULL) {
			result = -1;
			goto out;
		}
		count++;
	}
	qsort(entries, count, sizeof(*entries), entry_compare);
	for (size_t item = 0; item < count; item++) {
		struct stat st;
		char full[PS4_SHELL_PATH_MAX];
		int is_directory = 0;
		if (snprintf(full, sizeof(full), "%s%s%s", path,
				strcmp(path, "/") == 0 ? "" : "/", entries[item].name) >=
				(int)sizeof(full)) {
			result = -1;
			goto out;
		}
		if (stat(full, &st) == 0) {
			is_directory = S_ISDIR(st.st_mode);
		}
		if (long_format && stat(full, &st) == 0) {
			if (write_fmt(writer, ctx, "%c%04o %10lld %s%s\n",
					is_directory ? 'd' : '-', (unsigned int)(st.st_mode & 07777),
					(long long)st.st_size, entries[item].name,
					is_directory ? "/" : "") != 0) {
				result = -1;
				goto out;
			}
		} else if (write_fmt(writer, ctx, "%s%s\n", entries[item].name,
				is_directory ? "/" : "") != 0) {
			result = -1;
			goto out;
		}
	}
out:
	closedir(directory);
	for (size_t item = 0; item < count; item++) {
		free(entries[item].name);
	}
	free(entries);
	return result;
}

static int command_cat(struct ps4_shell_state *state, const char *name,
		ps4_shell_write_fn writer, void *ctx) {
	unsigned char buffer[16384];
	char path[PS4_SHELL_PATH_MAX];
	ssize_t got;
	int fd;
	if (resolve_arg(state, name, path) != 0) {
		return shell_error(writer, ctx, "cat", name, errno);
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return shell_error(writer, ctx, "cat", name, errno);
	}
	while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
		if (write_all(writer, ctx, buffer, (size_t)got) != 0) {
			close(fd);
			return -1;
		}
	}
	if (got < 0) {
		int error = errno;
		close(fd);
		return shell_error(writer, ctx, "cat", name, error);
	}
	close(fd);
	return 0;
}

static int command_head(struct ps4_shell_state *state, const char *name, int lines,
		ps4_shell_write_fn writer, void *ctx) {
	unsigned char buffer[4096];
	char path[PS4_SHELL_PATH_MAX];
	ssize_t got;
	int fd;
	if (resolve_arg(state, name, path) != 0) {
		return shell_error(writer, ctx, "head", name, errno);
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return shell_error(writer, ctx, "head", name, errno);
	}
	while (lines > 0 && (got = read(fd, buffer, sizeof(buffer))) > 0) {
		size_t length = (size_t)got;
		for (size_t i = 0; i < (size_t)got; i++) {
			if (buffer[i] == '\n' && --lines == 0) {
				length = i + 1;
				break;
			}
		}
		if (write_all(writer, ctx, buffer, length) != 0) {
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}

static int command_tail(struct ps4_shell_state *state, const char *name, int lines,
		ps4_shell_write_fn writer, void *ctx) {
	char path[PS4_SHELL_PATH_MAX];
	unsigned char *data;
	struct stat st;
	size_t start = 0;
	ssize_t got;
	int fd;
	if (resolve_arg(state, name, path) != 0) {
		return shell_error(writer, ctx, "tail", name, errno);
	}
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) != 0) {
		int error = errno;
		if (fd >= 0) close(fd);
		return shell_error(writer, ctx, "tail", name, error);
	}
	if (st.st_size < 0 || st.st_size > 16 * 1024 * 1024) {
		close(fd);
		return shell_error(writer, ctx, "tail", name, EFBIG);
	}
	data = malloc((size_t)st.st_size + 1);
	if (data == NULL) {
		close(fd);
		return -1;
	}
	got = read(fd, data, (size_t)st.st_size);
	close(fd);
	if (got < 0) {
		int error = errno;
		free(data);
		return shell_error(writer, ctx, "tail", name, error);
	}
	for (size_t i = (size_t)got; i > 0; i--) {
		if (data[i - 1] == '\n' && i != (size_t)got && --lines == 0) {
			start = i;
			break;
		}
	}
	if (lines > 0) start = 0;
	if (write_all(writer, ctx, data + start, (size_t)got - start) != 0) {
		free(data);
		return -1;
	}
	free(data);
	return 0;
}

static int copy_file(const char *source, const char *destination) {
	unsigned char buffer[16384];
	struct stat st;
	ssize_t got;
	int input;
	int output;
	input = open(source, O_RDONLY);
	if (input < 0 || fstat(input, &st) != 0 || !S_ISREG(st.st_mode)) {
		if (input >= 0) close(input);
		errno = EINVAL;
		return -1;
	}
	output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
	if (output < 0) {
		close(input);
		return -1;
	}
	while ((got = read(input, buffer, sizeof(buffer))) > 0) {
		size_t offset = 0;
		while (offset < (size_t)got) {
			ssize_t written = write(output, buffer + offset, (size_t)got - offset);
			if (written < 0 && errno == EINTR) continue;
			if (written <= 0) {
				close(input); close(output); return -1;
			}
			offset += (size_t)written;
		}
	}
	if (close(input) != 0 || close(output) != 0 || got < 0) return -1;
	return 0;
}

static int parse_line_count(const struct ps4_shell_args *args, int *lines,
		const char **name) {
	*lines = 10;
	if (args->argc == 2) {
		*name = args->argv[1];
		return 0;
	}
	if (args->argc == 4 && strcmp(args->argv[1], "-n") == 0) {
		char *end;
		long value = strtol(args->argv[2], &end, 10);
		if (*end == '\0' && value >= 0 && value <= 100000) {
			*lines = (int)value;
			*name = args->argv[3];
			return 0;
		}
	}
	return -1;
}

static int command_grep(struct ps4_shell_state *state, const char *needle,
		const char *name, ps4_shell_write_fn writer, void *ctx) {
	char path[PS4_SHELL_PATH_MAX];
	char *line = malloc(65536);
	size_t used = 0;
	int matched = 0;
	int fd;
	char byte;
	ssize_t got;

	if (line == NULL) return -1;
	if (resolve_arg(state, name, path) != 0 || (fd = open(path, O_RDONLY)) < 0) {
		int error = errno;
		free(line);
		return shell_error(writer, ctx, "grep", name, error);
	}
	while ((got = read(fd, &byte, 1)) > 0) {
		if (used == 65535) {
			close(fd); free(line);
			return shell_error(writer, ctx, "grep", name, EOVERFLOW);
		}
		line[used++] = byte;
		if (byte == '\n') {
			line[used] = '\0';
			if (strstr(line, needle) != NULL) {
				if (write_all(writer, ctx, line, used) != 0) { close(fd); free(line); return -1; }
				matched = 1;
			}
			used = 0;
		}
	}
	if (got < 0) { int error = errno; close(fd); free(line); return shell_error(writer, ctx, "grep", name, error); }
	if (used > 0) {
		line[used] = '\0';
		if (strstr(line, needle) != NULL) {
			if (write_all(writer, ctx, line, used) != 0 || write_all(writer, ctx, "\n", 1) != 0) { close(fd); free(line); return -1; }
			matched = 1;
		}
	}
	close(fd); free(line);
	return matched ? 0 : 1;
}

static int command_hexdump(struct ps4_shell_state *state, const char *name,
		uint64_t limit, ps4_shell_write_fn writer, void *ctx) {
	unsigned char data[16];
	char path[PS4_SHELL_PATH_MAX];
	uint64_t offset = 0;
	int fd;
	if (resolve_arg(state, name, path) != 0 || (fd = open(path, O_RDONLY)) < 0)
		return shell_error(writer, ctx, "hexdump", name, errno);
	while (offset < limit) {
		size_t wanted = limit - offset < sizeof(data) ? (size_t)(limit - offset) : sizeof(data);
		ssize_t got = read(fd, data, wanted);
		if (got < 0 && errno == EINTR) continue;
		if (got < 0) { int error = errno; close(fd); return shell_error(writer, ctx, "hexdump", name, error); }
		if (got == 0) break;
		if (write_fmt(writer, ctx, "%08llx  ", (unsigned long long)offset) != 0) { close(fd); return -1; }
		for (ssize_t i = 0; i < got; i++) if (write_fmt(writer, ctx, "%02x%s", data[i], i + 1 == got ? "" : " ") != 0) { close(fd); return -1; }
		if (write_all(writer, ctx, "\n", 1) != 0) { close(fd); return -1; }
		offset += (uint64_t)got;
	}
	close(fd);
	return 0;
}

static int command_find_path(const char *path, int depth,
		ps4_shell_write_fn writer, void *ctx) {
	struct stat st;
	DIR *directory;
	struct dirent *entry;
	if (stat(path, &st) != 0) return shell_error(writer, ctx, "find", path, errno);
	if (write_fmt(writer, ctx, "%s%s\n", path, S_ISDIR(st.st_mode) ? "/" : "") != 0) return -1;
	if (!S_ISDIR(st.st_mode) || depth >= 32) return 0;
	directory = opendir(path);
	if (directory == NULL) return shell_error(writer, ctx, "find", path, errno);
	while ((entry = readdir(directory)) != NULL) {
		char child[PS4_SHELL_PATH_MAX];
		int result;
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		if (snprintf(child, sizeof(child), "%s%s%s", path, strcmp(path, "/") == 0 ? "" : "/", entry->d_name) >= (int)sizeof(child)) { closedir(directory); return -1; }
		result = command_find_path(child, depth + 1, writer, ctx);
		if (result != 0) { closedir(directory); return result; }
	}
	closedir(directory);
	return 0;
}

int ps4_shell_execute(struct ps4_shell_state *state, const char *command,
		ps4_shell_write_fn writer, void *writer_ctx) {
	struct ps4_shell_args args;
	char first[PS4_SHELL_PATH_MAX];
	char second[PS4_SHELL_PATH_MAX];
	struct stat st;
	const char *name;
	int lines;

	if (split_command(command, &args) != 0) {
		return shell_error(writer, writer_ctx, "shell", command, errno);
	}
	if (args.argc == 0) return 0;
	if (strcmp(args.argv[0], "pwd") == 0 && args.argc == 1) {
		return write_fmt(writer, writer_ctx, "%s\n", state->cwd) == 0 ? 0 : -1;
	}
	if (strcmp(args.argv[0], "cd") == 0 && args.argc <= 2) {
		name = args.argc == 1 ? state->home : args.argv[1];
		if (resolve_arg(state, name, first) != 0 || stat(first, &st) != 0)
			return shell_error(writer, writer_ctx, "cd", name, errno);
		if (!S_ISDIR(st.st_mode)) return shell_error(writer, writer_ctx, "cd", name, ENOTDIR);
		(void)snprintf(state->cwd, sizeof(state->cwd), "%s", first);
		return 0;
	}
	if (strcmp(args.argv[0], "ls") == 0) return command_ls(state, &args, writer, writer_ctx);
	if (strcmp(args.argv[0], "cat") == 0 && args.argc == 2)
		return command_cat(state, args.argv[1], writer, writer_ctx);
	if (strcmp(args.argv[0], "head") == 0 && parse_line_count(&args, &lines, &name) == 0)
		return command_head(state, name, lines, writer, writer_ctx);
	if (strcmp(args.argv[0], "tail") == 0 && parse_line_count(&args, &lines, &name) == 0)
		return command_tail(state, name, lines, writer, writer_ctx);
	if (strcmp(args.argv[0], "mkdir") == 0 && args.argc == 2) {
		if (resolve_arg(state, args.argv[1], first) == 0 && mkdir(first, 0755) == 0) return 0;
		return shell_error(writer, writer_ctx, "mkdir", args.argv[1], errno);
	}
	if (strcmp(args.argv[0], "rmdir") == 0 && args.argc == 2) {
		if (resolve_arg(state, args.argv[1], first) == 0 && rmdir(first) == 0) return 0;
		return shell_error(writer, writer_ctx, "rmdir", args.argv[1], errno);
	}
	if (strcmp(args.argv[0], "touch") == 0 && args.argc == 2) {
		int fd;
		if (resolve_arg(state, args.argv[1], first) != 0) return shell_error(writer, writer_ctx, "touch", args.argv[1], errno);
		fd = open(first, O_WRONLY | O_CREAT, 0644);
		if (fd >= 0 && close(fd) == 0) return 0;
		return shell_error(writer, writer_ctx, "touch", args.argv[1], errno);
	}
	if (strcmp(args.argv[0], "rm") == 0 && args.argc == 2) {
		if (resolve_arg(state, args.argv[1], first) == 0 && strcmp(first, "/") != 0 && unlink(first) == 0) return 0;
		return shell_error(writer, writer_ctx, "rm", args.argv[1], errno == 0 ? EPERM : errno);
	}
	if ((strcmp(args.argv[0], "cp") == 0 || strcmp(args.argv[0], "mv") == 0) && args.argc == 3) {
		if (resolve_arg(state, args.argv[1], first) != 0 || resolve_arg(state, args.argv[2], second) != 0)
			return shell_error(writer, writer_ctx, args.argv[0], args.argv[1], errno);
		if (strcmp(args.argv[0], "cp") == 0 ? copy_file(first, second) == 0 : rename(first, second) == 0) return 0;
		return shell_error(writer, writer_ctx, args.argv[0], args.argv[1], errno);
	}
	if (strcmp(args.argv[0], "chmod") == 0 && args.argc == 3) {
		char *end;
		unsigned long mode = strtoul(args.argv[1], &end, 8);
		if (*end == '\0' && mode <= 07777 && resolve_arg(state, args.argv[2], first) == 0 &&
				chmod(first, (mode_t)mode) == 0) return 0;
		return shell_error(writer, writer_ctx, "chmod", args.argv[2], errno == 0 ? EINVAL : errno);
	}
	if (strcmp(args.argv[0], "grep") == 0 && args.argc == 3)
		return command_grep(state, args.argv[1], args.argv[2], writer, writer_ctx);
	if (strcmp(args.argv[0], "hexdump") == 0) {
		uint64_t limit = ULLONG_MAX;
		if (args.argc == 2) name = args.argv[1];
		else if (args.argc == 4 && strcmp(args.argv[1], "-n") == 0) {
			char *end;
			unsigned long long parsed = strtoull(args.argv[2], &end, 10);
			if (*end != '\0') goto invalid;
			limit = (uint64_t)parsed;
			name = args.argv[3];
		} else goto invalid;
		return command_hexdump(state, name, limit, writer, writer_ctx);
	}
	if (strcmp(args.argv[0], "find") == 0 && args.argc <= 2) {
		name = args.argc == 2 ? args.argv[1] : ".";
		if (resolve_arg(state, name, first) != 0) return shell_error(writer, writer_ctx, "find", name, errno);
		return command_find_path(first, 0, writer, writer_ctx);
	}
	if (strcmp(args.argv[0], "stat") == 0 && args.argc == 2) {
		if (resolve_arg(state, args.argv[1], first) != 0 || stat(first, &st) != 0)
			return shell_error(writer, writer_ctx, "stat", args.argv[1], errno);
		return write_fmt(writer, writer_ctx, "path: %s\ntype: %s\nmode: %04o\nsize: %lld\nmtime: %lld\n",
				first, S_ISDIR(st.st_mode) ? "directory" : S_ISREG(st.st_mode) ? "file" : "other",
				(unsigned int)(st.st_mode & 07777), (long long)st.st_size,
				(long long)st.st_mtime) == 0 ? 0 : -1;
	}
	if (strcmp(args.argv[0], "run") == 0 && args.argc >= 2) {
		return command_run(state, &args, writer, writer_ctx);
	}
invalid:
	if (write_fmt(writer, writer_ctx, "%s: command not found or invalid arguments\n", args.argv[0]) != 0) return -1;
	return 127;
}
