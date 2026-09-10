/* Copyright (C) 2026 DROPBEARSSH-PS4 contribution
   Derived from John Törnblom's tiny-ps4-shell installer (GPLv3-or-later),
   see main_install.c in this directory. GPLv3-or-later.

   Swap the *staged* Dropbear daemon under /system/vsh/app/BREW00010/ with a
   build from this repository.

   Why: on this console the home-screen tile launches the already-staged daemon
   ("Dropbear SSH ... gestartet - Port 2222"), so the packaged installer never
   runs on a tile tap and /system stays read-only for every other tool. This
   payload performs exactly the steps of install() from main_install.c
   (jailbreak -> root -> nmount(MNT_UPDATE) on /system -> copy files), but reads
   the payload files from SRC_DIR instead of app0, so it can be loaded by any
   payload loader (GoldHEN BinLoader, Orbis Toolbox, ...).

   Safety: writes exactly four files below DST_DIR and nothing else; each copy is
   verified by size before it is reported as successful. */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include <orbis/libkernel.h>

#include "kern_orbis.h"

#define BUFSIZE    (1024 * 8)
#define SRC_DIR    "/data/ps4-research/swap"
#define DST_DIR    "/system/vsh/app/BREW00010"

static void
orbis_notify(const char *fmt, ...) {
  OrbisNotificationRequest req;
  va_list args;

  bzero(&req, sizeof req);
  va_start(args, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, args);
  va_end(args);

  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}


static void
build_iovec(struct iovec **iov, int *iovlen, const char *name, const char *val) {
  int i;

  if (*iovlen < 0)
    return;

  i = *iovlen;
  *iov = realloc(*iov, sizeof(**iov) * (i + 2));
  if (*iov == NULL) {
    *iovlen = -1;
    return;
  }

  (*iov)[i].iov_base = strdup(name);
  (*iov)[i].iov_len = strlen(name) + 1;
  i++;

  (*iov)[i].iov_base = val ? strdup(val) : NULL;
  (*iov)[i].iov_len = val ? strlen(val) + 1 : 0;
  i++;

  *iovlen = i;
}


/* identical to main_install.c: nmount(/system, MNT_UPDATE) */
static int
mount_system(void) {
  struct iovec *iov = NULL;
  int iovlen = 0;

  build_iovec(&iov, &iovlen, "fstype", "exfatfs");
  build_iovec(&iov, &iovlen, "fspath", "/system");
  build_iovec(&iov, &iovlen, "from", "/dev/da0x4.crypt");
  build_iovec(&iov, &iovlen, "large", "yes");
  build_iovec(&iov, &iovlen, "timezone", "static");
  build_iovec(&iov, &iovlen, "async", NULL);
  build_iovec(&iov, &iovlen, "ignoreacl", NULL);

#define SYS_nmount 378
#define MNT_UPDATE 0x0000000000010000ULL

  return orbis_syscall(SYS_nmount, iov, iovlen, MNT_UPDATE);
}


static long
file_size(const char *path) {
  struct stat s;

  if (stat(path, &s))
    return -1;
  return (long)s.st_size;
}


static int
copy_file(const char *src_str, const char *dst_str) {
  int src, dst;
  char data[BUFSIZE];
  ssize_t nread;
  long expect, seen = 0;

  expect = file_size(src_str);
  if (expect <= 0)
    return -1;

  src = open(src_str, O_RDONLY, 0);
  if (src < 0)
    return -1;

  dst = open(dst_str, O_CREAT | O_WRONLY | O_TRUNC, 0755);
  if (dst < 0) {
    close(src);
    return -1;
  }

  while ((nread = read(src, data, sizeof data)) > 0) {
    ssize_t written = 0;
    while (written < nread) {
      ssize_t n = write(dst, data + written, nread - written);
      if (n <= 0) {
        close(src);
        close(dst);
        return -1;
      }
      written += n;
    }
    seen += nread;
  }
  close(src);
  close(dst);

  if (nread < 0 || seen != expect)
    return -1;
  if (file_size(dst_str) != expect)
    return -1;
  return 0;
}


int
main(void) {
  int fails = 0;

  orbis_notify("SWAP: starting (daemon -> %s)", DST_DIR);

  if (app_jailbreak()) {
    orbis_notify("SWAP: FAILED - jailbreak");
    return 1;
  }
  if (seteuid(0)) {
    orbis_notify("SWAP: FAILED - root");
    return 1;
  }
  if (mount_system()) {
    orbis_notify("SWAP: FAILED - remount /system (errno %d)", errno);
    return 1;
  }

  fails += !!copy_file(SRC_DIR "/daemon.bin", DST_DIR "/eboot.bin");
  fails += !!copy_file(SRC_DIR "/daemon.sfo", DST_DIR "/sce_sys/param.sfo");
  fails += !!copy_file(SRC_DIR "/libc.prx", DST_DIR "/sce_module/libc.prx");
  fails += !!copy_file(SRC_DIR "/libSceFios2.prx", DST_DIR "/sce_module/libSceFios2.prx");

  if (fails)
    orbis_notify("SWAP: FAILED - %d file(s) not replaced", fails);
  else
    orbis_notify("SWAP: OK - daemon replaced, restart Dropbear");

  return fails ? 1 : 0;
}
