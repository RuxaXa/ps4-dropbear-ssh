#include "ps4-runtime.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int fake_dropbear_main(int argc, char **argv, const char *multipath) {
    fprintf(stderr, "fake argc=%d argv0=%s argv1=%s argv2=%s argv3=%s argv4=%s argv5=%s multipath=%s\n",
            argc, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], multipath);
    return 37;
}

static int test_redirect(void) {
    char path[] = "/tmp/dropbear-ps4-runtime-test-XXXXXX";
    int seed = mkstemp(path);
    if (seed < 0) {
        perror("mkstemp");
        return 1;
    }
    close(seed);
    unlink(path);

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        if (ps4_redirect_stdio(path) != 0) {
            _exit(2);
        }
        puts("stdout-marker");
        fputs("stderr-marker\n", stderr);
        fflush(stdout);
        fflush(stderr);
        _exit(0);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child failed: status=%d\n", status);
        unlink(path);
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        perror("stat");
        return 1;
    }
    if ((st.st_mode & 0777) != 0600) {
        fprintf(stderr, "unexpected mode: %03o\n", st.st_mode & 0777);
        unlink(path);
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        unlink(path);
        return 1;
    }
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    unlink(path);

    if (n == 0 || strstr(buf, "stdout-marker") == NULL || strstr(buf, "stderr-marker") == NULL) {
        fprintf(stderr, "missing redirected markers: %s\n", buf);
        return 1;
    }
    return 0;
}

static int test_app_mode(void) {
    char path[] = "/tmp/dropbear-ps4-app-test-XXXXXX";
    int seed = mkstemp(path);
    if (seed < 0) {
        perror("mkstemp");
        return 1;
    }
    close(seed);
    unlink(path);

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        int rc = ps4_run_server_app("/app0/eboot.bin", path, fake_dropbear_main);
        fflush(stdout);
        fflush(stderr);
        _exit(rc == 37 ? 0 : 3);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "app child failed: status=%d\n", status);
        unlink(path);
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        unlink(path);
        return 1;
    }
    char buf[2048] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    unlink(path);

    const char *expected[] = {
        "ps4-dropbear: starting",
        "fake argc=6 argv0=dropbear argv1=-F argv2=-E argv3=-R argv4=-p argv5=2222 multipath=/app0/eboot.bin",
        "ps4-dropbear: returned rc=37"
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (n == 0 || strstr(buf, expected[i]) == NULL) {
            fprintf(stderr, "missing app marker '%s': %s\n", expected[i], buf);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    if (test_redirect() != 0) {
        return 1;
    }
    return test_app_mode();
}
