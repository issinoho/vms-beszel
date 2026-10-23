/* bz_stats.h - beszel statistics from raw counters, and their CBOR encoding.
 *
 * Mirrors upstream's agent 0.21.1 (PROTOCOL.md 4.2-4.7):
 *  - CPU and network deltas are kept per request interval (CacheTimeMs), so the
 *    hub's 1 s real-time view never moves the 60 s baselines; a new interval is
 *    seeded from the 60000 baseline (CPU) or starts at zero rate (network);
 *  - a reply is reused while younger than CacheTimeMs/2, and a reused reply never
 *    carries Details;
 *  - values are rounded as upstream rounds them (two decimals, GiB = 2^30).
 */
#ifndef BZ_STATS_H
#define BZ_STATS_H

#include <stddef.h>
#include <stdint.h>

#include "bz_buf.h"
#include "bz_sys.h"

#define BZ_DEFAULT_INTERVAL_MS 60000
#define BZ_MAX_IVAL 8
#define BZ_LOAD_PERIOD_MS 5000

struct bz_fs_spec {
    char dev[64];       /* what the platform measures */
    char name[64];      /* the display name from "dev__name", or "" */
};

struct bz_stats_cfg {
    struct bz_fs_spec root;
    int nextra;
    struct bz_fs_spec extra[BZ_MAX_FS];
    /* NICS (PROTOCOL.md 7): blacklist if it started with '-' */
    int nics_set, nics_black, nnics;
    char nics[BZ_MAX_NIC][32];
    const char *version;
};

/* "dev[__name]" entries, comma-separated for EXTRA_FILESYSTEMS */
int bz_fs_spec_parse(struct bz_fs_spec *fs, const char *s, size_t n);
int bz_stats_cfg_extra(struct bz_stats_cfg *c, const char *list);
void bz_stats_cfg_nics(struct bz_stats_cfg *c, const char *spec);
int bz_nic_selected(const struct bz_stats_cfg *c, const char *name, uint64_t tx, uint64_t rx);
int bz_glob(const char *pat, const char *s);     /* '*' and '?', case-insensitive */

struct bz_load {
    double avg[3];
    uint64_t last_ms;
    int started;
};
/* feed the run-queue length; applies Linux's damping once per 5 s elapsed */
void bz_load_sample(struct bz_load *l, uint64_t now_ms, double runq);

/* the computed values one reply carries */
struct bz_fs_out {
    char key[64];
    double total, used;     /* GiB, rounded */
};
struct bz_nic_out {
    char name[32];
    uint64_t v[4];          /* up B/s, down B/s, total sent, total received */
};
struct bz_stats_out {
    double cpu;
    double cpub[5];         /* user, system, iowait, steal, idle */
    int ncores;
    unsigned char cores[BZ_MAX_CPU];
    double la[3];
    double mem, mem_used, mem_pct, mem_bc, swap, swap_used;    /* GiB, deprecated in 0.21.1 */
    uint64_t mem_bytes[4];  /* total, used, buff/cache, ZFS ARC (always 0) */
    uint64_t swap_bytes[2]; /* total, used */
    double disk_total, disk_used, disk_pct;
    char rdn[64];
    uint64_t bw[2];
    int nni;
    struct bz_nic_out ni[BZ_MAX_NIC];
    int nefs;
    struct bz_fs_out efs[BZ_MAX_FS];
    uint64_t uptime;
    int threads;
};

struct bz_nic_prev {
    char name[32];
    uint64_t tx, rx;
};
struct bz_ival {
    int used;
    unsigned key;
    struct bz_cpu_times cpu;
    int ncpu;
    struct bz_cpu_times percpu[BZ_MAX_CPU];
    uint64_t net_ms;        /* 0: no baseline yet */
    uint64_t net_tx, net_rx;
    int nprev;
    struct bz_nic_prev prev[BZ_MAX_NIC];
    bz_buf cache;           /* last reply's CombinedData, without Details */
    uint64_t cache_ms;
    int cache_valid;
};

struct bz_fs_usage {
    int ok;
    uint64_t total, used;   /* bytes */
};
typedef void (*bz_fs_usage_fn)(void *ctx, const char *dev, struct bz_fs_usage *u);

struct bz_stats {
    struct bz_stats_cfg cfg;
    struct bz_ival iv[BZ_MAX_IVAL];
    struct bz_load load;
    int details_dirty;
};

void bz_stats_init(struct bz_stats *st);
void bz_stats_free(struct bz_stats *st);
/* the 60000 CPU baseline at start, as upstream's init() takes it */
void bz_stats_seed(struct bz_stats *st, const struct bz_sample *s);
/* compute one reply's values from a fresh sample */
void bz_stats_compute(struct bz_stats *st, unsigned cache_ms, uint64_t now_ms,
                      const struct bz_sample *s, bz_fs_usage_fn fs, void *fs_ctx,
                      struct bz_stats_out *out);
struct bz_ival *bz_stats_ival(struct bz_stats *st, unsigned cache_ms);

/* CombinedData; details may be NULL */
void bz_encode_combined(bz_buf *b, const struct bz_stats_out *o, const struct bz_identity *details,
                        const char *version);
double bz_two_decimals(double v);

#endif
