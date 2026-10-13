/* probe_rmi.c - Phase 0 follow-up: how $GETRMI wants its item buffers.
 *
 * probe_stats.c passed 64-byte buffers for scalar items (all BADPARAM) and a
 * 65536-byte length for MODES/DISKS, which truncates to 0 in the 16-bit field.
 * Here: each scalar item at length 4 and at 8; all scalars in one list at 4;
 * MODES and DISKS with a 32000-byte buffer, two samples 5 s apart.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <starlet.h>
#include <rmidef.h>
#include <efndef.h>

struct item { unsigned short len, code; void *buf; unsigned short *retlen; };
struct iosb { unsigned short st, cnt; unsigned int dev; };
static unsigned char big[32000];

static int call(struct item *il)
{
    struct iosb iosb;
    int st = sys$getrmi(EFN$C_ENF, 0, 0, il, &iosb, 0, 0);
    if (st & 1) { sys$synch(EFN$C_ENF, &iosb); st = iosb.st; }
    return st;
}

static void dump(const char *name, int code)
{
    unsigned short rl = 0;
    struct item il[2];
    int st, i;
    memset(il, 0, sizeof il);
    memset(big, 0xEE, sizeof big);
    il[0].len = sizeof big; il[0].code = (unsigned short)code; il[0].buf = big; il[0].retlen = &rl;
    st = call(il);
    printf("%s st=%d rl=%d", name, st, rl);
    /* show up to the first run of 0xEE fill, so a wrong retlen is visible */
    for (i = 0; i < 512 && i < (int)sizeof big; i++) {
        if (i % 32 == 0) printf("\n  %04x:", i);
        printf(" %02x", big[i]);
        if (i > rl + 8 && big[i] == 0xEE && big[i - 1] == 0xEE && big[i - 2] == 0xEE) break;
    }
    printf("\n");
}

static const struct { const char *n; int c; } items[] = {
    { "CPUINTSTK", RMI$_CPUINTSTK }, { "CPUMPSYNCH", RMI$_CPUMPSYNCH }, { "CPUKERNEL", RMI$_CPUKERNEL },
    { "CPUEXEC", RMI$_CPUEXEC }, { "CPUSUPER", RMI$_CPUSUPER }, { "CPUUSER", RMI$_CPUUSER },
    { "CPUCOMPAT", RMI$_CPUCOMPAT }, { "CPUIDLE", RMI$_CPUIDLE }, { "KERNEL", RMI$_KERNEL },
    { "IDLE", RMI$_IDLE }, { "COM", RMI$_COM }, { "COMO", RMI$_COMO }, { "PROCS", RMI$_PROCS },
    { "FRLIST", RMI$_FRLIST }, { "MODLIST", RMI$_MODLIST }, { "CACHE_MEMORY", RMI$_CACHE_MEMORY },
    { "CACHE_FREE", RMI$_CACHE_FREE }, { "DIRIO", RMI$_DIRIO }, { "BUFIO", RMI$_BUFIO },
    { "OPCNT", RMI$_OPCNT }, { "IOQUELEN", RMI$_IOQUELEN }
};
#define NITEMS (sizeof items / sizeof items[0])

int main(void)
{
    unsigned __int64 v[NITEMS];
    unsigned short rl[NITEMS];
    struct item il[NITEMS + 1];
    int i, len, st;
    for (i = 0; i < (int)NITEMS; i++) {
        for (len = 4; len <= 8; len += 4) {
            unsigned short r = 0;
            unsigned __int64 x = 0;
            struct item one[2];
            memset(one, 0, sizeof one);
            one[0].len = (unsigned short)len; one[0].code = (unsigned short)items[i].c; one[0].buf = &x; one[0].retlen = &r;
            st = call(one);
            printf("%-13s len=%d st=%-6d rl=%d value=%llu\n", items[i].n, len, st, r, x);
        }
    }
    memset(il, 0, sizeof il);
    memset(v, 0, sizeof v);
    for (i = 0; i < (int)NITEMS; i++) {
        il[i].len = 4; il[i].code = (unsigned short)items[i].c; il[i].buf = &v[i]; il[i].retlen = &rl[i];
    }
    st = call(il);
    printf("one list, all len 4: st=%d\n", st);
    for (i = 0; i < 2; i++) {
        dump("MODES", RMI$_MODES);
        dump("DISKS", RMI$_DISKS);
        if (i == 0) sleep(5);
    }
    return 1;
}
