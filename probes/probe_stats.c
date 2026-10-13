/* probe_stats.c - Phase 0 probe: what the system services give an agent.
 *
 * Takes two snapshots <secs> apart (default 5) of:
 *   $GETSYI  identity, memory, page/swap files, CPU counts, UUID
 *   $GETRMI  per-mode CPU time, compute queues, free/modified lists, XFC,
 *            and the raw RMI$_MODES / RMI$_DISKS records (hex), one item per
 *            call so each item's status is visible on its own
 *   $GETDVI  every disk: mounted, size, free, operation count, queue length
 *   LAN      $QIO read-counters on unit 0 of every LAN device, decoded
 * and prints raw values, return lengths and statuses (no interpretation beyond
 * the counter decode).  Run once without privileges and once with.
 *
 * Build: CC PROBE_STATS+SYS$LIBRARY:SYS$LIB_C.TLB/LIBRARY ; LINK PROBE_STATS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <starlet.h>
#include <descrip.h>
#include <ssdef.h>
#include <syidef.h>
#include <rmidef.h>
#include <dvidef.h>
#include <dvsdef.h>
#include <dcdef.h>
#include <iodef.h>
#include <efndef.h>
#include <nmadef.h>

struct item { unsigned short len, code; void *buf; unsigned short *retlen; };
/* our own IOSB: the default (old) STARLET headers have no IOSB typedef */
struct iosb { unsigned short st, cnt; unsigned int dev; };

static unsigned char big[65536];

static void hex(const unsigned char *p, int n, int max)
{
    int i;
    for (i = 0; i < n && i < max; i++) {
        if (i % 32 == 0) printf("\n      %04x:", i);
        printf(" %02x", p[i]);
    }
    if (n > max) printf("\n      ... (%d more bytes)", n - max);
    printf("\n");
}

static void syi(const char *name, int code, int is_str)
{
    unsigned char buf[256];
    unsigned short rl = 0;
    struct item il[2];
    struct iosb iosb;
    int st;
    memset(buf, 0, sizeof buf);
    memset(il, 0, sizeof il);
    il[0].len = sizeof buf; il[0].code = (unsigned short)code; il[0].buf = buf; il[0].retlen = &rl;
    st = sys$getsyiw(EFN$C_ENF, 0, 0, il, &iosb, 0, 0);
    if (st & 1) st = iosb.st;
    printf("SYI %-16s st=%-8d rl=%-3d ", name, st, rl);
    if (!(st & 1)) { printf("\n"); return; }
    if (is_str) printf("\"%.*s\"\n", rl, buf);
    else if (rl == 8) printf("u64=%llu\n", *(unsigned __int64 *)buf);
    else if (rl == 16) { printf("bytes"); hex(buf, rl, 16); }
    else printf("u32=%u\n", *(unsigned int *)buf);
}

static int rmi(const char *name, int code, int dump)
{
    unsigned short rl = 0;
    struct item il[2];
    struct iosb iosb;
    int st;
    memset(big, 0, 64);
    memset(il, 0, sizeof il);
    il[0].len = dump ? sizeof big : 64; il[0].code = (unsigned short)code; il[0].buf = big; il[0].retlen = &rl;
    st = sys$getrmi(EFN$C_ENF, 0, 0, il, &iosb, 0, 0);
    if (st & 1) { sys$synch(EFN$C_ENF, &iosb); st = iosb.st; }
    printf("RMI %-20s st=%-8d rl=%-5d", name, st, rl);
    if (!(st & 1)) { printf("\n"); return 0; }
    if (dump) hex(big, rl, 1024);
    else if (rl == 8) printf(" u64=%llu\n", *(unsigned __int64 *)big);
    else if (rl == 4) printf(" u32=%u\n", *(unsigned int *)big);
    else hex(big, rl, 64);
    return rl;
}

static void disks(void)
{
    char name[65];
    struct dsc$descriptor_s nd = { 64, DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
    unsigned short nl;
    unsigned __int64 ctx = 0;
    int devclass = DC$_DISK;
    struct item scan[2];
    memset(scan, 0, sizeof scan);
    scan[0].len = 4; scan[0].code = DVS$_DEVCLASS; scan[0].buf = &devclass;
    for (;;) {
        $DESCRIPTOR(pat, "*");
        int st = sys$device_scan(&nd, &nl, &pat, scan, &ctx);
        unsigned int mnt = 0, maxb = 0, freeb = 0, opcnt = 0, qlen = 0, errcnt = 0, shm = 0, shmbr = 0;
        char vol[13], dname[65];
        unsigned short vl = 0, dl = 0;
        struct item il[11];
        struct iosb iosb;
        struct dsc$descriptor_s dev = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
        if (!(st & 1)) { if (st != SS$_NOMOREDEV) printf("device_scan st=%d\n", st); break; }
        dev.dsc$w_length = nl;
        memset(il, 0, sizeof il);
        il[0].len = 4; il[0].code = DVI$_MNT; il[0].buf = &mnt;
        il[1].len = 4; il[1].code = DVI$_MAXBLOCK; il[1].buf = &maxb;
        il[2].len = 4; il[2].code = DVI$_FREEBLOCKS; il[2].buf = &freeb;
        il[3].len = 4; il[3].code = DVI$_OPCNT; il[3].buf = &opcnt;
        il[4].len = 4; il[4].code = DVI$_QLEN; il[4].buf = &qlen;
        il[5].len = 4; il[5].code = DVI$_ERRCNT; il[5].buf = &errcnt;
        il[6].len = 12; il[6].code = DVI$_VOLNAM; il[6].buf = vol; il[6].retlen = &vl;
        il[7].len = 64; il[7].code = DVI$_DISPLAY_DEVNAM; il[7].buf = dname; il[7].retlen = &dl;
        il[8].len = 4; il[8].code = DVI$_SHDW_MASTER; il[8].buf = &shm;
        il[9].len = 4; il[9].code = DVI$_SHDW_MEMBER; il[9].buf = &shmbr;
        st = sys$getdviw(EFN$C_ENF, 0, &dev, il, &iosb, 0, 0, 0);
        if (st & 1) st = iosb.st;
        printf("DVI %-24.*s st=%d mnt=%u max=%u free=%u opcnt=%u qlen=%u err=%u vol=\"%.*s\" disp=\"%.*s\" shmaster=%u shmember=%u\n",
               nl, name, st, mnt, maxb, freeb, opcnt, qlen, errcnt, vl, vol, dl, dname, shm, shmbr);
    }
}

static void lan(void)
{
    char name[65];
    struct dsc$descriptor_s nd = { 64, DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
    unsigned short nl;
    unsigned __int64 ctx = 0;
    int devclass = DC$_SCOM;
    struct item scan[2];
    memset(scan, 0, sizeof scan);
    scan[0].len = 4; scan[0].code = DVS$_DEVCLASS; scan[0].buf = &devclass;
    for (;;) {
        $DESCRIPTOR(pat, "*");
        int st = sys$device_scan(&nd, &nl, &pat, scan, &ctx);
        unsigned short chan = 0;
        unsigned int unit = 0;
        struct item il[2];
        struct iosb iosb;
        struct dsc$descriptor_s dev = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
        struct dsc$descriptor_s bd = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)big };
        int n, i;
        if (!(st & 1)) { if (st != SS$_NOMOREDEV) printf("lan scan st=%d\n", st); break; }
        dev.dsc$w_length = nl;
        memset(il, 0, sizeof il);
        il[0].len = 4; il[0].code = DVI$_UNIT; il[0].buf = &unit;
        sys$getdviw(EFN$C_ENF, 0, &dev, il, &iosb, 0, 0, 0);
        if (unit != 0) continue;            /* template units carry the device counters */
        st = sys$assign(&dev, &chan, 0, 0, 0);
        printf("LAN %-20.*s assign st=%d", nl, name, st);
        if (!(st & 1)) { printf("\n"); continue; }
        bd.dsc$w_length = 4096;
        memset(big, 0, 4096);
        st = sys$qiow(EFN$C_ENF, chan, IO$_SENSEMODE | IO$M_CTRL | IO$M_RD_COUNT, &iosb,
                      0, 0, 0, &bd, 0, 0, 0, 0);
        if (st & 1) st = iosb.st;
        n = iosb.cnt;
        printf(" qio st=%d bytes=%d\n", st, n);
        if (st & 1) {
            /* NMA counter list: 16-bit header (type, width bits), value */
            i = 0;
            while (i + 2 <= n) {
                unsigned short h = *(unsigned short *)(big + i);
                int type = h & NMA$M_CNT_TYP, w = (h & NMA$M_CNT_WID) >> 13, bytes;
                unsigned __int64 v = 0;
                i += 2;
                bytes = w == 1 ? 1 : w == 2 ? 2 : w == 3 ? 4 : 8;
                if (h & NMA$M_CNT_MAP) { bytes = 2 + bytes; }
                if (i + bytes > n) break;
                memcpy(&v, big + i, bytes > 8 ? 8 : bytes);
                if (type == NMA$C_CTLIN_BRC || type == NMA$C_CTLIN_BSN || type == NMA$C_CTLIN_DBR ||
                    type == NMA$C_CTLIN_DBS || type == NMA$C_CTLIN_MBY)
                    printf("    ctr type=%d width=%d map=%d value=%llu %s\n", type, bytes, (h & NMA$M_CNT_MAP) != 0, v,
                           type == NMA$C_CTLIN_BRC ? "(bytes received)" : type == NMA$C_CTLIN_BSN ? "(bytes sent)" :
                           type == NMA$C_CTLIN_DBR ? "(blocks received)" : type == NMA$C_CTLIN_DBS ? "(blocks sent)" :
                           "(multicast bytes received)");
                i += bytes;
            }
            printf("    raw first 96 bytes:"); hex(big, n, 96);
        }
        sys$dassgn(chan);
    }
}

static void snapshot(int n)
{
    printf("===== snapshot %d\n", n);
    rmi("CPUKERNEL", RMI$_CPUKERNEL, 0);
    rmi("CPUEXEC", RMI$_CPUEXEC, 0);
    rmi("CPUSUPER", RMI$_CPUSUPER, 0);
    rmi("CPUUSER", RMI$_CPUUSER, 0);
    rmi("CPUINTSTK", RMI$_CPUINTSTK, 0);
    rmi("CPUMPSYNCH", RMI$_CPUMPSYNCH, 0);
    rmi("CPUCOMPAT", RMI$_CPUCOMPAT, 0);
    rmi("CPUIDLE", RMI$_CPUIDLE, 0);
    rmi("COM", RMI$_COM, 0);
    rmi("COMO", RMI$_COMO, 0);
    rmi("PROCS", RMI$_PROCS, 0);
    rmi("FRLIST", RMI$_FRLIST, 0);
    rmi("MODLIST", RMI$_MODLIST, 0);
    rmi("CACHE_MEMORY", RMI$_CACHE_MEMORY, 0);
    rmi("CACHE_FREE", RMI$_CACHE_FREE, 0);
    rmi("DIRIO", RMI$_DIRIO, 0);
    rmi("BUFIO", RMI$_BUFIO, 0);
    rmi("MODES", RMI$_MODES, 1);
    rmi("DISKS", RMI$_DISKS, 1);
    disks();
    lan();
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    syi("VERSION", SYI$_VERSION, 1);
    syi("NODENAME", SYI$_NODENAME, 1);
    syi("SCSNODE", SYI$_SCSNODE, 1);
    syi("HW_NAME", SYI$_HW_NAME, 1);
    syi("ARCH_NAME", SYI$_ARCH_NAME, 1);
    syi("HW_MODEL", SYI$_HW_MODEL, 0);
    syi("SERIAL_NUMBER", SYI$_SERIAL_NUMBER, 1);
    syi("SYSTEM_UUID", SYI$_SYSTEM_UUID, 0);
    syi("BOOTTIME", SYI$_BOOTTIME, 0);
    syi("ACTIVECPU_CNT", SYI$_ACTIVECPU_CNT, 0);
    syi("AVAILCPU_CNT", SYI$_AVAILCPU_CNT, 0);
    syi("ACTIVE_CPU_MASK", SYI$_ACTIVE_CPU_MASK, 0);
    syi("PAGE_SIZE", SYI$_PAGE_SIZE, 0);
    syi("MEMSIZE", SYI$_MEMSIZE, 0);
    syi("PAGEFILE_PAGE", SYI$_PAGEFILE_PAGE, 0);
    syi("PAGEFILE_FREE", SYI$_PAGEFILE_FREE, 0);
    syi("SWAPFILE_PAGE", SYI$_SWAPFILE_PAGE, 0);
    syi("SWAPFILE_FREE", SYI$_SWAPFILE_FREE, 0);
    snapshot(1);
    sleep(secs);
    snapshot(2);
    return 1;
}
