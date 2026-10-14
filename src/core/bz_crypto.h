/* bz_crypto.h - the cryptography the core needs.  Implemented on OpenSSL
 * (SSL3 on OpenVMS) by src/plat/bz_crypto_ossl.c on every platform.
 */
#ifndef BZ_CRYPTO_H
#define BZ_CRYPTO_H

#include <stddef.h>

void bz_sha1(const void *p, size_t n, unsigned char out[20]);
void bz_sha256(const void *p, size_t n, unsigned char out[32]);
/* 1 if sig (64 bytes) is a valid Ed25519 signature of msg by pub (32 bytes) */
int bz_ed25519_verify(const unsigned char pub[32], const void *msg, size_t n,
                      const unsigned char sig[64]);
/* cryptographically random bytes; 0 on failure */
int bz_random(void *p, size_t n);

#endif
