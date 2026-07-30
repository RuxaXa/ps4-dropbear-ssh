# Build 57 provenance

## Immutable identities

- Dropbear upstream commit: `ee65bff1567576a223febcdd5ae552326a4da4b1`
- Verified upstream tree: `1e62cc9d1b1664baf54a89a1088cf190f465686c`
- Upstream parent: `962e5ae1747c2b173fc370fe724bbda090661a50`
- Upstream author date: `2026-05-19T11:03:39Z`
- Dropbear source/banner version: `2026.91`
- PS4 app label: v1.07
- Internal build number: 57
- Reboot-tested Build 57 payload SHA-256: `7e3e8406daecc81e9e391e4dd44c9ed3a198b75f29e011843b04f20ad151e5ce`
- Historical v1.07 PKG size: `6619136` bytes
- Historical v1.07 PKG SHA-256: `a711007389a011d8d6d2b01e075e01767fe9dc47981ede26b04aa9ee749392f6`

The payload and PKG are not included. The exact cryptographic transformation chain from source to ELF/FSELF/PKG and from package payload to the installed console `eboot.bin` was not completely frozen for the historical artifact; the hashes must not be treated as proving that missing chain.

`build57-working-tree.patch` records modifications to files tracked by the upstream checkout when the baseline was frozen. The previously untracked PS4 runtime, shell, SCP and test sources are included directly under `dropbear-ps4/`. The public `Makefile.in`, build wrapper and example options make that source buildable from a fresh checkout but do not retroactively prove the missing historical packaging chain.

## Deliberate artifact policy

The tested console binary, package, host keys and console installation data are not part of this public repository. This directory contains only hashes and source-level provenance.

## Known source-snapshot caveat

The original baseline helper archive omitted the then-untracked `ps4-runtime.c/.h` files. They remained present in the active Build 57 source checkout and are included in the public source tree. The separately frozen shell, SCP and test files were byte-identical to the active checkout during release preparation.

The generated Build 57 Makefile linked all three PS4 objects. The public `Makefile.in` was corrected to represent that tested object graph; see `dropbear-ps4/PS4_PORT.md`.
