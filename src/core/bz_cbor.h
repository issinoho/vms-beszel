/* bz_cbor.h - the CBOR subset spoken by the beszel 0.21.0 hub (RFC 8949).
 *
 * Encoder: definite-length items only, integers in their shortest form and
 * floats always as 8-byte doubles (0xFB), which is what upstream's fxamacker
 * encoder emits with default options (docs/PROTOCOL.md 5.1, 5.2, 9).
 *
 * Maps whose entry count depends on omit rules are written with
 * bz_cb_map_begin / bz_cb_map_end: the header is fixed up once the count is
 * known.
 *
 * Decoder: a bounds-checked cursor over one message.  Every read checks the
 * remaining length; any malformed input sets `err` and makes all later reads
 * fail, so callers can read a whole structure and check `err` once.
 * Indefinite-length items are accepted by bz_cbr_skip only (the hub never
 * sends them).  Nesting is limited to BZ_CBOR_MAX_DEPTH.
 */
#ifndef BZ_CBOR_H
#define BZ_CBOR_H

#include <stddef.h>
#include <stdint.h>

#include "bz_buf.h"

#define BZ_CBOR_MAX_DEPTH 32

/* major types */
#define BZ_CB_UINT 0
#define BZ_CB_NINT 1
#define BZ_CB_BYTES 2
#define BZ_CB_TEXT 3
#define BZ_CB_ARRAY 4
#define BZ_CB_MAP 5
#define BZ_CB_TAG 6
#define BZ_CB_SIMPLE 7

/* encoder */
void bz_cb_uint(bz_buf *b, uint64_t v);
void bz_cb_int(bz_buf *b, int64_t v);
void bz_cb_f64(bz_buf *b, double v);
void bz_cb_bool(bz_buf *b, int v);
void bz_cb_null(bz_buf *b);
void bz_cb_text(bz_buf *b, const char *s);
void bz_cb_textn(bz_buf *b, const char *s, size_t n);
void bz_cb_bytes(bz_buf *b, const void *p, size_t n);
void bz_cb_array(bz_buf *b, size_t n);
void bz_cb_map(bz_buf *b, size_t n);
size_t bz_cb_map_begin(bz_buf *b);
void bz_cb_map_end(bz_buf *b, size_t start, size_t count);

/* decoder */
typedef struct bz_cbr {
    const unsigned char *p;
    size_t len;
    size_t pos;
    int err;
} bz_cbr;

void bz_cbr_init(bz_cbr *r, const void *p, size_t len);
/* peek the major type of the next item, or -1 at end/error */
int bz_cbr_peek(const bz_cbr *r);
int bz_cbr_skip(bz_cbr *r);
int bz_cbr_uint(bz_cbr *r, uint64_t *v);
int bz_cbr_bool(bz_cbr *r, int *v);
int bz_cbr_is_null(bz_cbr *r);          /* consumes the null if present */
int bz_cbr_bytes(bz_cbr *r, const unsigned char **p, size_t *n);
int bz_cbr_text(bz_cbr *r, const char **p, size_t *n);
int bz_cbr_map(bz_cbr *r, uint64_t *n);
int bz_cbr_array(bz_cbr *r, uint64_t *n);

#endif
