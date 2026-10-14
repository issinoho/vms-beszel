/* bz_agent.h - one connection to the hub, as a state machine (PROTOCOL.md 1, 2, 6).
 *
 * The caller owns the socket and the clock.  It asks bz_agent_should_connect,
 * opens the connection, reports bytes with bz_agent_on_data and sends whatever
 * the agent leaves in `out`.  A return of 0 from on_data or tick means "send
 * `out`, then close the socket and call bz_agent_on_closed".
 *
 * Pacing follows upstream's connection manager: an attempt at start, then
 * every 10 s while down, at most one attempt per 5 s, and an immediate retry
 * (subject to the 5 s rule) after a verified connection drops.  The hub's
 * requests are answered only after its signature over our token verified.
 */
#ifndef BZ_AGENT_H
#define BZ_AGENT_H

#include <stdint.h>

#include "bz_buf.h"
#include "bz_key.h"
#include "bz_stats.h"
#include "bz_sys.h"
#include "bz_url.h"
#include "bz_ws.h"

#define BZ_LOG_DEBUG 0
#define BZ_LOG_INFO 1
#define BZ_LOG_WARN 2
#define BZ_LOG_ERROR 3

#define BZ_RETRY_MS 10000           /* agent/connection_manager.go:58 */
#define BZ_MIN_ATTEMPT_MS 5000      /* :365-367 */
#define BZ_RX_DEADLINE_MS 120000    /* agent/client.go:29-33 */
#define BZ_UPGRADE_TIMEOUT_MS 30000

struct bz_env {
    void *ctx;
    int (*sample)(void *ctx, struct bz_sample *s);
    void (*fs_usage)(void *ctx, const char *dev, struct bz_fs_usage *u);
    double (*runq)(void *ctx);
    void (*log)(void *ctx, int level, const char *msg);
};

struct bz_agent_cfg {
    bz_url url;
    const char *token;
    bz_keys keys;
    char fingerprint[49];
    const char *system_name;
    const char *port;               /* reported for universal tokens */
    const char *version;
    const char *user_agent;
};

enum bz_conn_state { BZ_DOWN, BZ_UPGRADING, BZ_OPEN };

struct bz_agent {
    struct bz_agent_cfg cfg;
    struct bz_identity id;
    struct bz_stats st;
    struct bz_env env;
    enum bz_conn_state state;
    int verified;
    uint64_t next_attempt, last_attempt, last_rx, opened, next_load;
    char ws_key[25];
    bz_buf in, out, reply, scratch;
    bz_ws_rx rx;
    struct bz_sample sample;        /* large: kept here, not on the stack */
    struct bz_stats_out so;
};

int bz_agent_init(struct bz_agent *a, const struct bz_agent_cfg *cfg, const struct bz_identity *id,
                  const struct bz_env *env, uint64_t now);
void bz_agent_free(struct bz_agent *a);

int bz_agent_should_connect(const struct bz_agent *a, uint64_t now);
void bz_agent_on_connect_failed(struct bz_agent *a, uint64_t now, const char *why);
int bz_agent_on_connected(struct bz_agent *a, uint64_t now);
int bz_agent_on_data(struct bz_agent *a, uint64_t now, const void *p, size_t n);
void bz_agent_on_closed(struct bz_agent *a, uint64_t now);
int bz_agent_tick(struct bz_agent *a, uint64_t now);

#endif
