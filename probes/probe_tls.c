/* probe_tls.c - Phase 0 probe: SSL3 crypto, TLS and sockets for the agent.
 *
 *   probe_tls <hub-host> <hub-port> [<tls-host>]
 *
 * 1. Ed25519 verify: RFC 8032 test vectors 1 and 2, plus a corrupted copy.
 * 2. SHA-256("abc") against FIPS 180-2.
 * 3. TCP to the hub: non-blocking connect + select(), a WebSocket upgrade with
 *    a token that cannot exist (expect 401), and the status line read back.
 * 4. TLS client handshake to <tls-host>:443 with SNI (protocol, cipher and the
 *    verify result with no CA store loaded: a failure there is expected).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static int unhex(const char *h, unsigned char *out)
{
    int n = 0;
    while (h[0] && h[1]) { unsigned int b; sscanf(h, "%2x", &b); out[n++] = (unsigned char)b; h += 2; }
    return n;
}

static int ed_verify(const char *pkh, const char *msgh, const char *sigh, int corrupt)
{
    unsigned char pk[32], msg[64], sig[64];
    int ml = unhex(msgh, msg), rc;
    EVP_PKEY *k;
    EVP_MD_CTX *c;
    unhex(pkh, pk); unhex(sigh, sig);
    if (corrupt) sig[10] ^= 1;
    k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pk, 32);
    if (!k) { printf("  new_raw_public_key failed\n"); ERR_print_errors_fp(stdout); return -1; }
    c = EVP_MD_CTX_new();
    rc = EVP_DigestVerifyInit(c, NULL, NULL, NULL, k);
    if (rc == 1) rc = EVP_DigestVerify(c, sig, 64, msg, (size_t)ml);
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
    return rc;
}

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *ai;
    int s, rc, err = 0, fl;
    fd_set w;
    struct timeval tv;
    socklen_t el = sizeof err;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if ((rc = getaddrinfo(host, port, &hints, &ai)) != 0) { printf("  getaddrinfo(%s): %d\n", host, rc); return -1; }
    s = socket(AF_INET, SOCK_STREAM, 0);
    fl = fcntl(s, F_GETFL, 0);
    printf("  fcntl O_NONBLOCK: %d\n", fcntl(s, F_SETFL, fl | O_NONBLOCK));
    rc = connect(s, ai->ai_addr, ai->ai_addrlen);
    printf("  connect: rc=%d errno=%d (EINPROGRESS=%d)\n", rc, rc ? errno : 0, EINPROGRESS);
    freeaddrinfo(ai);
    FD_ZERO(&w); FD_SET(s, &w);
    tv.tv_sec = 5; tv.tv_usec = 0;
    rc = select(s + 1, NULL, &w, NULL, &tv);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
    printf("  select writable: rc=%d so_error=%d\n", rc, err);
    fcntl(s, F_SETFL, fl);                  /* blocking again for the simple reads below */
    return (rc == 1 && err == 0) ? s : -1;
}

int main(int argc, char **argv)
{
    unsigned char md[32];
    unsigned int mdl = 0;
    char buf[1024];
    int s, n;
    if (argc < 3) { printf("usage: probe_tls <hub-host> <hub-port> [<tls-host>]\n"); return 1; }
    printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION));

    printf("1. Ed25519 (1 = verified)\n");
    printf("  RFC 8032 test 1: %d\n", ed_verify(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", 0));
    printf("  RFC 8032 test 2: %d\n", ed_verify(
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", 0));
    printf("  test 2, corrupted (expect 0): %d\n", ed_verify(
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", 1));

    EVP_Digest("abc", 3, md, &mdl, EVP_sha256(), NULL);
    printf("2. SHA-256(abc) %s\n", memcmp(md, "\xba\x78\x16\xbf\x8f\x01\xcf\xea", 8) == 0 ? "OK" : "WRONG");

    printf("3. TCP to the hub\n");
    if ((s = tcp_connect(argv[1], argv[2])) >= 0) {
        n = sprintf(buf, "GET /api/beszel/agent-connect HTTP/1.1\r\nHost: %s:%s\r\nConnection: Upgrade\r\n"
                    "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "X-Token: probe-not-a-token\r\nX-Beszel: 0.21.0\r\n\r\n", argv[1], argv[2]);
        printf("  send: %d of %d\n", (int)send(s, buf, n, 0), n);
        n = (int)recv(s, buf, sizeof buf - 1, 0);
        if (n > 0) { buf[n] = 0; *strchr(buf, '\r') = 0; printf("  reply: %s\n", buf); }
        else printf("  recv: %d errno=%d\n", n, errno);
        close(s);
    }

    if (argc > 3) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        SSL *ssl;
        printf("4. TLS to %s:443\n", argv[3]);
        if ((s = tcp_connect(argv[3], "443")) >= 0) {
            ssl = SSL_new(ctx);
            SSL_set_fd(ssl, s);
            SSL_set_tlsext_host_name(ssl, argv[3]);
            n = SSL_connect(ssl);
            printf("  SSL_connect: %d  %s  %s  verify=%ld (20 = no CA store loaded, expected)\n", n,
                   SSL_get_version(ssl), SSL_get_cipher_name(ssl), SSL_get_verify_result(ssl));
            if (n != 1) ERR_print_errors_fp(stdout);
            SSL_free(ssl);
            close(s);
        }
        SSL_CTX_free(ctx);
    }
    return 1;
}
