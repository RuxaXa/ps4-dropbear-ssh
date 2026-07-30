#ifndef DROPBEAR_PS4_SHELL_H_
#define DROPBEAR_PS4_SHELL_H_

#include <stddef.h>

#define PS4_SHELL_PATH_MAX 1024
#define PS4_SHELL_MAX_ARGS 16

typedef int (*ps4_shell_write_fn)(void *ctx, const void *data, size_t len);

struct ps4_shell_state {
	char cwd[PS4_SHELL_PATH_MAX];
	char home[PS4_SHELL_PATH_MAX];
};

void ps4_shell_init(struct ps4_shell_state *state, const char *home);
int ps4_shell_resolve(const char *cwd, const char *path,
		char *resolved, size_t resolved_size);
int ps4_shell_handles(const char *command);
int ps4_shell_execute(struct ps4_shell_state *state, const char *command,
		ps4_shell_write_fn writer, void *writer_ctx);

#endif
