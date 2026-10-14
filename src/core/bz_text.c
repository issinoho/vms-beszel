/* bz_text.c - small text helpers (see bz_text.h). */
#include <string.h>

#include "bz_text.h"

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void bz_b64_encode(bz_buf *out, const unsigned char *p, size_t n)
{
    size_t i;
    char q[4];
    for (i = 0; i + 2 < n; i += 3) {
        q[0] = b64[p[i] >> 2];
        q[1] = b64[((p[i] & 3) << 4) | (p[i + 1] >> 4)];
        q[2] = b64[((p[i + 1] & 15) << 2) | (p[i + 2] >> 6)];
        q[3] = b64[p[i + 2] & 63];
        bz_buf_put(out, q, 4);
    }
    if (n - i == 1) {
        q[0] = b64[p[i] >> 2];
        q[1] = b64[(p[i] & 3) << 4];
        q[2] = q[3] = '=';
        bz_buf_put(out, q, 4);
    } else if (n - i == 2) {
        q[0] = b64[p[i] >> 2];
        q[1] = b64[((p[i] & 3) << 4) | (p[i + 1] >> 4)];
        q[2] = b64[(p[i + 1] & 15) << 2];
        q[3] = '=';
        bz_buf_put(out, q, 4);
    }
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int bz_b64_decode(bz_buf *out, const char *s, size_t n)
{
    size_t i, pad = 0;
    int v[4], k;
    unsigned char o[3];
    if (n % 4)
        return 0;
    if (n && s[n - 1] == '=') pad++;
    if (n > 1 && s[n - 2] == '=') pad++;
    for (i = 0; i < n; i += 4) {
        int last = i + 4 == n;
        for (k = 0; k < 4; k++) {
            if (last && k >= 4 - (int)pad) {
                v[k] = 0;
                continue;
            }
            if ((v[k] = b64val((unsigned char)s[i + k])) < 0)
                return 0;
        }
        o[0] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
        o[1] = (unsigned char)(((v[1] & 15) << 4) | (v[2] >> 2));
        o[2] = (unsigned char)(((v[2] & 3) << 6) | v[3]);
        bz_buf_put(out, o, last ? 3 - pad : 3);
    }
    return !out->oom;
}

void bz_hex(char *out, const unsigned char *p, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = hx[p[i] >> 4];
        out[2 * i + 1] = hx[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int isws(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

void bz_trim(const char **s, size_t *n)
{
    while (*n && isws((unsigned char)**s)) {
        (*s)++;
        (*n)--;
    }
    while (*n && isws((unsigned char)(*s)[*n - 1]))
        (*n)--;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

int bz_ieqn(const char *a, size_t an, const char *b)
{
    size_t i;
    for (i = 0; i < an; i++)
        if (!b[i] || lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
    return b[an] == 0;
}

int bz_ieq(const char *a, const char *b)
{
    return bz_ieqn(a, strlen(a), b);
}

int bz_strlcpyn(char *dst, size_t cap, const char *src, size_t n)
{
    int ok = n < cap;
    if (!cap)
        return 0;
    if (!ok)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return ok;
}

int bz_strlcpy(char *dst, const char *src, size_t cap)
{
    return bz_strlcpyn(dst, cap, src, strlen(src));
}
