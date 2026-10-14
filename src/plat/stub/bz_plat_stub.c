/* bz_plat_stub.c - the platform layer on Linux, with scripted system counters.
 *
 * Used for the unit and integration tests and for protocol work without a VMS
 * node.  The "system" is an imaginary 2-CPU OpenVMS host whose load follows a
 * control file (BZ_STUB_CONTROL), re-read on every sample:
 *     cpu <percent busy>        default 20
 *     rx <bytes/s>  tx <bytes/s> on EWA0, defaults 20000 / 5000
 * Settings come from BESZEL_AGENT_<NAME> environment variables, upstream's own
 * prefix, then the config file.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bz_agent.h"
#include "bz_platform.h"
#include "bz_text.h"

#define NCPU 2

static volatile sig_atomic_t stop_flag;
static uint64_t t0;
static struct bz_sample acc;            /* cumulative counters */
static uint64_t last_ms;
static double cpu_pct = 20, rx_rate = 20000, tx_rate = 5000;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

uint64_t bz_plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void bz_plat_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int bz_plat_read_file(const char *path, bz_buf *out)
{
    FILE *f = fopen(path, "rb");
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
    char tmp[1100];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return 0;
    chmod(tmp, 0600);
    if (fwrite(p, 1, n, f) != n) {
        fclose(f);
        remove(tmp);
        return 0;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

int bz_plat_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int bz_plat_remove(const char *path)
{
    return remove(path) == 0;
}

int bz_plat_file_exposed(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 044) != 0;
}

void bz_plat_data_path(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

const char *bz_plat_default_config(void)
{
    return "beszel-agent.conf";
}

const char *bz_plat_default_data_dir(void)
{
    return "beszel-data";
}

const char *bz_plat_default_ca_file(void)
{
    return NULL;                        /* the system store */
}

const char *bz_plat_default_root_fs(void)
{
    return "DKA0:";
}

int bz_plat_conf_override(void *ctx, const char *name, bz_buf *out)
{
    char env[64];
    const char *v;
    (void)ctx;
    snprintf(env, sizeof env, "BESZEL_AGENT_%s", name);
    v = getenv(env);
    if (!v)
        return 0;
    bz_buf_puts(out, v);
    return 1;
}

void bz_plat_log(int level, const char *msg)
{
    static const char *const names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    time_t now = time(NULL);
    struct tm tm;
    char ts[32];
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y/%m/%d %H:%M:%S", &tm);
    {
        char clean[600];
        size_t i;
        for (i = 0; msg[i] && i < sizeof clean - 1; i++)
            clean[i] = ((unsigned char)msg[i] < 0x20 || msg[i] == 0x7F) ? '?' : msg[i];
        clean[i] = 0;
        fprintf(stderr, "%s %s %s\n", ts, names[level & 3], clean);
    }
    fflush(stderr);
}

int bz_plat_init(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    t0 = last_ms = bz_plat_now_ms();
    /* boot-time counters: a day of uptime at 5% busy */
    {
        int i;
        uint64_t day = 86400ULL * 100;
        acc.ncpu = NCPU;
        for (i = 0; i < NCPU; i++) {
            acc.cpu[i].total = day;
            acc.cpu[i].idle = day * 95 / 100;
            acc.cpu[i].user = day * 3 / 100;
            acc.cpu[i].system = day * 2 / 100;
        }
        acc.nnic = 2;
        strcpy(acc.nic[0].name, "EWA0");
        acc.nic[0].rx = 3000000000ULL;
        acc.nic[0].tx = 900000000ULL;
        strcpy(acc.nic[1].name, "EWB0");    /* configured, never used: no traffic */
    }
    return 1;
}

int bz_plat_check_account(char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    return 1;           /* the Linux stub is a test platform: no account rules */
}

int bz_plat_identity(struct bz_identity *id)
{
    const char *h = getenv("BZ_STUB_HOSTNAME");
    memset(id, 0, sizeof *id);
    bz_strlcpy(id->hostname, h ? h : "stubvms", sizeof id->hostname);
    bz_strlcpy(id->kernel, "OpenVMS E9.2-4", sizeof id->kernel);
    bz_strlcpy(id->os_name, "OpenVMS E9.2-4 (x86-64)", sizeof id->os_name);
    bz_strlcpy(id->cpu_model, "QEMU Standard PC (stub)", sizeof id->cpu_model);
    bz_strlcpy(id->arch, "x86_64", sizeof id->arch);
    id->cores = id->threads = NCPU;
    id->mem_total = 8ULL << 30;
    snprintf(id->fp_seed, sizeof id->fp_seed, "%s", "5354554253545542535455425354554200");
    return 1;
}

static void read_control(void)
{
    const char *path = getenv("BZ_STUB_CONTROL");
    FILE *f;
    char line[128];
    double v;
    if (!path || !(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "cpu %lf", &v) == 1)
            cpu_pct = v < 0 ? 0 : v > 100 ? 100 : v;
        else if (sscanf(line, "rx %lf", &v) == 1)
            rx_rate = v;
        else if (sscanf(line, "tx %lf", &v) == 1)
            tx_rate = v;
    }
    fclose(f);
}

int bz_plat_sample(struct bz_sample *s)
{
    uint64_t now = bz_plat_now_ms(), dt = now - last_ms, ticks = dt / 10;
    int i;
    read_control();
    if (ticks) {
        last_ms += ticks * 10;
        for (i = 0; i < NCPU; i++) {
            uint64_t busy = (uint64_t)((double)ticks * cpu_pct / 100 + 0.5);
            acc.cpu[i].total += ticks;
            acc.cpu[i].idle += ticks - busy;
            acc.cpu[i].user += busy * 6 / 10;
            acc.cpu[i].system += busy - busy * 6 / 10;
        }
        acc.nic[0].rx += (uint64_t)(rx_rate * (double)ticks / 100);
        acc.nic[0].tx += (uint64_t)(tx_rate * (double)ticks / 100);
    }
    memset(&acc.all, 0, sizeof acc.all);
    for (i = 0; i < NCPU; i++) {
        acc.all.total += acc.cpu[i].total;
        acc.all.idle += acc.cpu[i].idle;
        acc.all.user += acc.cpu[i].user;
        acc.all.system += acc.cpu[i].system;
    }
    acc.mem_total = 8ULL << 30;
    acc.mem_used = (3ULL << 30) + (uint64_t)(cpu_pct * 10485760);  /* + 10 MiB per % busy */
    acc.mem_buffcache = 0;
    acc.swap_total = 2ULL << 30;
    acc.swap_used = 128ULL << 20;
    acc.uptime_s = 86400 + (now - t0) / 1000;
    *s = acc;
    return 1;
}

void bz_plat_fs_usage(const char *dev, struct bz_fs_usage *u)
{
    memset(u, 0, sizeof *u);
    if (bz_ieq(dev, "DKA0:")) {
        u->ok = 1;
        u->total = 21474836480ULL;          /* 20 GiB */
        u->used = 10200547328ULL;
    } else if (bz_ieq(dev, "DKA200:")) {
        u->ok = 1;
        u->total = 1010949120ULL;
        u->used = 438910976ULL;
    }
}

double bz_plat_runq(void)
{
    return cpu_pct * NCPU / 100;
}

int bz_plat_stop_requested(void)
{
    return stop_flag;
}
