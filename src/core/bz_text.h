/* bz_text.h - small text helpers: base64, hex, trimming, ASCII case. */
#ifndef BZ_TEXT_H
#define BZ_TEXT_H

#include <stddef.h>

#include "bz_buf.h"

void bz_b64_encode(bz_buf *out, const unsigned char *p, size_t n);
/* standard alphabet with padding; returns 0 on any invalid character or length */
int bz_b64_decode(bz_buf *out, const char *s, size_t n);
void bz_hex(char *out, const unsigned char *p, size_t n);   /* out: 2n+1 bytes */

/* trim ASCII whitespace in place over [*s, *s + *n) */
void bz_trim(const char **s, size_t *n);
int bz_ieq(const char *a, const char *b);                    /* ASCII case-insensitive */
int bz_ieqn(const char *a, size_t an, const char *b);
/* copy at most cap-1 bytes and terminate; returns 0 if truncated */
int bz_strlcpy(char *dst, const char *src, size_t cap);
int bz_strlcpyn(char *dst, size_t cap, const char *src, size_t n);

#endif
