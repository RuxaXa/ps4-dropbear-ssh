# Build and verification

## Host regression tests

```sh
./tests/run_ps4_shell_tests.sh
```

The host tests compile with `-Wall -Wextra -Werror` and exercise the portable in-process shell and SCP implementation. Target behavior still requires separate PS4 tests.

## PS4 cross-build prerequisites

- OpenOrbis PS4 Toolchain v0.5.4-compatible tree;
- Clang/LLVM and `ld.lld` capable of `x86_64-scei-ps4-elf` output;
- a POSIX host with Autoconf/Make;
- environment variables pointing to those independently obtained tools.

No Sony SDK, Sony library or PS4 firmware file is included or required by this source repository.

## Configuration

```sh
cd dropbear-ps4
cp localoptions-ps4.example.h localoptions.h
export OO_PS4_TOOLCHAIN=/absolute/path/to/PS4Toolchain
export LLVM_BIN=/absolute/path/to/llvm/bin
export CC="$PWD/build-aux/ps4-clang-wrap.sh"

./configure \
  --host=x86_64-unknown-freebsd9 \
  --build=x86_64-pc-linux-gnu \
  --disable-zlib --disable-pam --disable-syslog \
  --disable-lastlog --disable-utmp --disable-utmpx \
  --disable-wtmp --disable-wtmpx --disable-loginfunc \
  --disable-pututline --disable-pututxline \
  --enable-bundled-libtom --disable-harden \
  ac_cv_func_getusershell=no \
  ac_cv_func_clearenv=no \
  ac_cv_func_daemon=no \
  ac_cv_func_basename=yes \
  ac_cv_func_dirname=yes \
  ac_cv_func_openpty=yes \
  ac_cv_func_fork=yes \
  ac_cv_func_vfork=yes \
  CC="$CC" \
  CFLAGS="-I$OO_PS4_TOOLCHAIN/include -I. -O2 -D__ORBIS__ -D_GNU_SOURCE" \
  LDFLAGS= LIBS=

make PROGRAMS=dropbear MULTI=1 -j2
```

The wrapper performs the final PS4 link with OpenOrbis startup objects and the local compatibility source in `build-aux/ps4_compat.c`.

## Reproducibility boundary

The repository can verify source tests and the cross-link process when the declared toolchain exists. It does not claim bit-for-bit reproduction of the private Build 57 binary because the original complete compiler/runtime package manifest was not frozen before that build. The binary hash is preserved as provenance, not as a promise that arbitrary LLVM/OpenOrbis revisions will reproduce identical bytes.

## Deployment boundary

Building is not deployment. Before any console installation, independently verify:

1. exact source commit and output hash;
2. ELF/FSELF entry chain;
3. package title/version and staged path;
4. absence of stale listener processes;
5. protocol-safe handshake rather than a raw TCP probe;
6. authentication, listener return and rollback artifact.
