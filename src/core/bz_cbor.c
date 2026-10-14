/* bz_cbor.c - the CBOR subset spoken by the beszel hub (see bz_cbor.h). */
#include <string.h>

#include "bz_cbor.h"

/* ---- encoder ---- */

static void head(bz_buf *b, int major, uint64_t v)
{
    unsigned char h[9];
    size_t n;
    int i;
    unsigned mt = (unsigned)major << 5;
    if (v < 24) {
        h[0] = (unsigned char)(mt | (unsigned)v);
        n = 1;
    } else if (v <= 0xFFu) {
        h[0] = (unsigned char)(mt | 24);
        h[1] = (unsigned char)v;
        n = 2;
    } else if (v <= 0xFFFFu) {
        h[0] = (unsigned char)(mt | 25);
        n = 3;
    } else if (v <= 0xFFFFFFFFu) {
        h[0] = (unsigned char)(mt | 26);
        n = 5;
    } else {
        h[0] = (unsigned char)(mt | 27);
        n = 9;
    }
    if (n > 2)
        for (i = (int)n - 1; i >= 1; i--) {
            h[i] = (unsigned char)(v & 0xFF);
            v >>= 8;
        }
    bz_buf_put(b, h, n);
}

void bz_cb_uint(bz_buf *b, uint64_t v)
{
    head(b, BZ_CB_UINT, v);
}

void bz_cb_int(bz_buf *b, int64_t v)
{
    if (v >= 0)
        head(b, BZ_CB_UINT, (uint64_t)v);
    else
        head(b, BZ_CB_NINT, (uint64_t)(-(v + 1)));
}

void bz_cb_f64(bz_buf *b, double v)
{
    unsigned char h[9];
    uint64_t u;
    int i;
    /* IEEE binary64 on every target (VSI C /FLOAT=IEEE_FLOAT); byte order is
       taken from the integer view, so this works on either endianness. */
    memcpy(&u, &v, 8);
    h[0] = 0xFB;
    for (i = 8; i >= 1; i--) {
        h[i] = (unsigned char)(u & 0xFF);
        u >>= 8;
    }
    bz_buf_put(b, h, 9);
}

void bz_cb_bool(bz_buf *b, int v)
{
    bz_buf_putc(b, v ? 0xF5 : 0xF4);
}

void bz_cb_null(bz_buf *b)
{
    bz_buf_putc(b, 0xF6);
}

void bz_cb_textn(bz_buf *b, const char *s, size_t n)
{
    head(b, BZ_CB_TEXT, n);
    bz_buf_put(b, s, n);
}

void bz_cb_text(bz_buf *b, const char *s)
{
    bz_cb_textn(b, s, strlen(s));
}

void bz_cb_bytes(bz_buf *b, const void *p, size_t n)
{
    head(b, BZ_CB_BYTES, n);
    bz_buf_put(b, p, n);
}

void bz_cb_array(bz_buf *b, size_t n)
{
    head(b, BZ_CB_ARRAY, n);
}

void bz_cb_map(bz_buf *b, size_t n)
{
    head(b, BZ_CB_MAP, n);
}

size_t bz_cb_map_begin(bz_buf *b)
{
    size_t start = b->len;
    bz_buf_putc(b, 0xA0);                   /* placeholder: empty map */
    return start;
}

void bz_cb_map_end(bz_buf *b, size_t start, size_t count)
{
    bz_buf h;
    if (b->oom)
        return;
    if (count < 24) {
        b->p[start] = (unsigned char)(0xA0 | count);
        return;
    }
    /* longer header: encode it, then open a gap after the placeholder */
    bz_buf_init(&h);
    head(&h, BZ_CB_MAP, count);
    if (h.oom || !bz_buf_reserve(b, h.len - 1)) {
        b->oom = 1;
        bz_buf_free(&h);
        return;
    }
    memmove(b->p + start + h.len, b->p + start + 1, b->len - start - 1);
    memcpy(b->p + start, h.p, h.len);
    b->len += h.len - 1;
    b->p[b->len] = 0;
    bz_buf_free(&h);
}

/* ---- decoder ---- */

void bz_cbr_init(bz_cbr *r, const void *p, size_t len)
{
    r->p = (const unsigned char *)p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

static int fail(bz_cbr *r)
{
    r->err = 1;
    return 0;
}

/* Read an item header.  *indef is set for additional info 31. */
static int rhead(bz_cbr *r, int *major, uint64_t *arg, int *indef)
{
    unsigned ib, ai;
    size_t w, i;
    uint64_t v = 0;
    if (r->err || r->pos >= r->len)
        return fail(r);
    ib = r->p[r->pos++];
    *major = (int)(ib >> 5);
    ai = ib & 31;
    *indef = 0;
    if (ai < 24) {
        *arg = ai;
        return 1;
    }
    if (ai == 31) {
        if (*major == BZ_CB_UINT || *major == BZ_CB_NINT || *major == BZ_CB_TAG)
            return fail(r);
        *indef = 1;
        *arg = 0;
        return 1;
    }
    if (ai > 27)
        return fail(r);
    w = (size_t)1 << (ai - 24);
    if (r->len - r->pos < w)
        return fail(r);
    for (i = 0; i < w; i++)
        v = (v << 8) | r->p[r->pos++];
    *arg = v;
    return 1;
}

int bz_cbr_peek(const bz_cbr *r)
{
    if (r->err || r->pos >= r->len)
        return -1;
    return r->p[r->pos] >> 5;
}

static int skip_depth(bz_cbr *r, int depth)
{
    int major, indef;
    uint64_t arg, i;
    if (depth > BZ_CBOR_MAX_DEPTH)
        return fail(r);
    if (!rhead(r, &major, &arg, &indef))
        return 0;
    switch (major) {
    case BZ_CB_UINT:
    case BZ_CB_NINT:
        return 1;
    case BZ_CB_BYTES:
    case BZ_CB_TEXT:
        if (indef) {
            /* chunks of the same major type until the break byte */
            for (;;) {
                if (r->pos >= r->len)
                    return fail(r);
                if (r->p[r->pos] == 0xFF) {
                    r->pos++;
                    return 1;
                }
                if ((r->p[r->pos] >> 5) != major || (r->p[r->pos] & 31) == 31)
                    return fail(r);
                if (!skip_depth(r, depth + 1))
                    return 0;
            }
        }
        if (arg > r->len - r->pos)
            return fail(r);
        r->pos += (size_t)arg;
        return 1;
    case BZ_CB_ARRAY:
    case BZ_CB_MAP:
        if (indef) {
            for (;;) {
                if (r->pos >= r->len)
                    return fail(r);
                if (r->p[r->pos] == 0xFF) {
                    r->pos++;
                    return 1;
                }
                if (!skip_depth(r, depth + 1))
                    return 0;
                if (major == BZ_CB_MAP && !skip_depth(r, depth + 1))
                    return 0;
            }
        }
        /* each element needs at least one byte: reject absurd counts early */
        if (arg > r->len - r->pos)
            return fail(r);
        for (i = 0; i < arg; i++) {
            if (!skip_depth(r, depth + 1))
                return 0;
            if (major == BZ_CB_MAP && !skip_depth(r, depth + 1))
                return 0;
        }
        return 1;
    case BZ_CB_TAG:
        return skip_depth(r, depth + 1);
    default:
        /* simple values and floats: rhead has already consumed any payload
           (1, 2, 4 or 8 bytes); a lone break byte here is malformed */
        return indef ? fail(r) : 1;
    }
}

int bz_cbr_skip(bz_cbr *r)
{
    return skip_depth(r, 0);
}

int bz_cbr_uint(bz_cbr *r, uint64_t *v)
{
    int major, indef;
    if (!rhead(r, &major, v, &indef))
        return 0;
    if (major != BZ_CB_UINT)
        return fail(r);
    return 1;
}

int bz_cbr_bool(bz_cbr *r, int *v)
{
    if (r->err || r->pos >= r->len)
        return fail(r);
    if (r->p[r->pos] == 0xF5)
        *v = 1;
    else if (r->p[r->pos] == 0xF4)
        *v = 0;
    else
        return fail(r);
    r->pos++;
    return 1;
}

int bz_cbr_is_null(bz_cbr *r)
{
    if (r->err || r->pos >= r->len || r->p[r->pos] != 0xF6)
        return 0;
    r->pos++;
    return 1;
}

static int rstring(bz_cbr *r, int want, const unsigned char **p, size_t *n)
{
    int major, indef;
    uint64_t arg;
    if (!rhead(r, &major, &arg, &indef))
        return 0;
    if (major != want || indef || arg > r->len - r->pos)
        return fail(r);
    *p = r->p + r->pos;
    *n = (size_t)arg;
    r->pos += (size_t)arg;
    return 1;
}

int bz_cbr_bytes(bz_cbr *r, const unsigned char **p, size_t *n)
{
    return rstring(r, BZ_CB_BYTES, p, n);
}

int bz_cbr_text(bz_cbr *r, const char **p, size_t *n)
{
    const unsigned char *u;
    if (!rstring(r, BZ_CB_TEXT, &u, n))
        return 0;
    *p = (const char *)u;
    return 1;
}

static int rcontainer(bz_cbr *r, int want, uint64_t *n)
{
    int major, indef;
    if (!rhead(r, &major, n, &indef))
        return 0;
    if (major != want || indef || *n > r->len - r->pos)
        return fail(r);
    return 1;
}

int bz_cbr_map(bz_cbr *r, uint64_t *n)
{
    return rcontainer(r, BZ_CB_MAP, n);
}

int bz_cbr_array(bz_cbr *r, uint64_t *n)
{
    return rcontainer(r, BZ_CB_ARRAY, n);
}
