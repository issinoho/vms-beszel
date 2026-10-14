/* bz_plat_vms.c - the platform layer on OpenVMS (x86-64 and IA64).
 *
 * Every source here was probed in Phase 0 and needs no privilege
 * (docs/NOTES.md 1-15; DECISIONS.md D9):
 *   $GETSYI   identity, memory size, page and swap files, boot time
 *   $GETRMI   per-CPU mode times (RMI$_MODES), free/modified lists, run queue;
 *             every item must be passed its exact size (NOTES.md 1)
 *   $GETDVI   disk size and free blocks
 *   $QIO      LAN read-counters on each LAN device's unit 0 (NOTES.md 12-13)
 *
 * Built with SYS$LIB_C.TLB for NMADEF; nothing from that library is copied
 * into the sources (NOTES.md 18).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <descrip.h>
#include <dcdef.h>
#include <dvidef.h>
#include <dvsdef.h>
#include <efndef.h>
#include <iodef.h>
#include <lnmdef.h>
#include <nmadef.h>
#include <rmidef.h>
#include <ssdef.h>
#include <starlet.h>
#include <syidef.h>

#include "bz_agent.h"
#include "bz_platform.h"
#include "bz_text.h"

/* item lists: our own types, as the default STARLET headers have no IOSB (NOTES.md 16) */
struct item {
    unsigned short len, code;
    void *buf;
    unsigned short *retlen;
};
struct iosb {
    unsigned short st, cnt;
    unsigned int dev;
};

static volatile sig_atomic_t stop_flag;
/* per-CPU mode counters: the last raw reading and our accumulated times */
static unsigned int cpu_prev[BZ_MAX_CPU][8];
static struct bz_cpu_times cpu_acc[BZ_MAX_CPU];
static int cpu_n = -1, cpu_have;
static unsigned int page_size = 8192;
static int cur_size;                    /* RMI$_CUR item size, found at init */

/* ---- time ---- */

static uint64_t vms_time(void)
{
    uint64_t q;
    sys$gettim((void *)&q);             /* 100 ns units since 17-NOV-1858 */
    return q;
}

uint64_t bz_plat_now_ms(void)
{
    /* the system clock can be set back; intervals must never run backwards */
    static uint64_t last;
    uint64_t now = vms_time() / 10000;
    if (now < last)
        now = last;
    last = now;
    return now;
}

void bz_plat_sleep_ms(unsigned ms)
{
    /* usleep() takes less than a second: longer values return at once */
    if (ms >= 1000)
        sleep(ms / 1000);
    if (ms % 1000)
        usleep((ms % 1000) * 1000);
}

/* ---- files ---- */

int bz_plat_read_file(const char *path, bz_buf *out)
{
    FILE *f = fopen(path, "r");
    char tmp[4096];
    size_t n;
    if (!f)
        return 0;
    bz_buf_clear(out);
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        bz_buf_put(out, tmp, n);
    fclose(f);
    return !out->oom;
}

int bz_plat_write_file(const char *path, const void *p, size_t n)
{
    /* owner read/write only: (S:RWED,O:RWED,G,W) in VMS terms */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600, "rfm=stmlf");
    long w;
    if (fd < 0)
        return 0;
    w = (long)write(fd, p, n);
    if (close(fd) != 0 || w != (long)n)
        return 0;
    return 1;
}

int bz_plat_file_exposed(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 044) != 0;
}

void bz_plat_data_path(char *out, size_t cap, const char *dir, const char *name)
{
    /* BESZEL$ROOT:[DATA] + "fingerprint" -> BESZEL$ROOT:[DATA]fingerprint.;
       a Unix-style directory gets a slash */
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == ']' || dir[n - 1] == '>' || dir[n - 1] == ':'))
        snprintf(out, cap, "%s%s.", dir, name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

const char *bz_plat_default_config(void)
{
    return "BESZEL$ROOT:[CONF]AGENT.CONF";
}

const char *bz_plat_default_data_dir(void)
{
    return "BESZEL$ROOT:[DATA]";
}

const char *bz_plat_default_ca_file(void)
{
    /* SSL3 loads no CA store for us (NOTES.md 20): the kit's bundle, if there */
    static const char bundle[] = "BESZEL$ROOT:[CONF]CACERT.PEM";
    struct stat st;
    return stat(bundle, &st) == 0 ? bundle : NULL;
}

const char *bz_plat_default_root_fs(void)
{
    return "SYS$SYSDEVICE:";
}

/* ---- configuration overrides: logical name BESZEL$<NAME> (D5) ---- */

int bz_plat_conf_override(void *ctx, const char *name, bz_buf *out)
{
    char lnm[64], val[256];
    unsigned short vl = 0;
    struct item il[2];
    struct dsc$descriptor_s tab, nam;
    static const char table[] = "LNM$FILE_DEV";
    unsigned int attr = LNM$M_CASE_BLIND;
    int st;
    (void)ctx;
    snprintf(lnm, sizeof lnm, "BESZEL$%s", name);
    tab.dsc$w_length = (unsigned short)strlen(table);
    tab.dsc$b_dtype = DSC$K_DTYPE_T;
    tab.dsc$b_class = DSC$K_CLASS_S;
    tab.dsc$a_pointer = (char *)table;
    nam.dsc$w_length = (unsigned short)strlen(lnm);
    nam.dsc$b_dtype = DSC$K_DTYPE_T;
    nam.dsc$b_class = DSC$K_CLASS_S;
    nam.dsc$a_pointer = lnm;
    memset(il, 0, sizeof il);
    il[0].len = sizeof val;
    il[0].code = LNM$_STRING;
    il[0].buf = val;
    il[0].retlen = &vl;
    st = sys$trnlnm(&attr, &tab, &nam, 0, il);
    if (!(st & 1))
        return 0;
    bz_buf_put(out, val, vl);
    return 1;
}

/* ---- logging: to SYS$OUTPUT, the detached process's log file ---- */

void bz_plat_log(int level, const char *msg)
{
    static const char *const names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    time_t now = time(NULL);
    char ts[32];
    /* single-threaded: localtime is safe, and VSI C hides localtime_r under C99 */
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("%s %-5s %s\n", ts, names[level & 3], msg);
    fflush(stdout);
}

/* ---- system services ---- */

static int syi(int code, void *buf, unsigned short len, unsigned short *rl)
{
    struct item il[2];
    struct iosb iosb;
    unsigned short dummy;
    int st;
    memset(il, 0, sizeof il);
    il[0].len = len;
    il[0].code = (unsigned short)code;
    il[0].buf = buf;
    il[0].retlen = rl ? rl : &dummy;
    st = sys$getsyiw(EFN$C_ENF, 0, 0, il, (void *)&iosb, 0, 0);
    return (st & 1) ? iosb.st & 1 : 0;
}

/* one $GETRMI item, at its exact size (NOTES.md 1).  The return length gets
   8 bytes: RMI$_MODES writes more than the 16-bit word an item list provides
   (its "length" was garbage in Phase 0, and a 2-byte one corrupted the stack). */
static int rmi(int code, void *buf, unsigned short len)
{
    struct item il[2];
    struct iosb iosb;
    uint64_t rl = 0;
    int st;
    memset(il, 0, sizeof il);
    il[0].len = len;
    il[0].code = (unsigned short)code;
    il[0].buf = buf;
    il[0].retlen = (unsigned short *)&rl;
    st = sys$getrmi(EFN$C_ENF, 0, 0, il, (void *)&iosb, 0, 0);
    if (st & 1) {
        sys$synch(EFN$C_ENF, (void *)&iosb);
        st = iosb.st;
    }
    return st & 1;
}

static uint64_t rmi_u(int code, int size)
{
    uint64_t v = 0;
    if (size == 8) {
        if (!rmi(code, &v, 8))
            return 0;
        return v;
    } else {
        unsigned int w = 0;
        if (!rmi(code, &w, 4))
            return 0;
        return w;
    }
}

/* ---- LAN devices: channels kept open, 32-bit counters extended ---- */

struct nic {
    char name[32];
    unsigned short chan;
    int have;
    unsigned int raw_tx, raw_rx;    /* last 32-bit readings */
    uint64_t tx, rx;                /* extended 64-bit totals */
    int warned;
};
static struct nic nics[BZ_MAX_NIC];
static int nnic;
static uint64_t nic_last_ms;
static unsigned char cbuf[4096];

static void nic_open_all(void)
{
    char name[65];
    struct dsc$descriptor_s nd, pat, dev;
    unsigned short nl;
    uint64_t ctx = 0;
    int devclass = DC$_SCOM;
    struct item scan[2];
    static char star[] = "*";
    memset(scan, 0, sizeof scan);
    scan[0].len = 4;
    scan[0].code = DVS$_DEVCLASS;
    scan[0].buf = &devclass;
    nd.dsc$w_length = 64; nd.dsc$b_dtype = DSC$K_DTYPE_T; nd.dsc$b_class = DSC$K_CLASS_S;
    nd.dsc$a_pointer = name;
    pat.dsc$w_length = 1; pat.dsc$b_dtype = DSC$K_DTYPE_T; pat.dsc$b_class = DSC$K_CLASS_S;
    pat.dsc$a_pointer = star;
    while (nnic < BZ_MAX_NIC) {
        unsigned int unit = 1;
        struct item il[2];
        struct iosb iosb;
        unsigned short chan = 0;
        const char *s, *e;
        int st = sys$device_scan(&nd, &nl, &pat, scan, (void *)&ctx);
        if (!(st & 1))
            break;
        dev.dsc$w_length = nl; dev.dsc$b_dtype = DSC$K_DTYPE_T; dev.dsc$b_class = DSC$K_CLASS_S;
        dev.dsc$a_pointer = name;
        memset(il, 0, sizeof il);
        il[0].len = 4;
        il[0].code = DVI$_UNIT;
        il[0].buf = &unit;
        if (!(sys$getdviw(EFN$C_ENF, 0, &dev, il, (void *)&iosb, 0, 0, 0) & 1) || unit != 0)
            continue;           /* template units (0) carry the device counters */
        if (!(sys$assign(&dev, &chan, 0, 0, 0) & 1))
            continue;           /* e.g. RMA0: not ours to read (NOTES.md 15) */
        name[nl] = 0;
        /* "_NODE$EWA0:" -> "EWA0" */
        s = name;
        if (*s == '_')
            s++;
        if ((e = strchr(s, '$')) != NULL)
            s = e + 1;
        e = strchr(s, ':');
        bz_strlcpyn(nics[nnic].name, sizeof nics[0].name, s, e ? (size_t)(e - s) : strlen(s));
        nics[nnic].chan = chan;
        nnic++;
    }
}

static int nic_read(struct nic *n, unsigned int *tx, unsigned int *rx)
{
    struct dsc$descriptor_s bd;
    struct iosb iosb;
    int st, i, got = 0;
    bd.dsc$w_length = sizeof cbuf; bd.dsc$b_dtype = DSC$K_DTYPE_T; bd.dsc$b_class = DSC$K_CLASS_S;
    bd.dsc$a_pointer = (char *)cbuf;
    st = sys$qiow(EFN$C_ENF, n->chan, IO$_SENSEMODE | IO$M_CTRL | IO$M_RD_COUNT, (void *)&iosb,
                  0, 0, 0, (unsigned long)&bd, 0, 0, 0, 0);
    if (!(st & 1) || !(iosb.st & 1))
        return 0;
    /* NMA counters: a 16-bit header (type, width), then the value */
    for (i = 0; i + 2 <= (int)iosb.cnt;) {
        unsigned short h;
        int type, w, bytes;
        unsigned int v = 0;
        memcpy(&h, cbuf + i, 2);
        i += 2;
        type = h & NMA$M_CNT_TYP;
        w = (h & NMA$M_CNT_WID) >> 13;
        bytes = w == 1 ? 1 : w == 2 ? 2 : w == 3 ? 4 : 8;
        if (h & NMA$M_CNT_MAP)
            bytes += 2;
        if (i + bytes > (int)iosb.cnt)
            break;
        if (bytes == 4)
            memcpy(&v, cbuf + i, 4);
        if (type == NMA$C_CTLIN_BSN && bytes == 4) {
            *tx = v;
            got |= 1;
        } else if (type == NMA$C_CTLIN_BRC && bytes == 4) {
            *rx = v;
            got |= 2;
        }
        i += bytes;
    }
    return got == 3;
}

static void extend(struct nic *n, unsigned int raw, unsigned int *last, uint64_t *total, const char *dir)
{
    /* a 32-bit counter that went down has wrapped: add the distance mod 2^32.
       DECnet-style counters may instead stop at 0xFFFFFFFF (NOTES.md 13). */
    *total += (unsigned int)(raw - *last);
    *last = raw;
    if (raw == 0xFFFFFFFFu && !n->warned) {
        char msg[200];
        n->warned = 1;
        snprintf(msg, sizeof msg, "LAN counter for %s bytes %s is at its 32-bit maximum; if it stays "
                 "there, its traffic is no longer counted until the counters are zeroed", n->name, dir);
        bz_plat_log(BZ_LOG_WARN, msg);
    }
}

static void nic_refresh(void)
{
    int i;
    nic_last_ms = bz_plat_now_ms();
    for (i = 0; i < nnic; i++) {
        struct nic *n = &nics[i];
        unsigned int tx, rx;
        if (!nic_read(n, &tx, &rx))
            continue;
        if (!n->have) {
            n->have = 1;
            n->raw_tx = tx;
            n->raw_rx = rx;
            n->tx = tx;
            n->rx = rx;
            continue;
        }
        extend(n, tx, &n->raw_tx, &n->tx, "sent");
        extend(n, rx, &n->raw_rx, &n->rx, "received");
    }
}

/* ---- the platform interface ---- */

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

int bz_plat_init(void)
{
    unsigned int w = 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);           /* not declared by VSI C under /STANDARD=C99 */
#endif
    syi(SYI$_PAGE_SIZE, &page_size, 4, NULL);
    /* RMI$_CUR's size is not documented: find it once */
    if (rmi(RMI$_CUR, &w, 4))
        cur_size = 4;
    else {
        uint64_t q;
        cur_size = rmi(RMI$_CUR, &q, 8) ? 8 : 0;
    }
    nic_open_all();
    nic_refresh();
    return 1;
}

int bz_plat_identity(struct bz_identity *id)
{
    char ver[9], hw[96], arch[16], node[16];
    unsigned char uuid[16];
    unsigned short vl = 0, hl = 0, al = 0, nl = 0, ul = 0;
    uint64_t memsize = 0;
    unsigned int cpus = 0;
    const char *v;
    size_t n;
    int i, zero = 1;
    memset(id, 0, sizeof *id);
    syi(SYI$_VERSION, ver, 8, &vl);
    syi(SYI$_HW_NAME, hw, sizeof hw - 1, &hl);
    syi(SYI$_ARCH_NAME, arch, sizeof arch - 1, &al);
    syi(SYI$_NODENAME, node, sizeof node - 1, &nl);
    syi(SYI$_ACTIVECPU_CNT, &cpus, 4, NULL);
    syi(SYI$_MEMSIZE, &memsize, 8, NULL);
    syi(SYI$_SYSTEM_UUID, uuid, 16, &ul);
    v = ver;
    n = vl;
    bz_trim(&v, &n);
    hw[hl] = 0;
    arch[al] = 0;
    node[nl] = 0;
    if (gethostname(id->hostname, sizeof id->hostname - 1) != 0 || !id->hostname[0])
        bz_strlcpy(id->hostname, node, sizeof id->hostname);
    snprintf(id->kernel, sizeof id->kernel, "OpenVMS %.*s", (int)n, v);
    snprintf(id->os_name, sizeof id->os_name, "OpenVMS %.*s (%s)", (int)n, v,
             strcmp(arch, "IA64") == 0 ? "Integrity" : strcmp(arch, "x86_64") == 0 ? "x86-64" : arch);
    bz_strlcpy(id->cpu_model, hw, sizeof id->cpu_model);
    bz_strlcpy(id->arch, arch, sizeof id->arch);
    id->cores = id->threads = (int)cpus;
    id->mem_total = memsize * page_size;
    /* fingerprint seed (D9): the UUID as F$GETSYI prints it (bytes reversed) */
    for (i = 0; i < 16 && ul == 16; i++)
        if (uuid[i])
            zero = 0;
    if (ul == 16 && !zero) {
        char *p = id->fp_seed;
        for (i = 15; i >= 0; i--, p += 2)
            sprintf(p, "%02X", uuid[i]);
    } else {
        snprintf(id->fp_seed, sizeof id->fp_seed, "%s%s", id->hostname, id->cpu_model);
    }
    return 1;
}

int bz_plat_sample(struct bz_sample *s)
{
    static unsigned char modes[4 + BZ_MAX_CPU * 33 + 512];
    static int warned_cpus;
    unsigned int ncpu = 0, i;
    uint64_t memsize = 0, free_p, mod_p, now, boot = 0;
    unsigned int pf_page = 0, pf_free = 0, sw_page = 0, sw_free = 0;
    memset(s, 0, sizeof *s);

    /* RMI$_MODES: a CPU count, then per CPU an id byte and eight longwords of
       10 ms ticks: [0] interrupt+idle, [1] MP synch, [2] kernel, [3] exec,
       [4] super, [5] user, [6] compat, [7] idle.  The return length is wrong
       on both architectures, so the count decides (NOTES.md 3, 5). */
    memset(modes, 0, sizeof modes);
    if (rmi(RMI$_MODES, modes, (unsigned short)sizeof modes)) {
        memcpy(&ncpu, modes, 4);
        if (ncpu > BZ_MAX_CPU) {
            if (!warned_cpus) {
                warned_cpus = 1;
                bz_plat_log(BZ_LOG_WARN, "more CPUs than this agent tracks; per-CPU charts show the first 64");
            }
            ncpu = BZ_MAX_CPU;
        }
        if ((int)ncpu != cpu_n) {        /* the CPU set changed: start again */
            memset(cpu_prev, 0, sizeof cpu_prev);
            memset(cpu_acc, 0, sizeof cpu_acc);
            cpu_n = (int)ncpu;
            cpu_have = 0;
        }
        for (i = 0; i < ncpu; i++) {
            unsigned int l[8], d[8];
            struct bz_cpu_times *c = &cpu_acc[i];
            int k;
            memcpy(l, modes + 4 + i * 33 + 1, sizeof l);
            /* Accumulate from deltas: [0] includes idle, but on x86 [7] can
               advance more than [0] in an interval (NOTES.md 4, 29).  Each
               interval's total is max(d0, d7) plus the other modes, so busy,
               idle, user and system always add up to the total. */
            for (k = 0; k < 8; k++)
                d[k] = cpu_have ? l[k] - cpu_prev[i][k] : 0;   /* mod 2^32; 0 on the first reading */
            memcpy(cpu_prev[i], l, sizeof l);
            {
                uint64_t intr = d[0] > d[7] ? d[0] - d[7] : 0;
                uint64_t user = (uint64_t)d[5] + d[4];                         /* user + super */
                uint64_t sys = intr + d[1] + d[2] + d[3] + d[6];                /* kernel, exec, MP synch, interrupt, compat */
                c->total += (d[0] > d[7] ? d[0] : d[7]) + d[1] + d[2] + d[3] + d[4] + d[5] + d[6];
                c->idle += d[7];
                c->user += user;
                c->system += sys;
            }
            s->cpu[i] = *c;
            s->all.total += c->total;
            s->all.idle += c->idle;
            s->all.user += c->user;
            s->all.system += c->system;
        }
        cpu_have = 1;
        s->ncpu = (int)ncpu;
    }

    /* memory: used = total - free list - modified list; XFC can't be measured
       (NOTES.md 6, 23), so it counts as used */
    syi(SYI$_MEMSIZE, &memsize, 8, NULL);
    free_p = rmi_u(RMI$_FRLIST, 8);
    mod_p = rmi_u(RMI$_MODLIST, 8);
    s->mem_total = memsize * page_size;
    s->mem_used = memsize > free_p + mod_p ? (memsize - free_p - mod_p) * page_size : 0;

    /* "swap": page files plus swap files (D9) */
    syi(SYI$_PAGEFILE_PAGE, &pf_page, 4, NULL);
    syi(SYI$_PAGEFILE_FREE, &pf_free, 4, NULL);
    syi(SYI$_SWAPFILE_PAGE, &sw_page, 4, NULL);
    syi(SYI$_SWAPFILE_FREE, &sw_free, 4, NULL);
    s->swap_total = ((uint64_t)pf_page + sw_page) * page_size;
    s->swap_used = ((uint64_t)(pf_page > pf_free ? pf_page - pf_free : 0) +
                    (sw_page > sw_free ? sw_page - sw_free : 0)) * page_size;

    nic_refresh();
    for (i = 0; i < (unsigned)nnic; i++) {
        if (!nics[i].have)
            continue;
        bz_strlcpy(s->nic[s->nnic].name, nics[i].name, sizeof s->nic[0].name);
        s->nic[s->nnic].tx = nics[i].tx;
        s->nic[s->nnic].rx = nics[i].rx;
        s->nnic++;
    }

    syi(SYI$_BOOTTIME, &boot, 8, NULL);
    now = vms_time();
    s->uptime_s = now > boot && boot ? (now - boot) / 10000000 : 0;
    return s->ncpu > 0;
}

void bz_plat_fs_usage(const char *dev, struct bz_fs_usage *u)
{
    struct dsc$descriptor_s d;
    struct item il[4];
    struct iosb iosb;
    unsigned int mnt = 0, maxb = 0, freeb = 0;
    memset(u, 0, sizeof *u);
    d.dsc$w_length = (unsigned short)strlen(dev);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)dev;
    memset(il, 0, sizeof il);
    il[0].len = 4; il[0].code = DVI$_MNT; il[0].buf = &mnt;
    il[1].len = 4; il[1].code = DVI$_MAXBLOCK; il[1].buf = &maxb;
    il[2].len = 4; il[2].code = DVI$_FREEBLOCKS; il[2].buf = &freeb;
    if (!(sys$getdviw(EFN$C_ENF, 0, &d, il, (void *)&iosb, 0, 0, 0) & 1) || !(iosb.st & 1) || !mnt)
        return;
    u->ok = 1;
    u->total = (uint64_t)maxb * 512;
    u->used = maxb > freeb ? (uint64_t)(maxb - freeb) * 512 : 0;
}

double bz_plat_runq(void)
{
    /* Running and runnable processes: CUR + COM + COMO (D9), less one: this
       process is necessarily current while it asks, and Linux's in-kernel
       sample never counts its sampler (NOTES.md 30).  Also keeps the LAN
       counters sampled at least every 5 s, so a 32-bit wrap is never missed
       (4 GiB takes 34 s at 1 Gb/s). */
    uint64_t cur;
    if (bz_plat_now_ms() - nic_last_ms >= 5000)
        nic_refresh();
    cur = rmi_u(RMI$_CUR, cur_size ? cur_size : 4);
    return (double)((cur ? cur - 1 : 0) + rmi_u(RMI$_COM, 4) + rmi_u(RMI$_COMO, 4));
}

int bz_plat_stop_requested(void)
{
    return stop_flag;
}
