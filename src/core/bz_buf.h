/* bz_buf.h - growable byte buffer.
 *
 * Every write either succeeds or sets `oom`; callers check `oom` once at the
 * end of a sequence of writes instead of after each one.  The buffer always
 * keeps a NUL after `len` so text content can be used as a C string.
 */
#ifndef BZ_BUF_H
#define BZ_BUF_H

#include <stddef.h>

typedef struct bz_buf {
    unsigned char *p;
    size_t len;
    size_t cap;
    int oom;
} bz_buf;

void bz_buf_init(bz_buf *b);
void bz_buf_free(bz_buf *b);
void bz_buf_clear(bz_buf *b);
int bz_buf_reserve(bz_buf *b, size_t extra);
void bz_buf_put(bz_buf *b, const void *data, size_t n);
void bz_buf_putc(bz_buf *b, int c);
void bz_buf_puts(bz_buf *b, const char *s);
void bz_buf_printf(bz_buf *b, const char *fmt, ...);
void bz_buf_consume(bz_buf *b, size_t n);   /* drop n bytes from the front */
void bz_buf_set(bz_buf *b, const void *data, size_t n);

#endif
