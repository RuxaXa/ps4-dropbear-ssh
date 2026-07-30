#include <time.h>
#include <stdint.h>
#include <sys/time.h>
#include <orbis/libkernel.h>

int __clock_gettime(clockid_t clk_id, struct timespec *tp) {
    return clock_gettime(clk_id, tp);
}

/* The regular PS4 app sandbox rejects the libc gettimeofday syscall path.
 * Process time is sufficient for Dropbear timing/seed-mixing uses;
 * cryptographic entropy is obtained independently. */
int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
    uint64_t usec;
    (void)tz;
    if (tv == NULL) {
        return -1;
    }
    usec = sceKernelGetProcessTime();
    tv->tv_sec = (time_t)(usec / 1000000ULL);
    tv->tv_usec = (suseconds_t)(usec % 1000000ULL);
    return 0;
}

typedef struct OrbisNetDnsInfoCompat {
    uint32_t primary_dns;
    uint32_t secondary_dns;
} OrbisNetDnsInfoCompat;

int sceNetGetDnsInfo(OrbisNetDnsInfoCompat *info, int flags) {
    (void)flags;
    if (info != NULL) {
        info->primary_dns = 0;
        info->secondary_dns = 0;
    }
    return -1;
}
