/* probe_c99.c - C99 features the agent core relies on, on each VSI C. */
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

struct pt { int x, y; };

static int fmt(char *b, size_t n, const char *f, ...)
{
    va_list ap; int r;
    va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap);
    return r;
}

int main(void)
{
    uint64_t big = UINT64_C(18446744073709551615);
    uint32_t w = 0xFFFFFFFFu;
    int8_t s = -38;
    struct pt p = { .y = 2, .x = 1 };            /* designated initializers */
    double d = 16.449999, e;
    char buf[64];
    union { double d; uint64_t u; } cv;
    for (int i = 0; i < 1; i++) { }              /* declaration in for */
    e = round(d * 100) / 100;
    cv.d = e;
    fmt(buf, sizeof buf, "%" PRIu64 " %u %d", big, (unsigned)w, (int)s);
    printf("ints: %s\n", buf);
    printf("sizeof: ptr=%d long=%d llong=%d size_t=%d\n", (int)sizeof(void *), (int)sizeof(long),
           (int)sizeof(long long), (int)sizeof(size_t));
    printf("desig: %d %d\n", p.x, p.y);
    printf("round: %.17g bits=%016" PRIx64 " (expect 16.45 = 40307333 33333333)\n", e, cv.u);
    printf("snprintf trunc: %d\n", snprintf(buf, 4, "abcdef"));
    {   unsigned char b8[8]; uint64_t v = 0x0102030405060708u; memcpy(b8, &v, 8);
        printf("endian: %s\n", b8[0] == 8 ? "little" : "big"); }
    return 0;
}
