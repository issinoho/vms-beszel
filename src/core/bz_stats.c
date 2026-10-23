/* bz_stats.c - beszel statistics from raw counters (see bz_stats.h).
 *
 * Line references are to the pinned upstream tree (cache/beszel-0.21.1).
 */
#include <math.h>
#include <string.h>

#include "bz_cbor.h"
#include "bz_stats.h"
#include "bz_text.h"

#define GIB 1073741824.0

double bz_two_decimals(double v)
{
    return round(v * 100) / 100;                /* utils.TwoDecimals */
}

static double to_gib(uint64_t b)
{
    return bz_two_decimals((double)b / GIB);    /* utils.BytesToGigabytes */
}

static double clamp_pct(double v)
{
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

/* ---- configuration ---- */

int bz_fs_spec_parse(struct bz_fs_spec *fs, const char *s, size_t n)
{
    /* "device[__name]", split at the first "__", both parts trimmed
       (agent/disk.go:69-78) */
    size_t i;
    const char *d = s, *nm = NULL;
    size_t dn = n, nn = 0;
    memset(fs, 0, sizeof *fs);
    for (i = 0; i + 1 < n; i++)
        if (s[i] == '_' && s[i + 1] == '_') {
            dn = i;
            nm = s + i + 2;
            nn = n - i - 2;
            break;
        }
    bz_trim(&d, &dn);
    if (nm)
        bz_trim(&nm, &nn);
    if (dn == 0 || !bz_strlcpyn(fs->dev, sizeof fs->dev, d, dn))
        return 0;
    if (nm && !bz_strlcpyn(fs->name, sizeof fs->name, nm, nn))
        return 0;
    return 1;
}

int bz_stats_cfg_extra(struct bz_stats_cfg *c, const char *list)
{
    const char *p = list;
    c->nextra = 0;
    while (*p) {
        size_t n = strcspn(p, ",");
        const char *e = p;
        size_t en = n;
        bz_trim(&e, &en);
        if (en) {
            if (c->nextra == BZ_MAX_FS || !bz_fs_spec_parse(&c->extra[c->nextra], e, en))
                return 0;
            c->nextra++;
        }
        p += n;
        if (*p)
            p++;
    }
    return 1;
}

void bz_stats_cfg_nics(struct bz_stats_cfg *c, const char *spec)
{
    /* agent/network.go newNicConfig: a leading '-' makes it a blacklist */
    const char *p = spec;
    c->nics_set = 1;
    c->nics_black = 0;
    c->nnics = 0;
    if (*p == '-') {
        c->nics_black = 1;
        p++;
    }
    while (*p) {
        size_t n = strcspn(p, ",");
        const char *e = p;
        size_t en = n;
        bz_trim(&e, &en);
        if (en && c->nnics < BZ_MAX_NIC && bz_strlcpyn(c->nics[c->nnics], sizeof c->nics[0], e, en))
            c->nnics++;
        p += n;
        if (*p)
            p++;
    }
}

static int lc(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int bz_glob(const char *pat, const char *s)
{
    /* path.Match for '*' and '?' (no '/' in NIC names); case-insensitive
       because OpenVMS device names are case-insensitive (DECISIONS.md D11) */
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*pat == '*') {
            star = pat++;
            mark = s;
        } else if (*pat == '?' || (*pat && lc((unsigned char)*pat) == lc((unsigned char)*s))) {
            pat++;
            s++;
        } else if (star) {
            pat = star + 1;
            s = ++mark;
        } else {
            return 0;
        }
    }
    while (*pat == '*')
        pat++;
    return *pat == 0;
}

static int nic_valid(const struct bz_stats_cfg *c, const char *name)
{
    /* agent/network.go isValidNic */
    int i;
    if (c->nnics == 0)
        return c->nics_black;
    for (i = 0; i < c->nnics; i++)
        if (bz_ieq(c->nics[i], name))
            return !c->nics_black;
    for (i = 0; i < c->nnics; i++)
        if (strchr(c->nics[i], '*') && bz_glob(c->nics[i], name))
            return !c->nics_black;
    return c->nics_black;
}

static int has_prefix(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

int bz_nic_selected(const struct bz_stats_cfg *c, const char *name, uint64_t tx, uint64_t rx)
{
    /* agent/network.go skipNetworkInterface */
    if (c->nics_set) {
        if (!nic_valid(c, name))
            return 0;
        if (!c->nics_black)
            return 1;
    }
    if (has_prefix(name, "lo") || has_prefix(name, "docker") || has_prefix(name, "br-") ||
        has_prefix(name, "veth") || has_prefix(name, "bond") || has_prefix(name, "cali") ||
        rx == 0 || tx == 0)
        return 0;
    return 1;
}

/* ---- load average ---- */

void bz_load_sample(struct bz_load *l, uint64_t now_ms, double runq)
{
    /* Linux calc_load: every 5 s, avg = avg*e + n*(1-e), with e = EXP_n/FIXED_1 */
    static const double e[3] = { 1884.0 / 2048, 2014.0 / 2048, 2037.0 / 2048 };
    int i, steps = 0;
    if (!l->started) {
        l->started = 1;
        l->last_ms = now_ms;
        return;
    }
    while (now_ms - l->last_ms >= BZ_LOAD_PERIOD_MS && steps < 720) {
        for (i = 0; i < 3; i++)
            l->avg[i] = l->avg[i] * e[i] + runq * (1 - e[i]);
        l->last_ms += BZ_LOAD_PERIOD_MS;
        steps++;
    }
    if (steps == 720)
        l->last_ms = now_ms;        /* a long stall: don't replay hours of samples */
}

/* ---- intervals ---- */

void bz_stats_init(struct bz_stats *st)
{
    int i;
    memset(st, 0, sizeof *st);
    for (i = 0; i < BZ_MAX_IVAL; i++)
        bz_buf_init(&st->iv[i].cache);
    st->details_dirty = 1;          /* as after upstream's updateSystemDetails */
}

void bz_stats_free(struct bz_stats *st)
{
    int i;
    for (i = 0; i < BZ_MAX_IVAL; i++)
        bz_buf_free(&st->iv[i].cache);
}

static struct bz_ival *find(struct bz_stats *st, unsigned key)
{
    int i;
    for (i = 0; i < BZ_MAX_IVAL; i++)
        if (st->iv[i].used && st->iv[i].key == key)
            return &st->iv[i];
    return NULL;
}

struct bz_ival *bz_stats_ival(struct bz_stats *st, unsigned key)
{
    struct bz_ival *iv = find(st, key), *base;
    int i, victim = -1;
    if (iv)
        return iv;
    for (i = 0; i < BZ_MAX_IVAL; i++)
        if (!st->iv[i].used) {
            victim = i;
            break;
        }
    if (victim < 0) {
        /* full: reuse the least recently used interval other than 60000 */
        uint64_t oldest = (uint64_t)-1;
        for (i = 0; i < BZ_MAX_IVAL; i++)
            if (st->iv[i].key != BZ_DEFAULT_INTERVAL_MS && st->iv[i].cache_ms < oldest) {
                oldest = st->iv[i].cache_ms;
                victim = i;
            }
    }
    iv = &st->iv[victim];
    {
        bz_buf keep = iv->cache;
        memset(iv, 0, sizeof *iv);
        iv->cache = keep;
        bz_buf_clear(&iv->cache);
    }
    iv->used = 1;
    iv->key = key;
    /* a new interval starts from the 60000 CPU baseline (agent/cpu.go:51-54, :94-96) */
    base = find(st, BZ_DEFAULT_INTERVAL_MS);
    if (base && base != iv) {
        iv->cpu = base->cpu;
        iv->ncpu = base->ncpu;
        memcpy(iv->percpu, base->percpu, sizeof iv->percpu);
    }
    return iv;
}

void bz_stats_seed(struct bz_stats *st, const struct bz_sample *s)
{
    struct bz_ival *iv = bz_stats_ival(st, BZ_DEFAULT_INTERVAL_MS);
    iv->cpu = s->all;
    iv->ncpu = s->ncpu;
    memcpy(iv->percpu, s->cpu, sizeof iv->percpu);
}

/* agent/cpu.go calculateBusy, with busy = total - idle - iowait */
static double busy_pct(const struct bz_cpu_times *t1, const struct bz_cpu_times *t2)
{
    double a1 = (double)t1->total, a2 = (double)t2->total;
    double b1 = a1 - (double)t1->idle - (double)t1->iowait;
    double b2 = a2 - (double)t2->idle - (double)t2->iowait;
    if (a2 <= a1 || b2 <= b1)
        return 0;
    return clamp_pct((b2 - b1) / (a2 - a1) * 100);
}

static void compute_cpu(struct bz_ival *iv, const struct bz_sample *s, struct bz_stats_out *o)
{
    const struct bz_cpu_times *t1 = &iv->cpu, *t2 = &s->all;
    double d = (double)t2->total - (double)t1->total;
    int i, n;
    if (d > 0) {
        o->cpu = bz_two_decimals(busy_pct(t1, t2));
        o->cpub[0] = bz_two_decimals(clamp_pct(((double)t2->user - (double)t1->user) / d * 100));
        o->cpub[1] = bz_two_decimals(clamp_pct(((double)t2->system - (double)t1->system) / d * 100));
        o->cpub[2] = bz_two_decimals(clamp_pct(((double)t2->iowait - (double)t1->iowait) / d * 100));
        o->cpub[3] = bz_two_decimals(clamp_pct(((double)t2->steal - (double)t1->steal) / d * 100));
        o->cpub[4] = bz_two_decimals(clamp_pct(((double)t2->idle - (double)t1->idle) / d * 100));
        iv->cpu = *t2;      /* only advanced on a positive delta (agent/cpu.go:62-65, :76) */
    }
    /* per core: the shorter of the two lists; always advances (agent/cpu.go:87-112) */
    n = iv->ncpu < s->ncpu ? iv->ncpu : s->ncpu;
    for (i = 0; i < n; i++)
        o->cores[i] = (unsigned char)round(busy_pct(&iv->percpu[i], &s->cpu[i]));
    o->ncores = n;
    iv->ncpu = s->ncpu;
    memcpy(iv->percpu, s->cpu, sizeof iv->percpu);
}

static const struct bz_nic_prev *prev_nic(const struct bz_ival *iv, const char *name)
{
    int i;
    for (i = 0; i < iv->nprev; i++)
        if (strcmp(iv->prev[i].name, name) == 0)
            return &iv->prev[i];
    return NULL;
}

static uint64_t rate(uint64_t cur, uint64_t prev, uint64_t ms)
{
    /* a counter that went down is a reset: the delta is the new value */
    uint64_t d = cur >= prev ? cur - prev : cur;
    return d * 1000 / ms;
}

static void compute_net(struct bz_stats *st, struct bz_ival *iv, uint64_t now_ms,
                        const struct bz_sample *s, struct bz_stats_out *o)
{
    /* agent/network.go:144-244 */
    uint64_t ms = 0, tx = 0, rx = 0, up = 0, down = 0;
    int i, nnew = 0;
    struct bz_nic_prev next[BZ_MAX_NIC];
    if (iv->net_ms != 0)
        ms = now_ms - iv->net_ms;
    iv->net_ms = now_ms;
    for (i = 0; i < s->nnic && o->nni < BZ_MAX_NIC; i++) {
        const struct bz_nic_counters *c = &s->nic[i];
        const struct bz_nic_prev *p;
        struct bz_nic_out *n;
        if (!bz_nic_selected(&st->cfg, c->name, c->tx, c->rx))
            continue;
        tx += c->tx;
        rx += c->rx;
        n = &o->ni[o->nni++];
        memset(n, 0, sizeof *n);
        bz_strlcpy(n->name, c->name, sizeof n->name);
        p = prev_nic(iv, c->name);
        if (ms > 0 && p) {
            n->v[0] = rate(c->tx, p->tx, ms);
            n->v[1] = rate(c->rx, p->rx, ms);
        }
        n->v[2] = c->tx;
        n->v[3] = c->rx;
        bz_strlcpy(next[nnew].name, c->name, sizeof next[0].name);
        next[nnew].tx = c->tx;
        next[nnew].rx = c->rx;
        nnew++;
    }
    memcpy(iv->prev, next, sizeof next[0] * (size_t)nnew);
    iv->nprev = nnew;
    if (ms > 0) {
        /* unsigned, as upstream: totals that went down give a huge rate and
           take the reset branch below */
        up = (tx - iv->net_tx) * 1000 / ms;
        down = (rx - iv->net_rx) * 1000 / ms;
    }
    if (up > UINT64_C(10000000000) || down > UINT64_C(10000000000)) {
        iv->net_ms = 0;
        iv->net_tx = iv->net_rx = 0;
        iv->nprev = 0;
        o->bw[0] = o->bw[1] = 0;
        return;
    }
    o->bw[0] = up;
    o->bw[1] = down;
    iv->net_tx = tx;
    iv->net_rx = rx;
}

void bz_stats_compute(struct bz_stats *st, unsigned cache_ms, uint64_t now_ms,
                      const struct bz_sample *s, bz_fs_usage_fn fs, void *fs_ctx,
                      struct bz_stats_out *o)
{
    struct bz_ival *iv = bz_stats_ival(st, cache_ms);
    struct bz_fs_usage u;
    int i;
    memset(o, 0, sizeof *o);
    compute_cpu(iv, s, o);
    for (i = 0; i < 3; i++)
        o->la[i] = bz_two_decimals(st->load.avg[i]);

    /* agent/memory.go:18-55: the byte fields, and the GB fields upstream still
       sends for hubs older than 0.21.1 */
    o->mem_bytes[0] = s->mem_total;
    o->mem_bytes[1] = s->mem_used;
    o->mem_bytes[2] = s->mem_buffcache;
    o->swap_bytes[0] = s->swap_total;
    o->swap_bytes[1] = s->swap_used;
    o->swap = to_gib(s->swap_total);
    o->swap_used = to_gib(s->swap_used);
    o->mem = to_gib(s->mem_total);
    o->mem_bc = to_gib(s->mem_buffcache);
    o->mem_used = to_gib(s->mem_used);
    o->mem_pct = s->mem_total ? bz_two_decimals((double)s->mem_used / (double)s->mem_total * 100.0) : 0;

    /* agent/disk.go:644-680: root usage; a failure leaves zeros */
    memset(&u, 0, sizeof u);
    fs(fs_ctx, st->cfg.root.dev, &u);
    if (u.ok) {
        o->disk_total = to_gib(u.total);
        o->disk_used = to_gib(u.used);
        o->disk_pct = u.total ? bz_two_decimals((double)u.used / (double)u.total * 100) : 0;
    }
    bz_strlcpy(o->rdn, st->cfg.root.name, sizeof o->rdn);
    /* agent/agent.go:236-258: extra filesystems with DiskTotal > 0 only */
    for (i = 0; i < st->cfg.nextra; i++) {
        const struct bz_fs_spec *e = &st->cfg.extra[i];
        struct bz_fs_out *f;
        memset(&u, 0, sizeof u);
        fs(fs_ctx, e->dev, &u);
        if (!u.ok || to_gib(u.total) <= 0)
            continue;
        f = &o->efs[o->nefs++];
        bz_strlcpy(f->key, e->name[0] ? e->name : e->dev, sizeof f->key);
        f->total = to_gib(u.total);
        f->used = to_gib(u.used);
    }

    compute_net(st, iv, now_ms, s, o);
    o->uptime = s->uptime_s;
}

/* ---- encoding (internal/entities/system/system.go, field order as declared) ---- */

static void f64_array(bz_buf *b, const double *v, int n)
{
    int i;
    bz_cb_array(b, (size_t)n);
    for (i = 0; i < n; i++)
        bz_cb_f64(b, v[i]);
}

static void encode_stats(bz_buf *b, const struct bz_stats_out *o)
{
    size_t m = bz_cb_map_begin(b), cnt = 0;
    int i;
    bz_cb_uint(b, 0); bz_cb_f64(b, o->cpu); cnt++;
    bz_cb_uint(b, 2); bz_cb_f64(b, o->mem); cnt++;
    bz_cb_uint(b, 3); bz_cb_f64(b, o->mem_used); cnt++;
    bz_cb_uint(b, 4); bz_cb_f64(b, o->mem_pct); cnt++;
    bz_cb_uint(b, 5); bz_cb_f64(b, o->mem_bc); cnt++;
    if (o->swap != 0) { bz_cb_uint(b, 7); bz_cb_f64(b, o->swap); cnt++; }           /* omitempty */
    if (o->swap_used != 0) { bz_cb_uint(b, 8); bz_cb_f64(b, o->swap_used); cnt++; }
    bz_cb_uint(b, 9); bz_cb_f64(b, o->disk_total); cnt++;
    bz_cb_uint(b, 10); bz_cb_f64(b, o->disk_used); cnt++;
    bz_cb_uint(b, 11); bz_cb_f64(b, o->disk_pct); cnt++;
    if (o->nefs) {
        bz_cb_uint(b, 21);
        bz_cb_map(b, (size_t)o->nefs);
        for (i = 0; i < o->nefs; i++) {
            /* FsStats: 0 d, 1 du; the rest omitted (2 r and 3 w are omitzero
               since 0.21.1, and we never measure them) */
            bz_cb_text(b, o->efs[i].key);
            bz_cb_map(b, 2);
            bz_cb_uint(b, 0); bz_cb_f64(b, o->efs[i].total);
            bz_cb_uint(b, 1); bz_cb_f64(b, o->efs[i].used);
        }
        cnt++;
    }
    if (o->bw[0] || o->bw[1]) {                 /* omitzero */
        bz_cb_uint(b, 26);
        bz_cb_array(b, 2);
        bz_cb_uint(b, o->bw[0]);
        bz_cb_uint(b, o->bw[1]);
        cnt++;
    }
    bz_cb_uint(b, 28); f64_array(b, o->la, 3); cnt++;   /* never omitted */
    if (o->nni) {
        bz_cb_uint(b, 31);
        bz_cb_map(b, (size_t)o->nni);
        for (i = 0; i < o->nni; i++) {
            int k;
            bz_cb_text(b, o->ni[i].name);
            bz_cb_array(b, 4);
            for (k = 0; k < 4; k++)
                bz_cb_uint(b, o->ni[i].v[k]);
        }
        cnt++;
    }
    bz_cb_uint(b, 33); f64_array(b, o->cpub, 5); cnt++;
    if (o->ncores) {                            /* Uint8Slice: a byte string */
        bz_cb_uint(b, 34);
        bz_cb_bytes(b, o->cores, (size_t)o->ncores);
        cnt++;
    }
    if (o->mem_bytes[0] || o->mem_bytes[1] || o->mem_bytes[2] || o->mem_bytes[3]) {   /* omitzero */
        bz_cb_uint(b, 41);
        bz_cb_array(b, 4);
        for (i = 0; i < 4; i++)
            bz_cb_uint(b, o->mem_bytes[i]);
        cnt++;
    }
    if (o->swap_bytes[0] || o->swap_bytes[1]) {
        bz_cb_uint(b, 42);
        bz_cb_array(b, 2);
        bz_cb_uint(b, o->swap_bytes[0]);
        bz_cb_uint(b, o->swap_bytes[1]);
        cnt++;
    }
    bz_cb_map_end(b, m, cnt);
}

static void encode_info(bz_buf *b, const struct bz_stats_out *o, const char *version)
{
    size_t m = bz_cb_map_begin(b), cnt = 0;
    int i;
    if (o->threads) { bz_cb_uint(b, 3); bz_cb_uint(b, (uint64_t)o->threads); cnt++; }
    bz_cb_uint(b, 5); bz_cb_uint(b, o->uptime); cnt++;
    bz_cb_uint(b, 6); bz_cb_f64(b, o->cpu); cnt++;
    bz_cb_uint(b, 7); bz_cb_f64(b, o->mem_pct); cnt++;
    bz_cb_uint(b, 8); bz_cb_f64(b, o->disk_pct); cnt++;
    bz_cb_uint(b, 9); bz_cb_f64(b, 0); cnt++;           /* deprecated Bandwidth, never omitted */
    bz_cb_uint(b, 10); bz_cb_text(b, version); cnt++;
    bz_cb_uint(b, 18); bz_cb_uint(b, o->bw[0] + o->bw[1]); cnt++;
    bz_cb_uint(b, 19); f64_array(b, o->la, 3); cnt++;
    bz_cb_uint(b, 20); bz_cb_uint(b, 2); cnt++;         /* ConnectionTypeWebSocket */
    if (o->nefs) {
        bz_cb_uint(b, 21);
        bz_cb_map(b, (size_t)o->nefs);
        for (i = 0; i < o->nefs; i++) {
            bz_cb_text(b, o->efs[i].key);
            bz_cb_f64(b, bz_two_decimals(o->efs[i].used / o->efs[i].total * 100));
        }
        cnt++;
    }
    if (o->rdn[0]) { bz_cb_uint(b, 24); bz_cb_text(b, o->rdn); cnt++; }
    bz_cb_map_end(b, m, cnt);
}

static void encode_details(bz_buf *b, const struct bz_identity *d)
{
    size_t m = bz_cb_map_begin(b), cnt = 0;
    bz_cb_uint(b, 0); bz_cb_text(b, d->hostname); cnt++;
    if (d->kernel[0]) { bz_cb_uint(b, 1); bz_cb_text(b, d->kernel); cnt++; }
    bz_cb_uint(b, 2); bz_cb_uint(b, (uint64_t)d->cores); cnt++;
    bz_cb_uint(b, 3); bz_cb_uint(b, (uint64_t)d->threads); cnt++;
    bz_cb_uint(b, 4); bz_cb_text(b, d->cpu_model); cnt++;
    bz_cb_uint(b, 5); bz_cb_uint(b, 0); cnt++;          /* Os: Linux (DECISIONS.md D7) */
    bz_cb_uint(b, 6); bz_cb_text(b, d->os_name); cnt++;
    bz_cb_uint(b, 7); bz_cb_text(b, d->arch); cnt++;
    bz_cb_uint(b, 9); bz_cb_uint(b, d->mem_total); cnt++;
    bz_cb_map_end(b, m, cnt);
}

void bz_encode_combined(bz_buf *b, const struct bz_stats_out *o, const struct bz_identity *details,
                        const char *version)
{
    bz_cb_map(b, details ? 5 : 4);
    bz_cb_uint(b, 0);
    encode_stats(b, o);
    bz_cb_uint(b, 1);
    encode_info(b, o, version);
    bz_cb_uint(b, 2);
    bz_cb_null(b);                  /* Containers: always null */
    if (details) {
        bz_cb_uint(b, 4);
        encode_details(b, details);
    }
    bz_cb_uint(b, 6);
    bz_cb_map(b, 0);                /* Monitors: always {} */
}
