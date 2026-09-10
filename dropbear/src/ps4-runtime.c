#include "ps4-runtime.h"

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

pid_t ps4_fork(void) {
#ifdef __ORBIS__
    unsigned long ret;
    unsigned char iserror;

    /* FreeBSD/Orbis SYS_fork. Calling the kernel directly avoids the broken
     * OpenOrbis libc wrapper; this is the same mechanism used by the proven
     * Tiny-Shell server on this console. */
    __asm__ __volatile__(
            "syscall"
            : "=a"(ret), "=@ccc"(iserror)
            : "a"(2L)
            : "rcx", "r11", "memory");
    if (iserror) {
        errno = (int)ret;
        return (pid_t)-1;
    }
    return (pid_t)ret;
#else
    return fork();
#endif
}

int ps4_execv(const char *path, char *const argv[]) {
#ifdef __ORBIS__
    unsigned long ret;
    unsigned char iserror;

    /* FreeBSD/Orbis SYS_execve. Replaces the calling process image. */
    __asm__ __volatile__(
            "syscall"
            : "=a"(ret), "=@ccc"(iserror)
            : "a"(59L), "D"(path), "S"(argv), "d"((void *)0)
            : "rcx", "r11", "memory");
    if (iserror) {
        errno = (int)ret;
    }
    return -1;
#else
    execv(path, argv);
    return -1;
#endif
}

pid_t ps4_waitpid(pid_t pid, int *status) {
#ifdef __ORBIS__
    unsigned long ret;
    unsigned char iserror;
    /* Das vierte Argument des x86-64-Syscall-ABI reist in r10. */
    register long syscall_arg4 __asm__("r10") = 0;

    /* FreeBSD/Orbis SYS_wait4(pid, status, options, rusage). */
    __asm__ __volatile__(
            "syscall"
            : "=a"(ret), "=@ccc"(iserror)
            : "a"(7L), "D"((long)pid), "S"(status), "d"(0L), "r"(syscall_arg4)
            : "rcx", "r11", "memory");
    if (iserror) {
        errno = (int)ret;
        return (pid_t)-1;
    }
    return (pid_t)ret;
#else
    return waitpid(pid, status, 0);
#endif
}

void ps4_exit(int code) {
#ifdef __ORBIS__
    unsigned long ret;
    unsigned char iserror;

    /* FreeBSD/Orbis SYS_exit. Never returns. */
    __asm__ __volatile__(
            "syscall"
            : "=a"(ret), "=@ccc"(iserror)
            : "a"(1L), "D"((long)code)
            : "rcx", "r11", "memory");
    (void)ret;
    (void)iserror;
#endif
    _exit(code);
}

static void ps4_diag_marker(int fd, const char *marker, size_t marker_len) {
    /* Use only write(2) here: stdio is exactly what this diagnostic probes. */
    (void)write(fd, marker, marker_len);
}

void ps4_runtime_errno_marker(const char *stage, int err) {
#ifdef __ORBIS__
    char marker[128];
    int len = snprintf(marker, sizeof(marker), "%s errno=%d\n", stage, err);
    int fd;

    if (len <= 0) {
        return;
    }
    if ((size_t)len >= sizeof(marker)) {
        len = (int)sizeof(marker) - 1;
    }
    fd = open("/data/dropbear/dropbear-runtime.log", O_WRONLY | O_APPEND);
    if (fd >= 0) {
        ps4_diag_marker(fd, marker, (size_t)len);
        close(fd);
    }
#else
    (void)stage;
    (void)err;
#endif
}

int ps4_redirect_stdio(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    ps4_diag_marker(fd, "open-ok\n", sizeof("open-ok\n") - 1);

    if (fchmod(fd, 0600) != 0) {
        ps4_diag_marker(fd, "fchmod-fail\n", sizeof("fchmod-fail\n") - 1);
        close(fd);
        return -1;
    }
    ps4_diag_marker(fd, "fchmod-ok\n", sizeof("fchmod-ok\n") - 1);

    if (dup2(fd, STDOUT_FILENO) < 0) {
        ps4_diag_marker(fd, "dup2-stdout-fail\n", sizeof("dup2-stdout-fail\n") - 1);
        close(fd);
        return -1;
    }
    ps4_diag_marker(fd, "dup2-stdout-ok\n", sizeof("dup2-stdout-ok\n") - 1);

    if (dup2(fd, STDERR_FILENO) < 0) {
        ps4_diag_marker(fd, "dup2-stderr-fail\n", sizeof("dup2-stderr-fail\n") - 1);
        close(fd);
        return -1;
    }
    ps4_diag_marker(fd, "dup2-stderr-ok\n", sizeof("dup2-stderr-ok\n") - 1);

    if (fd > STDERR_FILENO) {
        close(fd);
    }
    return 0;
}

int ps4_run_server_app(const char *argv0, const char *log_path, ps4_server_main_fn server_main) {
#ifdef __ORBIS__
    char *ps4_argv[] = {"dropbear", "-F", "-R", "-p", "0.0.0.0:2222",
            "-D", "/data/dropbear/.ssh", NULL};
    int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ps4_diag_marker(fd, "starting-without-stdio-redirect\n",
                sizeof("starting-without-stdio-redirect\n") - 1);
        close(fd);
    }
    return server_main(7, ps4_argv, argv0);
#else
    char *ps4_argv[] = {"dropbear", "-F", "-E", "-R", "-p", "2222", NULL};
    if (ps4_redirect_stdio(log_path) != 0) {
        return -1;
    }
    fprintf(stderr, "ps4-dropbear: starting\n");
    fflush(stderr);
    int rc = server_main(6, ps4_argv, argv0);
    fprintf(stderr, "ps4-dropbear: returned rc=%d\n", rc);
    fflush(stderr);
    return rc;
#endif
}
