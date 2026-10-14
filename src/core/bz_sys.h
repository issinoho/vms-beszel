/* bz_sys.h - raw system counters, as the platform layer reports them.
 *
 * These types carry no OpenVMS (or Linux) meaning: the platform maps its own
 * counters onto them (DECISIONS.md D9) and the core turns them into beszel's
 * statistics.  Cumulative counters only ever grow, except across a reset the
 * core detects.
 */
#ifndef BZ_SYS_H
#define BZ_SYS_H

#include <stdint.h>

#define BZ_MAX_CPU 64
#define BZ_MAX_NIC 16
#define BZ_MAX_FS 16

/* cumulative CPU time in ticks of any fixed unit */
struct bz_cpu_times {
    uint64_t total;     /* all time, including idle */
    uint64_t idle;
    uint64_t user;
    uint64_t system;
    uint64_t iowait;
    uint64_t steal;
};

struct bz_nic_counters {
    char name[32];
    uint64_t tx;        /* cumulative bytes sent */
    uint64_t rx;        /* cumulative bytes received */
};

struct bz_sample {
    struct bz_cpu_times all;
    int ncpu;
    struct bz_cpu_times cpu[BZ_MAX_CPU];
    uint64_t mem_total;     /* bytes */
    uint64_t mem_used;
    uint64_t mem_buffcache;
    uint64_t swap_total;
    uint64_t swap_used;
    int nnic;
    struct bz_nic_counters nic[BZ_MAX_NIC];
    uint64_t uptime_s;
};

/* what does not change while the agent runs: beszel's Details */
struct bz_identity {
    char hostname[256];
    char kernel[64];
    char os_name[96];
    char cpu_model[96];
    char arch[32];
    int cores;
    int threads;
    uint64_t mem_total;
    char fp_seed[160];      /* fingerprint seed (DECISIONS.md D9) */
};

#endif
