#ifndef DROPBEAR_LOCALOPTIONS_H_
#define DROPBEAR_LOCALOPTIONS_H_

/* OpenOrbis' bundled LibTomCrypt build does not expose the NIST curve table
 * expected by Dropbear. Keep modern independent Curve25519/Ed25519 support,
 * but omit the table-backed NIST ECDH/ECDSA families. */
#define DROPBEAR_ECDSA 0
#define DROPBEAR_ECDH 0


/* PS4 app sandboxes deny fork(2). This diagnostic build handles one SSH
 * session in-process so the banner, key exchange and authentication path can
 * be validated before introducing a serial pthread worker. */
#define DEBUG_NOFORK 1

/* Delayed hostkey generation must target the writable data partition. */
#define RSA_PRIV_FILENAME "/data/dropbear/dropbear_rsa_host_key"
#define ED25519_PRIV_FILENAME "/data/dropbear/dropbear_ed25519_host_key"

#endif
