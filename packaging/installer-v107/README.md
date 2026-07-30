# v1.07 packaging source

This directory preserves the installer/package recipe associated with the historical v1.07 package family.

## Components

- `main_install.c` — first-launch installer
- `kern_orbis.c` / `kern_orbis.h` — OpenOrbis kernel helper used by the installer
- `install.gp4` — package file map
- `Makefile.v107` — exact historical v1.07 recipe snapshot
- `icon0.png` — RuxaXa Dropbear PS4 artwork
- `COPYING.GPL-3.0` — installer license

The recipe expects:

- OpenOrbis PS4 Toolchain v0.5.4;
- `PkgTool.Core`;
- `create-fself`;
- LLVM/Clang 10-compatible command names;
- the built Dropbear daemon as `daemon.bin`;
- OpenOrbis `libc.prx` and `libSceFios2.prx`.

The runtime-module source and license are under `../../third_party/openorbis-v0.5.4-modules/`. The daemon source is under `../../dropbear/`.

The historical package is validated and published, but the old end-to-end build was not frozen sufficiently to claim deterministic package reproduction. Do not present a newly generated PKG as the reboot-tested artifact unless its full transformation chain and target behavior are tested again.
