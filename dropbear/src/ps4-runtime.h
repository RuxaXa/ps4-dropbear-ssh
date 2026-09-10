#ifndef DROPBEAR_PS4_RUNTIME_H_
#define DROPBEAR_PS4_RUNTIME_H_

#include <sys/types.h>

typedef int (*ps4_server_main_fn)(int argc, char **argv, const char *multipath);

int ps4_redirect_stdio(const char *path);
int ps4_run_server_app(const char *argv0, const char *log_path, ps4_server_main_fn server_main);

/* OpenOrbis libc's fork wrapper is incorrect for this target. */
pid_t ps4_fork(void);
void ps4_runtime_errno_marker(const char *stage, int err);

/* Direct kernel entry points for process creation. The OpenOrbis libc
 * wrappers for these calls are not reliable on this target either. */
int ps4_execv(const char *path, char *const argv[]);
pid_t ps4_waitpid(pid_t pid, int *status);
void ps4_exit(int code) __attribute__((noreturn));

#endif
