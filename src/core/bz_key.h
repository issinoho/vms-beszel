/* bz_key.h - the hub's public key(s), from KEY or KEY_FILE (PROTOCOL.md 2.3).
 *
 * The input is authorized_keys text: one key per line, blank lines and '#'
 * comments skipped, optional leading options, then "ssh-ed25519 <base64>
 * [comment]".  As upstream, any line that does not parse is fatal.  The hub
 * only ever has an Ed25519 key, so other key types are rejected by name.
 */
#ifndef BZ_KEY_H
#define BZ_KEY_H

#include <stddef.h>

#define BZ_KEY_MAX 8

typedef struct bz_keys {
    int n;
    unsigned char ed25519[BZ_KEY_MAX][32];
} bz_keys;

int bz_keys_parse(bz_keys *k, const char *text, size_t len, char *err, size_t errlen);

#endif
