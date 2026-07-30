# Security policy

## Build 57 is intentionally unsafe historical research software

The v1.07 / Build 57 source and package preserve the behavior reboot-tested on an owner-controlled LAN:

- password authentication is enabled;
- the PS4 synthetic account accepts `root` / `root`;
- the listener binds to `0.0.0.0:2222`;
- sessions are serial because `DEBUG_NOFORK` is enabled;
- the historical configuration uses `--disable-harden`;
- SCP downloads are capped at 8192 bytes.

Required boundaries:

1. Never expose TCP 2222 to the public internet.
2. Restrict reachability with a trusted VLAN/firewall policy.
3. Prefer public-key authentication and never copy a private key to the console.
4. Replace generated host keys after cloning an installation.
5. Do not use this daemon as a general-purpose production SSH server.
6. Treat any credential-removal/key-only change as a new build that needs clean-build, package and reboot testing.

## Package behavior

The first launch is an installer. It obtains elevated privileges in an already jailbroken environment, remounts the system area and writes the daemon application under `/system/vsh/app/BREW00010`. The next fresh launch starts that installed daemon. Review `packaging/installer-v107/main_install.c` before installation.

## Reporting

Do not submit Sony firmware, console dumps, device identities, credentials, private keys, certificates or private-network details in issues. Use hashes and redistributable minimal reproductions.
