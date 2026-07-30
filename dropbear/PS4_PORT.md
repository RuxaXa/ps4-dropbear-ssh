# Dropbear PS4 port — v1.07 / Build 57

## Identity

- Upstream: https://github.com/mkj/dropbear
- Upstream commit: `ee65bff1567576a223febcdd5ae552326a4da4b1`
- Upstream source/banner version: `2026.91`
- PS4 application release label: `v1.07`
- Internal tested build: `57`
- Tested binary SHA-256: `7e3e8406daecc81e9e391e4dd44c9ed3a198b75f29e011843b04f20ad151e5ce`
- Tested environment: owner-controlled PS4 on system software 9.00

The binary is not distributed here. The hash binds this source/provenance record to the tested artifact held privately by its owner.

## Why a PS4-specific port is needed

A normal Dropbear server assumes process creation, account databases, resolver behavior, signals, nonblocking descriptor operations, PTYs and standard shell execution. The regular PS4 homebrew app sandbox does not provide all of those semantics reliably.

The port therefore includes:

- app entry handling for missing/unreliable `argv`;
- fixed foreground listener configuration on TCP 2222;
- resolver bypass for the fixed IPv4 listener;
- writable host-key paths under `/data/dropbear`;
- synthetic PS4 account handling;
- serial no-fork SSH sessions;
- an in-process bounded command dispatcher;
- virtual PTY state and CRLF handling;
- an fd-based legacy SCP implementation with bounded transfers;
- runtime markers for constrained launch diagnostics.

## Evidence by feature

### Live-confirmed on the owner console

- listener startup and SSH protocol handshake;
- authentication and repeated serial sessions;
- reboot survival of the installed Build 57 package/daemon arrangement;
- fork-free research shell behavior;
- legacy SCP in the bounded configuration represented by Build 57;
- return to a usable listener after tested sessions.

### Host-tested

- path normalization and per-session CWD behavior;
- bounded shell parser/file commands;
- SCP request parsing and transfer edge cases;
- selected runtime wrapper behavior.

### Not implied

- arbitrary Unix command execution;
- unrestricted process spawning;
- full OpenSSH PTY semantics;
- SFTP support;
- safe internet exposure;
- unbounded bulk transfer.

## Transfer boundary

Build 57 is intentionally conservative. The proven PS4 fallback caps the custom SCP download path at 8 KiB. Use a separately validated transfer service for larger files until event-driven backpressure is demonstrated on the target.

## Security warning

The historical tested source contains a PS4-only default compatibility credential path. It is retained for source fidelity, not recommended deployment practice. Keep the service LAN-isolated and migrate to key-only authentication in a separately tested hardening release.

## Build-template correction

The generated `Makefile` used for Build 57 linked `ps4-runtime.o`, `ps4-shell.o` and `ps4-scp.o`. The preserved `Makefile.in` snapshot listed only `ps4-runtime.o`. This public tree adds the two missing object names to `Makefile.in` so regenerating the Makefile preserves the object graph that was actually tested. No runtime C behavior was changed by this correction.

## Tiny-Shell v1.08 boundary

A later 46-command Tiny-Shell integration was prepared only as documentation. It was not built, deployed or merged into this Build 57 source. Tiny-Shell is GPLv3-or-later and would require a separate licensing and process-isolation design.
