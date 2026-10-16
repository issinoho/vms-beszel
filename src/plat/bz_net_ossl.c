/* bz_net_ossl.c - TCP and TLS client on BSD sockets and OpenSSL.
 *
 * The same code runs on Linux and on OpenVMS (TCP/IP Services sockets, SSL3):
 * the Phase 0 probe showed non-blocking connect + select() and a TLS 1.3
 * handshake working unprivileged on both nodes (NOTES.md 19, 20).
 *
 * The socket is non-blocking for its whole life, and every operation has a
 * deadline (security review M1): a peer that stops mid-record, or trickles a
 * handshake, can't hang the agent, whose own timers, stop file and signals
 * keep running.
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
#include "bz_text.h"

struct bz_sock {
    int fd;
    SSL *ssl;
};

/* One TLS context for the life of the process: loading the CA bundle took
   about 7 s of CPU on the x86 node (NOTES.md 34), and it would otherwise be
   repeated on every reconnect.  Verification still happens per connection. */
static SSL_CTX *tls_ctx;
static char tls_ctx_ca[1024];

#define SEND_DEADLINE_MS 30000

/* wait until fd is readable (or writable) or the deadline passes: 1 ready, 0 not */
static int wait_io(int fd, int want_write, uint64_t deadline)
{
    for (;;) {
        fd_set fs;
        struct timeval tv;
        uint64_t now = bz_plat_now_ms();
        int rc;
        if (now >= deadline)
            return 0;
        FD_ZERO(&fs);
        FD_SET(fd, &fs);
        tv.tv_sec = (long)((deadline - now) / 1000);
        tv.tv_usec = (long)((deadline - now) % 1000) * 1000;
        rc = select(fd + 1, want_write ? NULL : &fs, want_write ? &fs : NULL, NULL, &tv);
        if (rc > 0)
            return 1;
        if (rc < 0 && errno != EINTR)
            return 0;
    }
}

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

/* "IPv4 192.0.2.10" / "IPv6 2001:db8::1": errors name the address that failed
   (NOTES.md 40: a socket() error at 03:04 didn't say which one) */
static void addr_name(const struct addrinfo *ai, char *out, size_t cap)
{
    char host[64];
    if (getnameinfo(ai->ai_addr, ai->ai_addrlen, host, sizeof host, NULL, 0, NI_NUMERICHOST) != 0)
        snprintf(host, sizeof host, "?");
    snprintf(out, cap, "%s %s", ai->ai_family == AF_INET6 ? "IPv6" : ai->ai_family == AF_INET ? "IPv4" : "address", host);
}

static int connect_timeout(const struct addrinfo *ai, unsigned timeout_ms, char *err, size_t errlen)
{
    char an[80];
    int fd, fl, rc, so = 0;
    socklen_t sl = sizeof so;
    fd_set w;
    struct timeval tv;
    addr_name(ai, an, sizeof an);
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        snprintf(err, errlen, "socket for %s: %s", an, strerror(errno));
        return -1;
    }
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        snprintf(err, errlen, "connect to %s: %s", an, strerror(errno));
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
            if (rc == 0)
                snprintf(err, errlen, "connect to %s: timed out", an);
            else
                snprintf(err, errlen, "select: %s", strerror(errno));
            close(fd);
            return -1;
        }
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so, &sl);
        if (so) {
            snprintf(err, errlen, "connect to %s: %s", an, strerror(so));
            close(fd);
            return -1;
        }
    }
    return fd;                              /* still non-blocking: see the top */
}

static int is_ip_literal(const char *host)
{
    unsigned char a[16];
    return inet_pton(AF_INET, host, a) == 1 || inet_pton(AF_INET6, host, a) == 1;
}

static SSL_CTX *tls_context(const char *ca_file, char *err, size_t errlen)
{
    const char *bundle = bz_plat_default_ca_file();
    SSL_CTX *ctx;
    if (!ca_file)
        ca_file = "";
    if (tls_ctx && strcmp(tls_ctx_ca, ca_file) == 0)
        return tls_ctx;
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ossl_err(err, errlen, "TLS context");
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (bundle) {
        if (SSL_CTX_load_verify_locations(ctx, bundle, NULL) != 1) {
            ossl_err(err, errlen, "loading the CA bundle");
            SSL_CTX_free(ctx);
            return NULL;
        }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        ossl_err(err, errlen, "loading the system CA store");
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (*ca_file && SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
        ossl_err(err, errlen, "loading CA_CERT_FILE");
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (tls_ctx)
        SSL_CTX_free(tls_ctx);
    tls_ctx = ctx;
    bz_strlcpy(tls_ctx_ca, ca_file, sizeof tls_ctx_ca);
    return ctx;
}

static int tls_start(bz_sock *s, const char *host, const char *ca_file, unsigned timeout_ms,
                     char *err, size_t errlen)
{
    uint64_t deadline = bz_plat_now_ms() + timeout_ms;
    int r;
    SSL_CTX *ctx = tls_context(ca_file, err, errlen);
    if (!ctx)
        return 0;
    s->ssl = SSL_new(ctx);
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
    /* the whole handshake within timeout_ms, however slowly the peer sends */
    while ((r = SSL_connect(s->ssl)) != 1) {
        int e = SSL_get_error(s->ssl, r);
        if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) &&
            wait_io(s->fd, e == SSL_ERROR_WANT_WRITE, deadline))
            continue;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            snprintf(err, errlen, "TLS handshake: timed out");
            return 0;
        }
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
    /* VMS TCP/IP answers each address twice for SOCK_STREAM, as TCP and as SCTP,
       and socket() refuses the SCTP ones with EPROTOTYPE: that error, from the
       last entry tried, hid the real one when the hub was down (NOTES.md 45) */
    hints.ai_protocol = IPPROTO_TCP;
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
    /* all of it within SEND_DEADLINE_MS, or fail (and the caller closes) */
    const char *c = p;
    uint64_t deadline = bz_plat_now_ms() + SEND_DEADLINE_MS;
    while (n) {
        long w;
        int want_write = 1;
        if (s->ssl) {
            int r = SSL_write(s->ssl, c, n > 65536 ? 65536 : (int)n);
            if (r > 0) {
                w = r;
            } else {
                int e = SSL_get_error(s->ssl, r);
                if (e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_WANT_READ) {
                    ERR_clear_error();
                    return 0;
                }
                want_write = e == SSL_ERROR_WANT_WRITE;
                w = 0;
            }
        } else {
            w = (long)send(s->fd, c, n, 0);
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                w = 0;
            else if (w <= 0)
                return 0;
        }
        if (w == 0) {
            if (!wait_io(s->fd, want_write, deadline))
                return 0;
            continue;
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
            return -2;                      /* a partial record: never blocks (M1) */
        default:
            ERR_clear_error();
            return -1;
        }
    } else {
        long r;
        do
            r = (long)recv(s->fd, p, cap, 0);
        while (r < 0 && errno == EINTR);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return -2;
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
    if (s->fd >= 0)
        close(s->fd);
    free(s);
}
