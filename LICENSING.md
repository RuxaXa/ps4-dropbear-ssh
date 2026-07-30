# Licensing and corresponding source

This is a multi-license repository. No top-level license overrides component-specific terms.

## Dropbear SSH

`dropbear/` is based on `mkj/dropbear` commit `ee65bff1567576a223febcdd5ae552326a4da4b1`. Its license collection is retained at `dropbear/LICENSE`. Bundled LibTomCrypt and LibTomMath retain their own adjacent license files.

## PS4 installer and packaging

`packaging/installer-v107/` is derived from John Törnblom's `tiny-ps4-shell` installer at commit `5ca90b7d2825e3e2c7f9fb1e81cf8949b37af563` and is licensed under GPLv3-or-later. The full GPLv3 text is included as `packaging/installer-v107/COPYING.GPL-3.0`.

The directory contains the source and package recipe required for the installer portion of the historical v1.07 package. The public Dropbear source in this repository is the corresponding daemon source.

## OpenOrbis runtime modules

The package contains OpenOrbis `libc.prx` and `libSceFios2.prx` from PS4 Toolchain v0.5.4, tag commit `b458dfd9d2f5c40aa249b127c9b9b4488afdc686`. OpenOrbis is GPLv3. The preferred source for those two modules and the GPL text are included under `third_party/openorbis-v0.5.4-modules/`.

Full pinned toolchain source:

https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/tree/b458dfd9d2f5c40aa249b127c9b9b4488afdc686

## Logo

`assets/dropbear-ssh-ps4-logo.png` and `packaging/installer-v107/icon0.png` are the RuxaXa project artwork, © 2026 RuxaXa, used here with permission. They are not covered by the software licenses above. Reuse outside this project requires permission from the owner.

## Package artifact

The compiled package is distributed only as a GitHub Release asset. Its complete file/hash inventory is in `RELEASE-v1.07.md`. No Sony firmware or decrypted Sony component is included; the runtime PRX files are the pinned OpenOrbis builds described above.
