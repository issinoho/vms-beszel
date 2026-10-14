/* bz_net_ossl.c - TCP and TLS client on BSD sockets and OpenSSL.
 *
 * The same code runs on Linux and on OpenVMS (TCP/IP Services sockets, SSL3):
 * the Phase 0 probe showed non-blocking connect + select() and a TLS 1.3
 * handshake working unprivileged on both nodes (NOTES.md 19, 20).
 *
 * TLS always verifies the hub's certificate and host name (DECISIONS.md D5):
 * against the platform's CA bundle (or the system store) plus CA_CERT_FILE,
 * which upstream also adds to the system roots (PROTOCOL.md 7).
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "bz_platform.h"

struct bz_sock {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
};

static void ossl_err(char *err, size_t errlen, const char *what)
{
    unsigned long e = ERR_get_error();
    char buf[200];
    if (e) {
        ERR_error_string_n(e, buf, sizeof buf);
        snprintf(err, errlen, "%s: %s", what, buf);
    } else {
        snprintf(err, errlen, "%s", what);
    }
    ERR_clear_error();
}

static int connect_timeout(const struct addrinfo *ai, unsigned timeout_ms, char *err, size_t errlen)
{
    int fd, fl, rc, so = 0;
    socklen_t sl = sizeof so;
    fd_set w;
    struct timeval tv;
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        snprintf(err, errlen, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (rc != 0) {
        FD_ZERO(&w);
        FD_SET(fd, &w);
        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
        rc = select(fd + 1, NULL, &w, NULL, &tv);
        if (rc <= 0) {
            snprintf(err, errlen, rc == 0 ? "connect: timed out" : "select: %s", strerror(errno));
            close(fd);
            return -1;
        }
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so, &sl);
        if (so) {
            snprintf(err, errlen, "connect: %s", strerror(so));
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl);                 /* blocking from here on */
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof tv);
    return fd;
}

static int is_ip_literal(const char *host)
{
    unsigned char a[16];
    return inet_pton(AF_INET, host, a) == 1 || inet_pton(AF_INET6, host, a) == 1;
}

static int tls_start(bz_sock *s, const char *host, const char *ca_file, unsigned timeout_ms,
                     char *err, size_t errlen)
{
    const char *bundle = bz_plat_default_ca_file();
    struct timeval tv;
    s->ctx = SSL_CTX_new(TLS_client_method());
    if (!s->ctx) {
        ossl_err(err, errlen, "TLS context");
        return 0;
    }
    SSL_CTX_set_min_proto_version(s->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(s->ctx, SSL_VERIFY_PEER, NULL);
    if (bundle) {
        if (SSL_CTX_load_verify_locations(s->ctx, bundle, NULL) != 1) {
            ossl_err(err, errlen, "loading the CA bundle");
            return 0;
        }
    } else if (SSL_CTX_set_default_verify_paths(s->ctx) != 1) {
        ossl_err(err, errlen, "loading the system CA store");
        return 0;
    }
    if (ca_file && *ca_file && SSL_CTX_load_verify_locations(s->ctx, ca_file, NULL) != 1) {
        ossl_err(err, errlen, "loading CA_CERT_FILE");
        return 0;
    }
    s->ssl = SSL_new(s->ctx);
    if (!s->ssl) {
        ossl_err(err, errlen, "TLS session");
        return 0;
    }
    if (is_ip_literal(host)) {
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(s->ssl), host) != 1) {
            ossl_err(err, errlen, "TLS IP check");
            return 0;
        }
    } else {
        SSL_set_tlsext_host_name(s->ssl, host);
        if (SSL_set1_host(s->ssl, host) != 1) {
            ossl_err(err, errlen, "TLS host check");
            return 0;
        }
    }
    SSL_set_fd(s->ssl, s->fd);
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof tv);
    if (SSL_connect(s->ssl) != 1) {
        long v = SSL_get_verify_result(s->ssl);
        if (v == X509_V_ERR_HOSTNAME_MISMATCH || v == X509_V_ERR_IP_ADDRESS_MISMATCH)
            snprintf(err, errlen, "TLS: the hub's certificate is not for %s (%s)", host,
                     X509_verify_cert_error_string(v));
        else if (v == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY ||
                 v == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT ||
                 v == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
                 v == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN)
            snprintf(err, errlen, "TLS: certificate not trusted: %s (set CA_CERT_FILE to the CA "
                     "that issued the hub's certificate)", X509_verify_cert_error_string(v));
        else if (v != X509_V_OK)
            snprintf(err, errlen, "TLS: certificate not trusted: %s", X509_verify_cert_error_string(v));
        else
            ossl_err(err, errlen, "TLS handshake");
        return 0;
    }
    /* reads only happen after bz_net_wait says there is data */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof tv);
    return 1;
}

bz_sock *bz_net_open(const char *host, const char *port, int tls, const char *ca_file,
                     unsigned timeout_ms, char *err, size_t errlen)
{
    struct addrinfo hints, *res, *ai;
    bz_sock *s;
    int rc;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "lookup of %s failed: %s", host, gai_strerror(rc));
        return NULL;
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        freeaddrinfo(res);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->fd = -1;
    for (ai = res; ai && s->fd < 0; ai = ai->ai_next)
        s->fd = connect_timeout(ai, timeout_ms, err, errlen);
    freeaddrinfo(res);
    if (s->fd < 0) {
        free(s);
        return NULL;
    }
    if (tls && !tls_start(s, host, ca_file, timeout_ms, err, errlen)) {
        bz_net_close(s);
        return NULL;
    }
    return s;
}

int bz_net_send(bz_sock *s, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        long w;
        if (s->ssl) {
            int r = SSL_write(s->ssl, c, n > 65536 ? 65536 : (int)n);
            if (r <= 0)
                return 0;
            w = r;
        } else {
            w = (long)send(s->fd, c, n, 0);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                return 0;
        }
        c += w;
        n -= (size_t)w;
    }
    return 1;
}

long bz_net_recv(bz_sock *s, void *p, size_t cap)
{
    if (s->ssl) {
        int r = SSL_read(s->ssl, p, cap > 65536 ? 65536 : (int)cap);
        if (r > 0)
            return r;
        switch (SSL_get_error(s->ssl, r)) {
        case SSL_ERROR_ZERO_RETURN:
            return 0;
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return -2;                      /* a partial record: wait again */
        default:
            ERR_clear_error();
            return -1;
        }
    } else {
        long r;
        do
            r = (long)recv(s->fd, p, cap, 0);
        while (r < 0 && errno == EINTR);
        return r;
    }
}

int bz_net_wait(bz_sock *s, unsigned timeout_ms)
{
    fd_set rd;
    struct timeval tv;
    int rc;
    if (s->ssl && SSL_pending(s->ssl) > 0)
        return 1;
    FD_ZERO(&rd);
    FD_SET(s->fd, &rd);
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
    rc = select(s->fd + 1, &rd, NULL, NULL, &tv);
    if (rc < 0)
        return errno == EINTR ? 0 : -1;
    return rc > 0;
}

void bz_net_close(bz_sock *s)
{
    if (!s)
        return;
    if (s->ssl) {
        SSL_shutdown(s->ssl);
        SSL_free(s->ssl);
    }
    if (s->ctx)
        SSL_CTX_free(s->ctx);
    if (s->fd >= 0)
        close(s->fd);
    free(s);
}
