# Verification record

## Source

Canonical host verification:

```sh
python3 scripts/audit-public-tree.py
./dropbear/tests/run_ps4_shell_tests.sh
```

The tests compile with `-Wall -Wextra -Werror` and cover the PS4 shell, SCP and runtime modules.

A clean PS4 cross-build from the sanitized source was performed twice with OpenOrbis PS4 Toolchain v0.5.4 and Debian clang/LLD 19.1.7. Both builds produced the same source-build ELF:

- size: `539832` bytes
- SHA-256: `193658e02e10ec9658fe5e2aedfc3a224d75a6f01a1bfe7d437c2d2b63d743ac`

That ELF is a clean public-source verification build, not the historical reboot-tested Build 57 FSELF representation.

## Historical PKG

The release package was independently revalidated and freshly extracted using OpenOrbis `PkgTool.Core` with its isolated compatible OpenSSL 1.1 runtime:

- `pkg_validate --verbose`: PASS
- `pkg_extract --verbose`: PASS
- extracted files matched the archived verification hashes byte-for-byte

Package identity:

- size: `6619136` bytes
- SHA-256: `a711007389a011d8d6d2b01e075e01767fe9dc47981ede26b04aa9ee749392f6`

See `RELEASE-v1.07.md` for every extracted file hash and the historical provenance limitation.
