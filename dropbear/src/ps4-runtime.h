#ifndef DROPBEAR_PS4_RUNTIME_H_
#define DROPBEAR_PS4_RUNTIME_H_

#include <sys/types.h>

typedef int (*ps4_server_main_fn)(int argc, char **argv, const char *multipath);

int ps4_redirect_stdio(const char *path);
int ps4_run_server_app(const char *argv0, const char *log_path, ps4_server_main_fn server_main);

/* OpenOrbis libc's fork wrapper is incorrect for this target. */
pid_t ps4_fork(void);
void ps4_runtime_errno_marker(const char *stage, int err);

#endif
