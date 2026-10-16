/* bz_main.c - the beszel agent for OpenVMS: settings, fingerprint, main loop.
 *
 *   beszel_agent [<config-file>]
 *   beszel_agent --sample [secs] [disk...]    print what would be reported, no hub
 *   beszel_agent --version                    print the version (MAKE_KIT checks it)
 *   beszel_agent --random                     print 32 random characters (a password
 *                                             for the agent's account, BESZEL$CONFIGURE)
 *
 * Stops on SIGINT/SIGTERM, or when a file named "stop" appears in DATA_DIR
 * (BESZEL$ROOT:[DATA]STOP. on OpenVMS): BESZEL$STARTUP.COM STOP creates it, and
 * BESZEL$RUN.COM then doesn't restart the agent.  A stop file left from before
 * is removed at start.
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

/* "don't restart me": BESZEL$RUN.COM stops instead of retrying (SS$_ABORT on VMS) */
#define BZ_EXIT_REFUSED 44

static int min_level = BZ_LOG_INFO;

/* Identical consecutive lines are counted, not repeated (review M4): a hub
   that keeps failing the same way writes a summary every 10 minutes. */
static char last_msg[512];
static int last_level;
static unsigned long repeats;
static uint64_t repeat_since;

static void flush_repeats(void)
{
    char note[96];
    if (repeats) {
        snprintf(note, sizeof note, "(the previous message was repeated %lu more times)", repeats);
        bz_plat_log(last_level, note);
        repeats = 0;
    }
}

static void log_cb(void *ctx, int level, const char *msg)
{
    uint64_t now;
    (void)ctx;
    if (level < min_level)
        return;
    now = bz_plat_now_ms();
    if (level == last_level && strcmp(msg, last_msg) == 0) {
        if (!repeats)
            repeat_since = now;
        repeats++;
        if (now - repeat_since >= 600000)
            flush_repeats();
        return;
    }
    flush_repeats();
    bz_strlcpy(last_msg, msg, sizeof last_msg);
    last_level = level;
    bz_plat_log(level, msg);
}

/* control characters would let a value forge log lines or HTTP headers (review I4) */
static int has_ctl(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7F)
            return 1;
    return 0;
}

static void wipe(bz_buf *b)
{
    if (b->p)
        memset(b->p, 0, b->cap);
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
    if (bz_plat_exists(path)) {
        /* the fingerprint ties this agent to its system in the hub: never
           replace a file that is there but unreadable or damaged (review L5) */
        size_t i;
        if (!bz_plat_read_file(path, &b)) {
            say(BZ_LOG_ERROR, "the fingerprint file %s can't be read", path);
            bz_buf_free(&b);
            return 0;
        }
        s = (const char *)b.p;
        n = b.len;
        bz_trim(&s, &n);
        for (i = 0; i < n; i++)
            if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
                break;
        if (n == 0 || n > 48 || i != n) {
            say(BZ_LOG_ERROR, "the fingerprint file %s is damaged; restore it, or delete it and "
                              "reset the system's fingerprint in the hub", path);
            bz_buf_free(&b);
            return 0;
        }
        bz_strlcpyn(out, 49, s, n);
        bz_buf_free(&b);
        return 1;
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

static void fs_print(const char *dev)
{
    struct bz_fs_usage u;
    bz_plat_fs_usage(dev, &u);
    if (u.ok)
        printf("  disk %-20s total %.2f GiB  used %.2f GiB (%.2f%%)\n", dev,
               (double)u.total / 1073741824.0, (double)u.used / 1073741824.0,
               u.total ? (double)u.used / (double)u.total * 100 : 0.0);
    else
        printf("  disk %-20s not mounted or not found\n", dev);
}

/* --sample [secs] [disk...]: what the agent would report, without a hub */
static int sample_mode(int argc, char **argv)
{
    static struct bz_stats st;
    static struct bz_sample s;
    static struct bz_stats_out o;
    struct bz_identity id;
    int secs = argc > 2 ? atoi(argv[2]) : 5, i;
    uint64_t t0;
    if (secs < 1)
        secs = 1;
    if (!bz_plat_init())
        return EXIT_FAILURE;
    bz_plat_identity(&id);
    printf("hostname   %s\nos         %s\nkernel     %s\ncpu model  %s\narch       %s\n"
           "cpus       %d\nmemory     %llu bytes\nfp seed    %s\n",
           id.hostname, id.os_name, id.kernel, id.cpu_model, id.arch, id.cores,
           (unsigned long long)id.mem_total, id.fp_seed);
    bz_stats_init(&st);
    bz_fs_spec_parse(&st.cfg.root, bz_plat_default_root_fs(), strlen(bz_plat_default_root_fs()));
    if (!bz_plat_sample(&s))
        printf("first sample failed\n");
    bz_stats_seed(&st, &s);
    t0 = bz_plat_now_ms();
    bz_stats_compute(&st, 60000, t0, &s, fs_cb, NULL, &o);     /* network baseline */
    printf("sampling for %d s...\n", secs);
    bz_plat_sleep_ms((unsigned)secs * 1000);
    if (!bz_plat_sample(&s))
        printf("second sample failed\n");
    bz_stats_compute(&st, 60000, bz_plat_now_ms(), &s, fs_cb, NULL, &o);
    printf("cpu        %.2f%%  user %.2f  system %.2f  iowait %.2f  steal %.2f  idle %.2f\n",
           o.cpu, o.cpub[0], o.cpub[1], o.cpub[2], o.cpub[3], o.cpub[4]);
    printf("per cpu   ");
    for (i = 0; i < o.ncores; i++)
        printf(" %u%%", o.cores[i]);
    printf("\nrun queue  %.0f (CUR+COM+COMO now)\n", bz_plat_runq());
    printf("memory     %.2f GiB, used %.2f GiB (%.2f%%), buff/cache %.2f GiB\n",
           o.mem, o.mem_used, o.mem_pct, o.mem_bc);
    printf("swap       %.2f GiB, used %.2f GiB\n", o.swap, o.swap_used);
    printf("uptime     %llu s\n", (unsigned long long)o.uptime);
    for (i = 0; i < o.nni; i++)
        printf("  nic %-8s up %llu B/s  down %llu B/s  total sent %llu  received %llu\n", o.ni[i].name,
               (unsigned long long)o.ni[i].v[0], (unsigned long long)o.ni[i].v[1],
               (unsigned long long)o.ni[i].v[2], (unsigned long long)o.ni[i].v[3]);
    for (i = 0; i < s.nnic; i++)
        if (!bz_nic_selected(&st.cfg, s.nic[i].name, s.nic[i].tx, s.nic[i].rx))
            printf("  nic %-8s skipped (no traffic, or NICS)\n", s.nic[i].name);
    fs_print(bz_plat_default_root_fs());
    for (i = 3; i < argc; i++)
        fs_print(argv[i]);
    bz_stats_free(&st);
    return EXIT_SUCCESS;
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
    static char stop_file[1100];
    uint64_t next_stop_check = 0;
    char *token_s;
    bz_sock *sock = NULL;
    int exit_on_dns = 0, rc = EXIT_FAILURE;
    static unsigned char rbuf[16384];

    if (argc > 1 && strcmp(argv[1], "--sample") == 0)
        return sample_mode(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("beszel_agent %s\n", BZ_PORT_VERSION);
        return EXIT_SUCCESS;
    }
    if (argc > 1 && strcmp(argv[1], "--random") == 0) {
        /* letters and digits only: a VMS password, never typed by anyone */
        static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        unsigned char r[32];
        char pw[33];
        int i;
        if (!bz_random(r, sizeof r))
            return EXIT_FAILURE;
        for (i = 0; i < 32; i++)
            pw[i] = set[r[i] % 36];         /* 256 % 36 = 4: a tiny bias, harmless here */
        pw[32] = 0;
        printf("%s\n", pw);
        memset(r, 0, sizeof r);
        return EXIT_SUCCESS;
    }
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
    if (!bz_plat_check_account(err, sizeof err)) {
        say(BZ_LOG_ERROR, "%s", err);         /* D12: no privileges, no system account */
        rc = BZ_EXIT_REFUSED;
        goto out;
    }

    memset(&cfg, 0, sizeof cfg);
    v = nonempty(bz_conf_get(&conf, "HUB_URL"));
    if (!v) {
        say(BZ_LOG_ERROR, "HUB_URL is not set (only the WebSocket connection is supported)");
        goto out;
    }
    if (has_ctl(v) || !bz_url_parse(&cfg.url, v, err, sizeof err)) {
        /* not echoed: a URL can hold user:password@ (review I3) */
        say(BZ_LOG_ERROR, "invalid HUB_URL: %s", has_ctl(v) ? "it contains control characters" : err);
        goto out;
    }
    if (!cfg.url.tls && strcmp(cfg.url.host, "127.0.0.1") != 0 && strcmp(cfg.url.host, "::1") != 0 &&
        !bz_ieq(cfg.url.host, "localhost"))
        say(BZ_LOG_WARN, "HUB_URL is http://: the token crosses the network in clear text, and "
                         "anyone on the path could use it to pose as this system; use https:// (D13)");

    /* agent/client.go getToken: TOKEN, else the single line of TOKEN_FILE */
    if ((v = nonempty(bz_conf_get(&conf, "TOKEN"))) != NULL) {
        bz_buf_puts(&token, v);
    } else if ((v = nonempty(bz_conf_get(&conf, "TOKEN_FILE"))) != NULL) {
        bz_buf b;
        bz_buf_init(&b);
        if (!bz_plat_read_file(v, &b) ||
            !bz_conf_token_file((const char *)b.p, b.len, &token)) {
            say(BZ_LOG_ERROR, "TOKEN_FILE %s: unreadable, or more than one token", v);
            wipe(&b);
            bz_buf_free(&b);
            goto out;
        }
        wipe(&b);
        bz_buf_free(&b);
    }
    if (!token.len) {
        say(BZ_LOG_ERROR, "must set TOKEN or TOKEN_FILE");
        goto out;
    }
    if (has_ctl((const char *)token.p) || token.len > 255) {
        say(BZ_LOG_ERROR, "TOKEN must be one line of printable characters");
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
        if (!bz_plat_read_file(v, &b) || !b.len || !strstr((const char *)b.p, "-----BEGIN CERTIFICATE-----")) {
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
        if (!load_fingerprint(cfg.fingerprint, data_dir, &id))
            goto out;
        say(BZ_LOG_INFO, "Data directory path=%s", data_dir);
        bz_plat_data_path(stop_file, sizeof stop_file, data_dir, "stop");
        if (bz_plat_exists(stop_file)) {
            say(BZ_LOG_INFO, "removing an old stop request %s", stop_file);
            bz_plat_remove(stop_file);
        }

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
        int keep;
        if (now >= next_stop_check) {
            next_stop_check = now + 2000;
            if (bz_plat_exists(stop_file)) {
                say(BZ_LOG_INFO, "stop requested (%s)", stop_file);
                break;
            }
        }
        keep = bz_agent_tick(&agent, now);

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
    flush_repeats();
    bz_buf_free(&keytext);
    wipe(&file);
    bz_buf_free(&file);
    bz_conf_free(&conf);
    return rc;
}
