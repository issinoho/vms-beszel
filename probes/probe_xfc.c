/* probe_xfc.c - Phase 2: can $GETRMI report the XFC size?
 * RMI$_CACHE_MEMORY alone returned 0 (NOTES.md 6).  Try RMI$_CACHE_EVERYTHING
 * (dumped), and CACHE_MEMORY/CACHE_FREE in the same list as CACHE_STATE.
 */
#include <stdio.h>
#include <string.h>
#include <starlet.h>
#include <rmidef.h>
#include <efndef.h>

struct item { unsigned short len, code; void *buf; unsigned short *retlen; };
struct iosb { unsigned short st, cnt; unsigned int dev; };
static unsigned char big[16000];

static int call(struct item *il)
{
    struct iosb iosb;
    int st = sys$getrmi(EFN$C_ENF, 0, 0, il, &iosb, 0, 0);
    if (st & 1) { sys$synch(EFN$C_ENF, &iosb); st = iosb.st; }
    return st;
}

int main(void)
{
    struct item il[4];
    unsigned short rl = 0, r1 = 0, r2 = 0, r3 = 0;
    unsigned int state = 0, mem = 0, fre = 0;
    int st, i, len;
    for (len = 4; len <= 16000; len = len < 1024 ? len * 4 : 16000) {
        memset(il, 0, sizeof il);
        memset(big, 0, sizeof big);
        il[0].len = (unsigned short)len; il[0].code = RMI$_CACHE_EVERYTHING; il[0].buf = big; il[0].retlen = &rl;
        st = call(il);
        printf("CACHE_EVERYTHING len=%d st=%d rl=%d\n", len, st, rl);
        if (st & 1) {
            for (i = 0; i < 256 && i < len; i++) {
                if (i % 32 == 0) printf("\n  %04x:", i);
                printf(" %02x", big[i]);
            }
            printf("\n");
            break;
        }
        if (len == 16000) break;
    }
    memset(il, 0, sizeof il);
    il[0].len = 4; il[0].code = RMI$_CACHE_STATE; il[0].buf = &state; il[0].retlen = &r1;
    il[1].len = 4; il[1].code = RMI$_CACHE_MEMORY; il[1].buf = &mem; il[1].retlen = &r2;
    il[2].len = 4; il[2].code = RMI$_CACHE_FREE; il[2].buf = &fre; il[2].retlen = &r3;
    st = call(il);
    printf("STATE+MEMORY+FREE st=%d state=%u memory=%u free=%u\n", st, state, mem, fre);
    return 1;
}
