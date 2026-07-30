# v1.07 / Build 57 release

## Identity

- app version: `1.07`
- internal daemon build: `57`
- title ID: `BREW00010`
- content ID: `IV0000-BREW00010_00-DROPBEARSSH00000`
- Dropbear source/banner version: `2026.91`
- Dropbear upstream base: `ee65bff1567576a223febcdd5ae552326a4da4b1`

## Release asset

`IV0000-BREW00010_00-DROPBEARSSH00000.pkg`

- size: `6619136` bytes
- SHA-256: `a711007389a011d8d6d2b01e075e01767fe9dc47981ede26b04aa9ee749392f6`
- OpenOrbis `PkgTool.Core pkg_validate --verbose`: PASS
- independent fresh extraction: PASS

## Extracted content

```text
daemon/eboot.bin          473504  7e3e8406daecc81e9e391e4dd44c9ed3a198b75f29e011843b04f20ad151e5ce
daemon/param                  500  ea98c724cecf969d3d2819a2e76d84f3b592c2585346e65bef813fd0e0e45a1a
eboot.bin                  103264  3539555a222e1b586675d9f0321667db2d552d5fccaa7e2a0e62cca3f38ffcbf
sce_module/libSceFios2.prx  52464  6de0afdaadb12f86e21b8d77ee969399cde0741319a1a770777d8d61bb858de2
sce_module/libc.prx          52448  11840c557f6ac9de8a8fbfed597493c26ff17f0e30725d9f7444da9e856255c3
sce_sys/keystone                96  d09eb8ab5f3b700164fc8ad85c54c2acbad71244e942aeedf7e3e7eb56408aac
```

The two PRX hashes exactly match OpenOrbis PS4 Toolchain v0.5.4 `src/modules/` artifacts. Their source and license are identified in `LICENSING.md`.

## Live evidence boundary

The package and daemon were installed and smoke-tested across two reboots on the owner's PS4 9.00 research console. The historical source-to-ELF-to-FSELF-to-PKG-to-installed-binary transformation chain was not completely frozen at every representation. The release binds the preserved package, extracted package payload and public source honestly; it does not claim a bit-reproducible rebuild of the historical PKG.

## Mandatory warning

This release includes the historical `root` / `root` compatibility path and a broad listener on TCP 2222. It is LAN-isolated research software only. Read `SECURITY.md` before installation.
