/* fuzz_core.c - mutation fuzzing of every parser that sees hub or config input.
 *
 *   fuzz_core [iterations] [seed]
 *
 * Built with ASan/UBSan (make fuzz).  Seeds are realistic inputs; each round
 * mutates one (bit flips, byte changes, inserts, deletes, truncation, splices)
 * and feeds it to a target.  The agent target runs whole frames through a
 * verified connection, so request handling and reply encoding are covered too.
 * Success means no sanitizer report and every parser terminating.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_agent.h"
#include "bz_cbor.h"
#include "bz_conf.h"
#include "bz_key.h"
#include "bz_proto.h"
#include "bz_text.h"
#include "bz_url.h"
#include "bz_ws.h"

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

struct seed {
    const char *hex;    /* binary seeds in hex */
    const char *text;   /* or text */
};

static const struct seed req_seeds[] = {
    { "a3000001a20019ea6001f50202", NULL },
    { "a3000001a2001903e801f40206", NULL },
    { "a200040205", NULL },
    { "a3000701a2000001a400600160026004000203", NULL },
    { "a200060209", NULL },
    { "a3000101a200584000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001f40201", NULL },
    { "a3000c016161029f01ff", NULL },
    { "bf0000ff", NULL },
};
static const struct seed ws_seeds[] = {
    { "820548656c6c6f", NULL },
    { "02034865 6c890268698002 6c6f", NULL },
    { "880503e8627965", NULL },
    { "827e00050102030405", NULL },
    { "827f00000000000000020102", NULL },
    { "8a00", NULL },
};
static const struct seed http_seeds[] = {
    { NULL, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n" },
    { NULL, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n" },
};
static const struct seed conf_seeds[] = {
    { NULL, "# c\nHUB_URL=http://h:8090\nTOKEN=abc\nKEY=\"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC+xV8Nx7iJVVAX7y8NKxoMcIIgK7V1AGO4oeBW0GLqM\"\nNICS=-EW*\n" },
    { NULL, "from=\"a b\" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC+xV8Nx7iJVVAX7y8NKxoMcIIgK7V1AGO4oeBW0GLqM c\n" },
    { NULL, "https://user@[fe80::1]:8443/x/../beszel/?a=b#f" },
    { NULL, " DKA200:__Tools, DKA300:, ,DKA0:__System" },
};

static size_t load(const struct seed *s, unsigned char *out, size_t cap)
{
    size_t n = 0;
    if (s->text) {
        n = strlen(s->text);
        if (n > cap)
            n = cap;
        memcpy(out, s->text, n);
        return n;
    }
    {
        const char *h = s->hex;
        while (*h && n < cap) {
            unsigned v;
            if (*h == ' ') {
                h++;
                continue;
            }
            if (sscanf(h, "%2x", &v) != 1)
                break;
            out[n++] = (unsigned char)v;
            h += 2;
        }
    }
    return n;
}

static size_t mutate(unsigned char *p, size_t n, size_t cap)
{
    int rounds = 1 + (int)(rnd() % 6), i;
    for (i = 0; i < rounds; i++) {
        size_t at = n ? (size_t)(rnd() % n) : 0;
        switch (rnd() % 7) {
        case 0:
            if (n) p[at] ^= (unsigned char)(1u << (rnd() % 8));
            break;
        case 1:
            if (n) p[at] = (unsigned char)rnd();
            break;
        case 2:
            if (n < cap) {
                memmove(p + at + 1, p + at, n - at);
                p[at] = (unsigned char)rnd();
                n++;
            }
            break;
        case 3:
            if (n) {
                memmove(p + at, p + at + 1, n - at - 1);
                n--;
            }
            break;
        case 4:
            n = n ? (size_t)(rnd() % (n + 1)) : 0;
            break;
        case 5:
            {   /* interesting values */
                static const unsigned char iv[] = { 0x00, 0x17, 0x18, 0x1b, 0x3f, 0x5f, 0x7f,
                                                    0x9f, 0xbf, 0xff, 0x7e, 0xfb, 0xf6 };
                if (n) p[at] = iv[rnd() % sizeof iv];
            }
            break;
        default:
            if (n > 2) {   /* duplicate a slice */
                size_t len = 1 + (size_t)(rnd() % (n / 2 + 1));
                if (at + len <= n && n + len <= cap) {
                    memmove(p + at + len, p + at, n - at);
                    n += len;
                }
            }
            break;
        }
    }
    return n;
}

/* the agent target: a connection already upgraded and verified */
static int fz_sample(void *c, struct bz_sample *s)
{
    (void)c;
    memset(s, 0, sizeof *s);
    s->ncpu = 1;
    s->all.total = rnd() % 1000;
    s->all.idle = rnd() % 1000;
    s->cpu[0] = s->all;
    s->nnic = 1;
    strcpy(s->nic[0].name, "EWA0");
    s->nic[0].tx = rnd();
    s->nic[0].rx = rnd();
    s->mem_total = rnd();
    s->mem_used = rnd();
    return 1;
}
static void fz_fs(void *c, const char *d, struct bz_fs_usage *u)
{
    (void)c;
    (void)d;
    u->ok = (int)(rnd() & 1);
    u->total = rnd();
    u->used = rnd();
}
static double fz_runq(void *c) { (void)c; return (double)(rnd() % 8); }
static void fz_log(void *c, int l, const char *m) { (void)c; (void)l; (void)m; }

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 100000, it;
    static unsigned char buf[8192];
    static struct bz_agent a;
    static struct bz_agent_cfg cfg;
    struct bz_identity id;
    struct bz_env env;
    char err[256];
    bz_buf in;
    bz_ws_rx rx;
    if (argc > 2)
        rng = strtoull(argv[2], NULL, 0) | 1;
    memset(&cfg, 0, sizeof cfg);
    bz_url_parse(&cfg.url, "http://hub:1", err, sizeof err);
    cfg.token = "t";
    cfg.version = "0.21.1";
    cfg.user_agent = "UA";
    cfg.port = "45876";
    cfg.system_name = "";
    memset(&id, 0, sizeof id);
    env.ctx = NULL;
    env.sample = fz_sample;
    env.fs_usage = fz_fs;
    env.runq = fz_runq;
    env.log = fz_log;
    bz_agent_init(&a, &cfg, &id, &env, 0);
    a.st.cfg.nextra = 1;
    strcpy(a.st.cfg.extra[0].dev, "X");
    bz_buf_init(&in);
    bz_ws_rx_init(&rx);

    for (it = 0; it < iters; it++) {
        int target = (int)(rnd() % 8);
        size_t n;
        if (target <= 2) {
            n = load(&req_seeds[rnd() % (sizeof req_seeds / sizeof req_seeds[0])], buf, sizeof buf);
            n = mutate(buf, n, sizeof buf);
            if (target == 0) {
                struct bz_request r;
                if (bz_request_parse(&r, buf, n)) {
                    const unsigned char *sig;
                    size_t sl;
                    unsigned cm;
                    int need, inc;
                    bz_fingerprint_request(&r, &sig, &sl, &need);
                    bz_data_options(&r, &cm, &inc);
                }
                {
                    bz_cbr c;
                    bz_cbr_init(&c, buf, n);
                    while (bz_cbr_skip(&c))
                        ;
                }
            } else {
                /* whole frame through a verified agent */
                unsigned char fr[8200];
                size_t fl = 0;
                a.state = BZ_OPEN;
                a.verified = target == 1;
                fr[fl++] = 0x82;
                if (n < 126) {
                    fr[fl++] = (unsigned char)n;
                } else {
                    fr[fl++] = 126;
                    fr[fl++] = (unsigned char)(n >> 8);
                    fr[fl++] = (unsigned char)n;
                }
                memcpy(fr + fl, buf, n);
                bz_agent_on_data(&a, (uint64_t)it * 37, fr, fl + n);
                bz_buf_clear(&a.out);
                bz_buf_clear(&a.in);
            }
        } else if (target == 3) {
            n = load(&ws_seeds[rnd() % (sizeof ws_seeds / sizeof ws_seeds[0])], buf, sizeof buf);
            n = mutate(buf, n, sizeof buf);
            bz_buf_clear(&in);
            bz_buf_put(&in, buf, n);
            {
                int guard = 0;
                enum bz_ws_event ev;
                while ((ev = bz_ws_parse(&rx, &in)) != BZ_WS_NEED_MORE && ev != BZ_WS_ERROR && ++guard < 10000)
                    ;
                if (guard >= 10000) {
                    printf("ws parser did not terminate\n");
                    return EXIT_FAILURE;
                }
                if (ev == BZ_WS_ERROR) {
                    bz_ws_rx_free(&rx);
                    bz_ws_rx_init(&rx);
                }
            }
        } else if (target == 4) {
            int status;
            n = load(&http_seeds[rnd() % 2], buf, sizeof buf - 1);
            n = mutate(buf, n, sizeof buf - 1);
            bz_ws_head_len(buf, n);
            bz_ws_check_response((const char *)buf, n, "dGhlIHNhbXBsZSBub25jZQ==", &status, err, sizeof err);
        } else {
            n = load(&conf_seeds[rnd() % (sizeof conf_seeds / sizeof conf_seeds[0])], buf, sizeof buf - 1);
            n = mutate(buf, n, sizeof buf - 1);
            buf[n] = 0;
            if (target == 5) {
                bz_conf c;
                bz_buf t;
                bz_conf_init(&c, NULL, NULL);
                bz_conf_parse(&c, (const char *)buf, n, err, sizeof err);
                bz_conf_free(&c);
                bz_buf_init(&t);
                bz_conf_token_file((const char *)buf, n, &t);
                bz_buf_free(&t);
            } else if (target == 6) {
                bz_keys k;
                bz_keys_parse(&k, (const char *)buf, n, err, sizeof err);
            } else {
                bz_url u;
                struct bz_stats_cfg sc;
                char clean[8200];
                memset(&sc, 0, sizeof sc);
                bz_url_parse(&u, (const char *)buf, err, sizeof err);
                if (strlen((const char *)buf) < 8000)
                    bz_path_clean(clean, (const char *)buf);
                bz_stats_cfg_extra(&sc, (const char *)buf);
                bz_stats_cfg_nics(&sc, (const char *)buf);
                bz_glob((const char *)buf, "EWA0");
            }
        }
    }
    bz_ws_rx_free(&rx);
    bz_buf_free(&in);
    bz_agent_free(&a);
    printf("%ld iterations, no faults\n", iters);
    return EXIT_SUCCESS;
}
