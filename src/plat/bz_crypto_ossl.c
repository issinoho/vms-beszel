/* bz_crypto_ossl.c - bz_crypto.h on OpenSSL 3 (SSL3 on OpenVMS, NOTES.md 20). */
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "bz_crypto.h"

static void digest(const EVP_MD *md, const void *p, size_t n, unsigned char *out)
{
    unsigned int len = 0;
    EVP_Digest(p, n, out, &len, md, NULL);
}

void bz_sha1(const void *p, size_t n, unsigned char out[20])
{
    digest(EVP_sha1(), p, n, out);
}

void bz_sha256(const void *p, size_t n, unsigned char out[32])
{
    digest(EVP_sha256(), p, n, out);
}

int bz_ed25519_verify(const unsigned char pub[32], const void *msg, size_t n,
                      const unsigned char sig[64])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
    EVP_MD_CTX *c;
    int ok = 0;
    if (!k)
        return 0;
    c = EVP_MD_CTX_new();
    if (c && EVP_DigestVerifyInit(c, NULL, NULL, NULL, k) == 1)
        ok = EVP_DigestVerify(c, sig, 64, msg, n) == 1;
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
    return ok;
}

int bz_random(void *p, size_t n)
{
    return RAND_bytes(p, (int)n) == 1;
}
