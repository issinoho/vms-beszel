/* bz_agent.c - one connection to the hub (see bz_agent.h). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bz_agent.h"
#include "bz_cbor.h"
#include "bz_crypto.h"
#include "bz_proto.h"

static void agent_log(struct bz_agent *a, int level, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    a->env.log(a->env.ctx, level, msg);
}

int bz_agent_init(struct bz_agent *a, const struct bz_agent_cfg *cfg, const struct bz_identity *id,
                  const struct bz_env *env, uint64_t now)
{
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    a->id = *id;
    a->env = *env;
    a->state = BZ_DOWN;
    a->next_attempt = now;
    a->next_load = now;
    bz_buf_init(&a->in);
    bz_buf_init(&a->out);
    bz_buf_init(&a->reply);
    bz_buf_init(&a->scratch);
    bz_ws_rx_init(&a->rx);
    bz_stats_init(&a->st);
    /* the 60000 CPU baseline at start, as upstream's init() */
    if (a->env.sample(a->env.ctx, &a->sample))
        bz_stats_seed(&a->st, &a->sample);
    return 1;
}

void bz_agent_free(struct bz_agent *a)
{
    bz_buf_free(&a->in);
    bz_buf_free(&a->out);
    bz_buf_free(&a->reply);
    bz_buf_free(&a->scratch);
    bz_ws_rx_free(&a->rx);
    bz_stats_free(&a->st);
}

int bz_agent_should_connect(const struct bz_agent *a, uint64_t now)
{
    return a->state == BZ_DOWN && now >= a->next_attempt;
}

void bz_agent_on_connect_failed(struct bz_agent *a, uint64_t now, const char *why)
{
    a->last_attempt = now;
    a->next_attempt = now + BZ_RETRY_MS;
    agent_log(a, BZ_LOG_WARN, "WebSocket connection failed: %s", why);
}

int bz_agent_on_connected(struct bz_agent *a, uint64_t now)
{
    unsigned char rnd[16];
    a->last_attempt = now;
    a->opened = now;
    a->last_rx = now;
    a->verified = 0;
    a->bad_messages = 0;
    bz_buf_clear(&a->in);
    bz_buf_clear(&a->out);
    bz_ws_rx_free(&a->rx);
    bz_ws_rx_init(&a->rx);
    if (!bz_random(rnd, sizeof rnd)) {
        agent_log(a, BZ_LOG_ERROR, "no random bytes for the WebSocket key");
        return 0;
    }
    bz_ws_make_key(a->ws_key, rnd);
    bz_ws_request(&a->out, a->cfg.url.host_header, a->cfg.url.target, a->ws_key, a->cfg.token,
                  a->cfg.version, a->cfg.user_agent);
    a->state = BZ_UPGRADING;
    return !a->out.oom;
}

void bz_agent_on_closed(struct bz_agent *a, uint64_t now)
{
    if (a->state == BZ_OPEN && a->verified) {
        /* after a drop from connected: retry at once, but not within 5 s of
           the last attempt (agent/connection_manager.go:335-341) */
        a->next_attempt = a->last_attempt + BZ_MIN_ATTEMPT_MS;
        agent_log(a, BZ_LOG_WARN, "Disconnected from hub");
    } else {
        a->next_attempt = a->last_attempt + BZ_RETRY_MS;
    }
    if (a->next_attempt < now)
        a->next_attempt = now;
    a->state = BZ_DOWN;
    a->verified = 0;
    bz_buf_clear(&a->in);
    bz_buf_clear(&a->out);
}

/* queue one masked frame; 0 if no random mask */
static int send_frame(struct bz_agent *a, int op, const void *p, size_t n)
{
    unsigned char mask[4];
    if (!bz_random(mask, 4)) {
        agent_log(a, BZ_LOG_ERROR, "no random bytes for a frame mask");
        return 0;
    }
    bz_ws_frame(&a->out, op, p, n, mask);
    return !a->out.oom;
}

static int send_close(struct bz_agent *a, unsigned code)
{
    unsigned char mask[4];
    if (!bz_random(mask, 4))
        return 0;
    bz_ws_close_frame(&a->out, code, mask);
    return 1;
}

static void fs_usage_cb(void *ctx, const char *dev, struct bz_fs_usage *u)
{
    struct bz_agent *a = ctx;
    a->env.fs_usage(a->env.ctx, dev, u);
}

static void get_data(struct bz_agent *a, uint64_t now, const struct bz_request *r)
{
    unsigned cache_ms;
    int include;
    struct bz_ival *iv;
    const bz_buf *payload;
    bz_data_options(r, &cache_ms, &include);
    iv = bz_stats_ival(&a->st, cache_ms);
    if (iv->cache_valid && now - iv->cache_ms < (uint64_t)(cache_ms / 2)) {
        /* agent/agent_cache.go: reused while younger than half the interval;
           a cached reply never carries Details */
        agent_log(a, BZ_LOG_DEBUG, "Cached data cacheTimeMs=%u", cache_ms);
        payload = &iv->cache;
    } else {
        memset(&a->sample, 0, sizeof a->sample);
        if (!a->env.sample(a->env.ctx, &a->sample))
            agent_log(a, BZ_LOG_ERROR, "Error collecting system data");
        bz_stats_compute(&a->st, cache_ms, now, &a->sample, fs_usage_cb, a, &a->so);
        a->so.threads = a->id.threads;
        bz_buf_clear(&iv->cache);
        bz_encode_combined(&iv->cache, &a->so, NULL, a->cfg.version);
        iv->cache_ms = now;
        iv->cache_valid = !iv->cache.oom;
        payload = &iv->cache;
        /* agent/system.go attachSystemDetails */
        if (cache_ms == BZ_DEFAULT_INTERVAL_MS && (include || a->st.details_dirty)) {
            bz_buf_clear(&a->scratch);
            bz_encode_combined(&a->scratch, &a->so, &a->id, a->cfg.version);
            a->st.details_dirty = 0;
            payload = &a->scratch;
        }
    }
    bz_reply_begin(&a->reply, r, 1);
    if (r->has_id)
        bz_cb_uint(&a->reply, 1);   /* legacy SystemData field (PROTOCOL.md 3.3) */
    bz_buf_put(&a->reply, payload->p, payload->len);
}

static int check_fingerprint(struct bz_agent *a, const struct bz_request *r)
{
    const unsigned char *sig;
    size_t sig_len;
    int need, i, ok = 0;
    if (!bz_fingerprint_request(r, &sig, &sig_len, &need))
        return 0;
    if (sig_len == 64)
        for (i = 0; i < a->cfg.keys.n && !ok; i++)
            ok = bz_ed25519_verify(a->cfg.keys.ed25519[i], a->cfg.token, strlen(a->cfg.token), sig);
    if (!ok)
        return 0;       /* as upstream: no reply; the hub closes after 5 s */
    a->verified = 1;
    agent_log(a, BZ_LOG_INFO, "WebSocket connected host=%s", a->cfg.url.host_header);
    bz_reply_fingerprint(&a->reply, r, a->cfg.fingerprint, need ? a->id.hostname : NULL,
                         need ? a->cfg.port : NULL, need ? a->cfg.system_name : NULL);
    return 1;
}

/* a message we can't use: logged, and after a few the connection is closed,
   so a peer can't fill the disk with log lines (review M4) */
static int bad_message(struct bz_agent *a, int level, const char *what)
{
    if (++a->bad_messages > BZ_MAX_BAD_MESSAGES) {
        agent_log(a, BZ_LOG_WARN, "Closing the connection: %d messages that could not be used",
                  a->bad_messages);
        return 0;
    }
    agent_log(a, level, "%s", what);
    return 1;
}

static int handle_message(struct bz_agent *a, uint64_t now, const unsigned char *p, size_t n)
{
    struct bz_request r;
    if (!bz_request_parse(&r, p, n))
        return bad_message(a, BZ_LOG_ERROR, "Error parsing message");
    bz_buf_clear(&a->reply);
    if (r.action == BZ_ACT_CHECK_FINGERPRINT) {
        if (!check_fingerprint(a, &r))
            return bad_message(a, BZ_LOG_ERROR, "Error handling message: invalid signature - check KEY value");
    } else if (!a->verified) {
        return bad_message(a, BZ_LOG_WARN, "Error handling message: hub not verified");
    } else if (r.action == BZ_ACT_GET_DATA) {
        get_data(a, now, &r);
    } else {
        agent_log(a, BZ_LOG_DEBUG, "Request action=%lu", (unsigned long)r.action);
        bz_reply_unsupported(&a->reply, &r);
    }
    if (a->reply.oom) {
        agent_log(a, BZ_LOG_ERROR, "out of memory building a reply");
        return 0;
    }
    if (a->reply.len && !send_frame(a, BZ_WS_OP_BINARY, a->reply.p, a->reply.len))
        return 0;
    return 1;
}

int bz_agent_on_data(struct bz_agent *a, uint64_t now, const void *p, size_t n)
{
    bz_buf_put(&a->in, p, n);
    if (a->in.oom)
        return 0;
    if (a->state == BZ_UPGRADING) {
        size_t hl = bz_ws_head_len(a->in.p, a->in.len);
        char err[160];
        int status;
        if (!hl) {
            if (a->in.len > 16384) {
                agent_log(a, BZ_LOG_WARN, "WebSocket connection failed: response head too long");
                return 0;
            }
            return 1;
        }
        if (!bz_ws_check_response((const char *)a->in.p, hl, a->ws_key, &status, err, sizeof err)) {
            if (status == 401)
                agent_log(a, BZ_LOG_WARN, "WebSocket connection failed: %s (check TOKEN)", err);
            else
                agent_log(a, BZ_LOG_WARN, "WebSocket connection failed: %s", err);
            return 0;
        }
        bz_buf_consume(&a->in, hl);
        a->state = BZ_OPEN;
        a->last_rx = now;
        agent_log(a, BZ_LOG_DEBUG, "WebSocket upgraded");
    }
    for (;;) {
        enum bz_ws_event ev = bz_ws_parse(&a->rx, &a->in);
        switch (ev) {
        case BZ_WS_NEED_MORE:
            return 1;
        case BZ_WS_MESSAGE:
            a->last_rx = now;
            if (a->rx.op == BZ_WS_OP_BINARY && a->rx.msg.len &&
                !handle_message(a, now, a->rx.msg.p, a->rx.msg.len))
                return 0;
            break;              /* text and empty messages are ignored */
        case BZ_WS_PING:
            a->last_rx = now;
            if (!send_frame(a, BZ_WS_OP_PONG, a->rx.msg.p, a->rx.msg.len))
                return 0;
            break;
        case BZ_WS_PONG:
            a->last_rx = now;
            break;
        case BZ_WS_CLOSE:
            agent_log(a, BZ_LOG_WARN, "Connection closed code=%u reason=\"%.*s\"", a->rx.close_code,
                 (int)(a->rx.msg.len > 200 ? 200 : a->rx.msg.len),
                 a->rx.msg.len ? (const char *)a->rx.msg.p : "");
            send_close(a, 1000);
            return 0;
        case BZ_WS_ERROR:
            agent_log(a, BZ_LOG_WARN, "WebSocket protocol error: %s", a->rx.error);
            send_close(a, a->rx.close_code);
            return 0;
        }
    }
}

int bz_agent_tick(struct bz_agent *a, uint64_t now)
{
    if (now >= a->next_load) {
        bz_load_sample(&a->st.load, now, a->env.runq(a->env.ctx));
        a->next_load = now + 1000;  /* bz_load_sample applies one step per 5 s */
    }
    if (a->state == BZ_UPGRADING && now - a->opened > BZ_UPGRADE_TIMEOUT_MS) {
        agent_log(a, BZ_LOG_WARN, "WebSocket connection failed: no upgrade response");
        return 0;
    }
    if (a->state == BZ_OPEN && !a->verified && now - a->opened > BZ_VERIFY_TIMEOUT_MS) {
        agent_log(a, BZ_LOG_WARN, "Connection closed: the hub did not prove its key within %d s",
                  BZ_VERIFY_TIMEOUT_MS / 1000);
        send_close(a, 1000);
        return 0;
    }
    if (a->state == BZ_OPEN && now - a->last_rx > BZ_RX_DEADLINE_MS) {
        agent_log(a, BZ_LOG_WARN, "Connection closed: nothing from the hub for %d s",
             BZ_RX_DEADLINE_MS / 1000);
        send_close(a, 1000);
        return 0;
    }
    return 1;
}
