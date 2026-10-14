/* bz_main.c - the beszel agent for OpenVMS: settings, fingerprint, main loop.
 *
 *   beszel_agent [<config-file>]
 *
 * Portable over bz_platform.h.  Settings follow upstream 0.21.0's names and
 * rules (PROTOCOL.md 7, DECISIONS.md D5).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_agent.h"
#include "bz_conf.h"
#include "bz_crypto.h"
#include "bz_key.h"
#include "bz_platform.h"
#include "bz_text.h"
#include "bz_url.h"
#include "bz_version.h"

static int min_level = BZ_LOG_INFO;

static void log_cb(void *ctx, int level, const char *msg)
{
    (void)ctx;
    if (level >= min_level)
        bz_plat_log(level, msg);
}

static void say(int level, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_cb(NULL, level, msg);
}

static int sample_cb(void *ctx, struct bz_sample *s)
{
    (void)ctx;
    return bz_plat_sample(s);
}

static void fs_cb(void *ctx, const char *dev, struct bz_fs_usage *u)
{
    (void)ctx;
    bz_plat_fs_usage(dev, u);
}

static double runq_cb(void *ctx)
{
    (void)ctx;
    return bz_plat_runq();
}

/* agent/fingerprint.go: SHA-256 of the seed, first 24 bytes in hex */
static void make_fingerprint(char out[49], const char *seed)
{
    unsigned char sum[32];
    bz_sha256(seed, strlen(seed), sum);
    bz_hex(out, sum, 24);
}

static int load_fingerprint(char out[49], const char *dir, const struct bz_identity *id)
{
    char path[1024];
    bz_buf b;
    const char *s;
    size_t n;
    bz_plat_data_path(path, sizeof path, dir, "fingerprint");
    bz_buf_init(&b);
    if (bz_plat_read_file(path, &b)) {
        s = (const char *)b.p;
        n = b.len;
        bz_trim(&s, &n);
        if (n > 0 && n < 49) {
            bz_strlcpyn(out, 49, s, n);
            bz_buf_free(&b);
            return 1;
        }
    }
    bz_buf_free(&b);
    make_fingerprint(out, id->fp_seed);
    if (!bz_plat_write_file(path, out, strlen(out)))
        say(BZ_LOG_WARN, "Could not save the fingerprint to %s; it will be regenerated at the next "
                         "start, and the hub will accept it only while the seed stays the same", path);
    return 1;
}

static const char *user_agent(void)
{
    /* agent/client.go getUserAgent: the same two browser strings */
    if (bz_plat_now_ms() % 2 == 0)
        return "Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
               "Chrome/124.0.0.0 Safari/537.36";
    return "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0_0) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/124.0.0.0 Safari/537.36";
}

static const char *nonempty(const char *s)
{
    return s && *s ? s : NULL;
}

int main(int argc, char **argv)
{
    static struct bz_agent agent;           /* large: not on the stack */
    static struct bz_agent_cfg cfg;
    static struct bz_identity id;
    bz_conf conf;
    bz_buf file, keytext, token;
    struct bz_env env;
    const char *path = argc > 1 ? argv[1] : bz_plat_default_config();
    const char *v, *ca_file = NULL, *data_dir;
    char err[256], port[16];
    char *token_s;
    bz_sock *sock = NULL;
    int exit_on_dns = 0, rc = EXIT_FAILURE;
    static unsigned char rbuf[16384];

    if (!bz_plat_init())
        return EXIT_FAILURE;
    bz_conf_init(&conf, bz_plat_conf_override, NULL);
    bz_buf_init(&file);
    bz_buf_init(&keytext);
    bz_buf_init(&token);

    if (bz_plat_read_file(path, &file)) {
        if (!bz_conf_parse(&conf, (const char *)file.p, file.len, err, sizeof err)) {
            say(BZ_LOG_ERROR, "%s: %s", path, err);
            goto out;
        }
        if (bz_plat_file_exposed(path))
            say(BZ_LOG_WARN, "%s can be read by other users, and it holds the token: "
                             "restrict it to its owner", path);
    } else {
        say(BZ_LOG_WARN, "no configuration file %s; using overrides only", path);
    }

    if ((v = bz_conf_get(&conf, "LOG_LEVEL")) != NULL) {
        /* agent/agent.go:95-105: debug, warn, error; anything else is info */
        if (bz_ieq(v, "debug")) min_level = BZ_LOG_DEBUG;
        else if (bz_ieq(v, "warn")) min_level = BZ_LOG_WARN;
        else if (bz_ieq(v, "error")) min_level = BZ_LOG_ERROR;
    }
    say(BZ_LOG_INFO, "beszel agent for OpenVMS %s (protocol %s)", BZ_PORT_VERSION, BZ_PROTOCOL_VERSION);

    memset(&cfg, 0, sizeof cfg);
    v = nonempty(bz_conf_get(&conf, "HUB_URL"));
    if (!v) {
        say(BZ_LOG_ERROR, "HUB_URL is not set (only the WebSocket connection is supported)");
        goto out;
    }
    if (!bz_url_parse(&cfg.url, v, err, sizeof err)) {
        say(BZ_LOG_ERROR, "invalid HUB_URL %s: %s", v, err);
        goto out;
    }

    /* agent/client.go getToken: TOKEN, else the single line of TOKEN_FILE */
    if ((v = nonempty(bz_conf_get(&conf, "TOKEN"))) != NULL) {
        bz_buf_puts(&token, v);
    } else if ((v = nonempty(bz_conf_get(&conf, "TOKEN_FILE"))) != NULL) {
        bz_buf b;
        bz_buf_init(&b);
        if (!bz_plat_read_file(v, &b) ||
            !bz_conf_token_file((const char *)b.p, b.len, &token)) {
            say(BZ_LOG_ERROR, "TOKEN_FILE %s: unreadable, or more than one token", v);
            bz_buf_free(&b);
            goto out;
        }
        bz_buf_free(&b);
    }
    if (!token.len) {
        say(BZ_LOG_ERROR, "must set TOKEN or TOKEN_FILE");
        goto out;
    }
    token_s = (char *)token.p;

    /* internal/cmd/agent/agent.go:121-143: KEY if non-empty, else KEY_FILE */
    if ((v = nonempty(bz_conf_get(&conf, "KEY"))) != NULL) {
        bz_buf_puts(&keytext, v);
    } else if ((v = nonempty(bz_conf_get(&conf, "KEY_FILE"))) != NULL) {
        if (!bz_plat_read_file(v, &keytext)) {
            say(BZ_LOG_ERROR, "KEY_FILE %s: unreadable", v);
            goto out;
        }
    }
    if (!keytext.len) {
        say(BZ_LOG_ERROR, "no key provided: set KEY or KEY_FILE to the hub's public key");
        goto out;
    }
    if (!bz_keys_parse(&cfg.keys, (const char *)keytext.p, keytext.len, err, sizeof err)) {
        say(BZ_LOG_ERROR, "KEY: %s", err);
        goto out;
    }

    /* a CA file that can't be read is fatal at start (agent/connection_manager.go:168-173) */
    if ((v = nonempty(bz_conf_get(&conf, "CA_CERT_FILE"))) != NULL) {
        bz_buf b;
        static char ca_copy[1024];
        bz_buf_init(&b);
        if (!bz_plat_read_file(v, &b) || !strstr((const char *)b.p, "-----BEGIN CERTIFICATE-----")) {
            say(BZ_LOG_ERROR, "CA_CERT_FILE %s does not contain any valid PEM certificates", v);
            bz_buf_free(&b);
            goto out;
        }
        bz_buf_free(&b);
        bz_strlcpy(ca_copy, v, sizeof ca_copy);
        ca_file = ca_copy;
    }

    /* agent/server.go:256-272: LISTEN, else PORT, else 45876; we only report it */
    v = nonempty(bz_conf_get(&conf, "LISTEN"));
    if (!v)
        v = nonempty(bz_conf_get(&conf, "PORT"));
    if (!v)
        v = "45876";
    {
        const char *colon = strrchr(v, ':');
        bz_strlcpy(port, colon ? colon + 1 : v, sizeof port);
    }

    memset(&agent, 0, sizeof agent);
    bz_plat_identity(&id);
    {
        static struct bz_stats_cfg sc;
        memset(&sc, 0, sizeof sc);
        v = nonempty(bz_conf_get(&conf, "FILESYSTEM"));
        if (!bz_fs_spec_parse(&sc.root, v ? v : bz_plat_default_root_fs(),
                              strlen(v ? v : bz_plat_default_root_fs()))) {
            say(BZ_LOG_ERROR, "invalid FILESYSTEM");
            goto out;
        }
        if ((v = nonempty(bz_conf_get(&conf, "EXTRA_FILESYSTEMS"))) != NULL &&
            !bz_stats_cfg_extra(&sc, v)) {
            say(BZ_LOG_ERROR, "invalid EXTRA_FILESYSTEMS (at most %d entries)", BZ_MAX_FS);
            goto out;
        }
        if ((v = bz_conf_get(&conf, "NICS")) != NULL)
            bz_stats_cfg_nics(&sc, v);
        v = bz_conf_get(&conf, "EXIT_ON_DNS_ERROR");
        exit_on_dns = v && bz_ieq(v, "true");

        data_dir = nonempty(bz_conf_get(&conf, "DATA_DIR"));
        {
            static char dd[512];
            bz_strlcpy(dd, data_dir ? data_dir : bz_plat_default_data_dir(), sizeof dd);
            data_dir = dd;
        }
        load_fingerprint(cfg.fingerprint, data_dir, &id);
        say(BZ_LOG_INFO, "Data directory path=%s", data_dir);

        {
            static char sysname[128];
            v = bz_conf_get(&conf, "SYSTEM_NAME");
            bz_strlcpy(sysname, v ? v : "", sizeof sysname);
            cfg.system_name = sysname;
        }
        cfg.token = token_s;
        cfg.port = port;
        cfg.version = BZ_PROTOCOL_VERSION;
        cfg.user_agent = user_agent();

        env.ctx = NULL;
        env.sample = sample_cb;
        env.fs_usage = fs_cb;
        env.runq = runq_cb;
        env.log = log_cb;
        bz_agent_init(&agent, &cfg, &id, &env, bz_plat_now_ms());
        agent.st.cfg = sc;
        agent.st.cfg.version = BZ_PROTOCOL_VERSION;
    }

    while (!bz_plat_stop_requested()) {
        uint64_t now = bz_plat_now_ms();
        int keep = bz_agent_tick(&agent, now);

        if (!sock && bz_agent_should_connect(&agent, now)) {
            say(BZ_LOG_DEBUG, "connecting to %s", cfg.url.host_header);
            sock = bz_net_open(cfg.url.host, cfg.url.port, cfg.url.tls, ca_file, 15000, err, sizeof err);
            now = bz_plat_now_ms();
            if (!sock) {
                bz_agent_on_connect_failed(&agent, now, err);
                if (exit_on_dns && strncmp(err, "lookup of", 9) == 0) {
                    bz_plat_sleep_ms(2000);
                    goto out;
                }
            } else {
                keep = bz_agent_on_connected(&agent, now);
            }
        }
        if (sock && agent.out.len) {
            if (!bz_net_send(sock, agent.out.p, agent.out.len))
                keep = 0;
            bz_buf_clear(&agent.out);
        }
        if (sock && !keep) {
            bz_net_close(sock);
            sock = NULL;
            bz_agent_on_closed(&agent, bz_plat_now_ms());
            continue;
        }
        if (!sock) {
            bz_plat_sleep_ms(500);
            continue;
        }
        switch (bz_net_wait(sock, 500)) {
        case 1:
            {
                long n = bz_net_recv(sock, rbuf, sizeof rbuf);
                if (n == -2)
                    break;
                if (n <= 0) {
                    if (n == 0)
                        say(BZ_LOG_WARN, "Connection closed by the hub");
                    else
                        say(BZ_LOG_WARN, "Connection lost");
                    keep = 0;
                } else {
                    keep = bz_agent_on_data(&agent, bz_plat_now_ms(), rbuf, (size_t)n);
                }
                if (agent.out.len) {
                    if (!bz_net_send(sock, agent.out.p, agent.out.len))
                        keep = 0;
                    bz_buf_clear(&agent.out);
                }
                if (!keep) {
                    bz_net_close(sock);
                    sock = NULL;
                    bz_agent_on_closed(&agent, bz_plat_now_ms());
                }
            }
            break;
        case -1:
            bz_net_close(sock);
            sock = NULL;
            bz_agent_on_closed(&agent, bz_plat_now_ms());
            break;
        default:
            break;
        }
    }
    say(BZ_LOG_INFO, "Stopping");
    if (sock) {
        unsigned char mask[4];
        bz_buf_clear(&agent.out);
        if (agent.state == BZ_OPEN && bz_random(mask, 4)) {
            bz_ws_close_frame(&agent.out, 1000, mask);
            bz_net_send(sock, agent.out.p, agent.out.len);
        }
        bz_net_close(sock);
    }
    bz_agent_free(&agent);
    rc = EXIT_SUCCESS;
out:
    if (token.p)
        memset(token.p, 0, token.cap);
    bz_buf_free(&token);
    bz_buf_free(&keytext);
    bz_buf_free(&file);
    bz_conf_free(&conf);
    return rc;
}
