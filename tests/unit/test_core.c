/* test_core.c - unit tests for src/core (Linux; the VMS build runs them too).
 *
 * Known answers come from the RFCs (CBOR: RFC 8949 appendix A; WebSocket:
 * RFC 6455 section 1.3) and from the Phase 0 capture of upstream's agent
 * (PROTOCOL.md section 9).  Signing for the handshake tests uses OpenSSL
 * directly; the agent itself only verifies.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include "bz_agent.h"
#include "bz_buf.h"
#include "bz_cbor.h"
#include "bz_conf.h"
#include "bz_crypto.h"
#include "bz_key.h"
#include "bz_proto.h"
#include "bz_stats.h"
#include "bz_text.h"
#include "bz_url.h"
#include "bz_ws.h"

static int fails, checks;

#define CHECK(c) check_((c), #c, __FILE__, __LINE__)
static void check_(int ok, const char *what, const char *file, int line)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s:%d: %s\n", file, line, what);
    }
}

static int hexeq(const bz_buf *b, const char *hex)
{
    char tmp[4096];
    if (b->len * 2 + 1 > sizeof tmp)
        return 0;
    bz_hex(tmp, b->p, b->len);
    if (strcmp(tmp, hex) != 0) {
        printf("    got  %s\n    want %s\n", tmp, hex);
        return 0;
    }
    return 1;
}

static size_t unhex(unsigned char *out, const char *h)
{
    size_t n = 0;
    while (h[0] && h[1]) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (unsigned char)v;
        h += 2;
    }
    return n;
}

/* ---- buffer, text ---- */

static void test_buf_text(void)
{
    bz_buf b;
    const char *s;
    size_t n;
    char hx[9];
    bz_buf_init(&b);
    bz_buf_printf(&b, "%s-%d", "abc", 42);
    CHECK(b.len == 6 && strcmp((char *)b.p, "abc-42") == 0);
    bz_buf_consume(&b, 4);
    CHECK(b.len == 2 && strcmp((char *)b.p, "42") == 0);
    bz_buf_clear(&b);
    {   /* printf longer than the small stack buffer */
        char big[600];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = 0;
        bz_buf_printf(&b, "<%s>", big);
        CHECK(b.len == 601 && b.p[0] == '<' && b.p[600] == '>');
    }
    bz_buf_clear(&b);
    bz_b64_encode(&b, (const unsigned char *)"foobar", 6);
    CHECK(strcmp((char *)b.p, "Zm9vYmFy") == 0);
    bz_buf_clear(&b);
    bz_b64_encode(&b, (const unsigned char *)"fo", 2);
    CHECK(strcmp((char *)b.p, "Zm8=") == 0);
    bz_buf_clear(&b);
    CHECK(bz_b64_decode(&b, "Zm9vYg==", 8) && b.len == 4 && memcmp(b.p, "foob", 4) == 0);
    bz_buf_clear(&b);
    CHECK(!bz_b64_decode(&b, "Zm9=vYg=", 8));
    CHECK(!bz_b64_decode(&b, "Zm9", 3));
    bz_hex(hx, (const unsigned char *)"\x01\xab\xff\x00", 4);
    CHECK(strcmp(hx, "01abff00") == 0);
    s = "  \tx y \r\n";
    n = strlen(s);
    bz_trim(&s, &n);
    CHECK(n == 3 && memcmp(s, "x y", 3) == 0);
    CHECK(bz_ieq("Upgrade", "upgrade") && !bz_ieq("upgrade", "upgrades"));
    bz_buf_free(&b);
}

/* ---- CBOR ---- */

static void test_cbor(void)
{
    bz_buf b;
    bz_cbr r;
    uint64_t v;
    size_t m, i;
    const unsigned char *p;
    size_t n;
    unsigned char in[64];
    bz_buf_init(&b);
    /* RFC 8949 appendix A */
    bz_cb_uint(&b, 0); CHECK(hexeq(&b, "00")); bz_buf_clear(&b);
    bz_cb_uint(&b, 23); CHECK(hexeq(&b, "17")); bz_buf_clear(&b);
    bz_cb_uint(&b, 24); CHECK(hexeq(&b, "1818")); bz_buf_clear(&b);
    bz_cb_uint(&b, 1000); CHECK(hexeq(&b, "1903e8")); bz_buf_clear(&b);
    bz_cb_uint(&b, 1000000); CHECK(hexeq(&b, "1a000f4240")); bz_buf_clear(&b);
    bz_cb_uint(&b, UINT64_C(1000000000000)); CHECK(hexeq(&b, "1b000000e8d4a51000")); bz_buf_clear(&b);
    bz_cb_uint(&b, UINT64_C(18446744073709551615)); CHECK(hexeq(&b, "1bffffffffffffffff")); bz_buf_clear(&b);
    bz_cb_int(&b, -1); CHECK(hexeq(&b, "20")); bz_buf_clear(&b);
    bz_cb_int(&b, -1000); CHECK(hexeq(&b, "3903e7")); bz_buf_clear(&b);
    bz_cb_int(&b, -38); CHECK(hexeq(&b, "3825")); bz_buf_clear(&b);
    /* floats are always 8-byte, as upstream (PROTOCOL.md 9) */
    bz_cb_f64(&b, 1.1); CHECK(hexeq(&b, "fb3ff199999999999a")); bz_buf_clear(&b);
    bz_cb_f64(&b, 0); CHECK(hexeq(&b, "fb0000000000000000")); bz_buf_clear(&b);
    bz_cb_f64(&b, 16.45); CHECK(hexeq(&b, "fb4030733333333333")); bz_buf_clear(&b);
    bz_cb_bool(&b, 0); bz_cb_bool(&b, 1); bz_cb_null(&b); CHECK(hexeq(&b, "f4f5f6")); bz_buf_clear(&b);
    bz_cb_text(&b, "IETF"); CHECK(hexeq(&b, "6449455446")); bz_buf_clear(&b);
    bz_cb_bytes(&b, "\x01\x02\x03\x04", 4); CHECK(hexeq(&b, "4401020304")); bz_buf_clear(&b);
    bz_cb_array(&b, 3); bz_cb_uint(&b, 1); bz_cb_uint(&b, 2); bz_cb_uint(&b, 3);
    CHECK(hexeq(&b, "83010203")); bz_buf_clear(&b);
    /* map fix-up: small and large counts */
    m = bz_cb_map_begin(&b);
    bz_cb_uint(&b, 1); bz_cb_uint(&b, 2); bz_cb_uint(&b, 3); bz_cb_uint(&b, 4);
    bz_cb_map_end(&b, m, 2);
    CHECK(hexeq(&b, "a201020304")); bz_buf_clear(&b);
    bz_cb_uint(&b, 7);
    m = bz_cb_map_begin(&b);
    for (i = 0; i < 25; i++) { bz_cb_uint(&b, i); bz_cb_uint(&b, 0); }
    bz_cb_map_end(&b, m, 25);
    /* 7, a 2-byte map head, 24 one-byte keys, key 24 (2 bytes), 25 values */
    CHECK(b.len == 1 + 2 + 24 + 2 + 25 && b.p[1] == 0xB8 && b.p[2] == 25 && b.p[3] == 0x00);
    bz_cbr_init(&r, b.p + 1, b.len - 1);
    CHECK(bz_cbr_map(&r, &v) && v == 25 && bz_cbr_skip(&r) == 1);
    bz_buf_clear(&b);

    /* decoder */
    /* {0: 1, 1: {2: 3}, 2: [true, null]} */
    n = unhex(in, "a3000101a102030282f5f6");
    bz_cbr_init(&r, in, n);
    CHECK(bz_cbr_map(&r, &v) && v == 3);
    CHECK(bz_cbr_uint(&r, &v) && v == 0 && bz_cbr_uint(&r, &v) && v == 1);
    CHECK(bz_cbr_uint(&r, &v) && v == 1 && bz_cbr_skip(&r));
    CHECK(bz_cbr_uint(&r, &v) && v == 2 && bz_cbr_skip(&r) && r.pos == n);
    n = unhex(in, "43010203");
    bz_cbr_init(&r, in, n);
    CHECK(bz_cbr_bytes(&r, &p, &n) && n == 3 && p[2] == 3);
    /* truncation and bad input set err */
    n = unhex(in, "1a000f42");
    bz_cbr_init(&r, in, n);
    CHECK(!bz_cbr_uint(&r, &v) && r.err);
    n = unhex(in, "5a7fffffff00");
    bz_cbr_init(&r, in, n);
    CHECK(!bz_cbr_skip(&r));
    n = unhex(in, "9bffffffffffffffff");
    bz_cbr_init(&r, in, n);
    CHECK(!bz_cbr_skip(&r));
    /* indefinite containers skip; nesting is bounded */
    n = unhex(in, "9f018202039f0405ffff");
    bz_cbr_init(&r, in, n);
    CHECK(bz_cbr_skip(&r) && r.pos == n);
    memset(in, 0x81, 40);
    in[40] = 0;
    bz_cbr_init(&r, in, 41);
    CHECK(!bz_cbr_skip(&r));
    n = unhex(in, "ff");
    bz_cbr_init(&r, in, n);
    CHECK(!bz_cbr_skip(&r));
    bz_buf_free(&b);
}

/* ---- URL ---- */

static void test_url(void)
{
    bz_url u;
    char err[128], out[128];
    CHECK(bz_url_parse(&u, "http://hub.example.com:8090", err, sizeof err));
    CHECK(!u.tls && strcmp(u.host, "hub.example.com") == 0 && strcmp(u.port, "8090") == 0);
    CHECK(strcmp(u.target, "/api/beszel/agent-connect") == 0);
    CHECK(strcmp(u.host_header, "hub.example.com:8090") == 0);
    CHECK(bz_url_parse(&u, "https://x.org/beszel/", err, sizeof err));
    CHECK(u.tls && strcmp(u.port, "443") == 0 && strcmp(u.target, "/beszel/api/beszel/agent-connect") == 0);
    CHECK(bz_url_parse(&u, "http://x//a/./b/../c?q=1#f", err, sizeof err));
    CHECK(strcmp(u.target, "/a/c/api/beszel/agent-connect?q=1") == 0);
    CHECK(bz_url_parse(&u, "http://user:pw@[::1]:9/", err, sizeof err));
    CHECK(strcmp(u.host, "::1") == 0 && strcmp(u.port, "9") == 0 && strcmp(u.host_header, "[::1]:9") == 0);
    CHECK(!bz_url_parse(&u, "wss://x", err, sizeof err));       /* D11 */
    CHECK(!bz_url_parse(&u, "http://", err, sizeof err));
    CHECK(!bz_url_parse(&u, "http://x:99999", err, sizeof err));
    CHECK(!bz_url_parse(&u, "http://x:8a", err, sizeof err));
    bz_path_clean(out, "/../a/../../b/");
    CHECK(strcmp(out, "/b") == 0);
    bz_path_clean(out, "");
    CHECK(strcmp(out, "/") == 0);
}

/* ---- configuration and keys ---- */

static const char *override_value;
static int test_override(void *ctx, const char *name, bz_buf *out)
{
    (void)ctx;
    if (override_value && strcmp(name, "NICS") == 0) {
        bz_buf_puts(out, override_value);
        return 1;
    }
    return 0;
}

static void test_conf(void)
{
    bz_conf c;
    bz_buf t;
    char err[128];
    const char *text = "# comment\n\n HUB_URL = http://h:8090 \nKEY=\"ssh-ed25519 AAAA x\"\r\nNICS=EWA0\n";
    bz_conf_init(&c, test_override, NULL);
    CHECK(bz_conf_parse(&c, text, strlen(text), err, sizeof err));
    CHECK(strcmp(bz_conf_get(&c, "HUB_URL"), "http://h:8090") == 0);
    CHECK(strcmp(bz_conf_get(&c, "KEY"), "ssh-ed25519 AAAA x") == 0);
    CHECK(strcmp(bz_conf_get(&c, "NICS"), "EWA0") == 0);
    override_value = "-EWB*";
    CHECK(strcmp(bz_conf_get(&c, "NICS"), "-EWB*") == 0);   /* the override wins */
    override_value = NULL;
    CHECK(bz_conf_get(&c, "TOKEN") == NULL);
    bz_conf_free(&c);
    bz_conf_init(&c, NULL, NULL);
    CHECK(!bz_conf_parse(&c, "HUB_URLS=x\n", 11, err, sizeof err) && strstr(err, "line 1"));
    bz_conf_free(&c);
    bz_conf_init(&c, NULL, NULL);
    CHECK(!bz_conf_parse(&c, "TOKEN=a\nTOKEN=b\n", 16, err, sizeof err) && strstr(err, "twice"));
    bz_conf_free(&c);
    bz_conf_init(&c, NULL, NULL);
    CHECK(!bz_conf_parse(&c, "#\nnoequals\n", 11, err, sizeof err) && strstr(err, "line 2"));
    bz_conf_free(&c);
    bz_buf_init(&t);
    CHECK(bz_conf_token_file("# t\n\n  abc-123 \n", 16, &t) && strcmp((char *)t.p, "abc-123") == 0);
    CHECK(!bz_conf_token_file("a\nb\n", 4, &t));
    bz_buf_free(&t);
}

/* a real hub key line, from the reference hub in Phase 0 (public data) */
static const char *hub_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC+xV8Nx7iJVVAX7y8NKxoMcIIgK7V1AGO4oeBW0GLqM";

static void test_keys(void)
{
    bz_keys k;
    char err[128], text[512];
    unsigned char want[32];
    unhex(want, "2fb157c371ee225554"); /* the first 9 key bytes, from the base64 above */
    CHECK(bz_keys_parse(&k, hub_key, strlen(hub_key), err, sizeof err) && k.n == 1);
    CHECK(memcmp(k.ed25519[0], want, 9) == 0);
    snprintf(text, sizeof text, "# hub\n\nfrom=\"10.0.0.1,10.0.0.2\" %s beszel@hub\n%s\n", hub_key, hub_key);
    CHECK(bz_keys_parse(&k, text, strlen(text), err, sizeof err) && k.n == 2);
    CHECK(!bz_keys_parse(&k, "ssh-rsa AAAAB3NzaC1yc2E= x", 26, err, sizeof err) && strstr(err, "not supported"));
    CHECK(!bz_keys_parse(&k, "ssh-ed25519 AAAA", 16, err, sizeof err));
    CHECK(!bz_keys_parse(&k, "garbage", 7, err, sizeof err));
    CHECK(!bz_keys_parse(&k, "# only a comment\n", 17, err, sizeof err));
}

/* ---- WebSocket ---- */

static void test_ws(void)
{
    char key[25], err[128];
    unsigned char rnd[16];
    const unsigned char mask[4] = { 1, 2, 3, 4 };
    const char *resp;
    int status;
    bz_buf b, in;
    bz_ws_rx rx;
    /* RFC 6455 1.3: "the sample nonce" */
    memcpy(rnd, "the sample nonce", 16);
    bz_ws_make_key(key, rnd);
    CHECK(strcmp(key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);
    resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    CHECK(bz_ws_head_len((const unsigned char *)resp, strlen(resp)) == strlen(resp));
    CHECK(bz_ws_check_response(resp, strlen(resp), key, &status, err, sizeof err));
    resp = "HTTP/1.1 101 Switching Protocols\r\nupgrade: WebSocket\r\nconnection: keep-alive, upgrade\r\n"
           "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    CHECK(bz_ws_check_response(resp, strlen(resp), key, &status, err, sizeof err));
    resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOx=\r\n\r\n";
    CHECK(!bz_ws_check_response(resp, strlen(resp), key, &status, err, sizeof err));
    resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Extensions: permessage-deflate\r\n"
           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    CHECK(!bz_ws_check_response(resp, strlen(resp), key, &status, err, sizeof err));
    resp = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
    CHECK(!bz_ws_check_response(resp, strlen(resp), key, &status, err, sizeof err) && status == 401);
    CHECK(strstr(err, "401 Unauthorized") != NULL);
    CHECK(bz_ws_head_len((const unsigned char *)"HTTP/1.1 101\r\n", 14) == 0);

    bz_buf_init(&b);
    bz_ws_request(&b, "h:1", "/api/beszel/agent-connect", key, "tok", "0.21.0", "UA");
    CHECK(strstr((char *)b.p, "GET /api/beszel/agent-connect HTTP/1.1\r\nHost: h:1\r\n") == (char *)b.p);
    CHECK(strstr((char *)b.p, "\r\nX-Token: tok\r\n") && strstr((char *)b.p, "\r\nX-Beszel: 0.21.0\r\n"));
    CHECK(!strstr((char *)b.p, "Extensions") && memcmp(b.p + b.len - 4, "\r\n\r\n", 4) == 0);

    /* masked client frame, RFC 6455 5.7 "Hello" with our own mask */
    bz_buf_clear(&b);
    bz_ws_frame(&b, BZ_WS_OP_BINARY, "Hello", 5, mask);
    CHECK(hexeq(&b, "82850102030449676f686e"));   /* "Hello" XOR 01 02 03 04 */
    bz_buf_clear(&b);
    {   /* 126-byte payload uses the 16-bit length */
        unsigned char big[300];
        memset(big, 0, sizeof big);
        bz_ws_frame(&b, BZ_WS_OP_BINARY, big, 300, mask);
        CHECK(b.p[1] == (0x80 | 126) && b.p[2] == 1 && b.p[3] == 44 && b.len == 4 + 4 + 300);
    }

    /* parser: unmasked "Hello" split across reads, fragments, ping, close */
    bz_buf_init(&in);
    bz_ws_rx_init(&rx);
    bz_buf_put(&in, "\x82\x05He", 4);
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_NEED_MORE);
    bz_buf_put(&in, "llo", 3);
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_MESSAGE && rx.op == 2 && rx.msg.len == 5);
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_NEED_MORE && in.len == 0);   /* compacted once (L2) */
    bz_buf_put(&in, "\x02\x03Hel\x89\x02hi\x80\x02lo", 13);   /* fragment, ping, last fragment */
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_PING && rx.msg.len == 2 && memcmp(rx.msg.p, "hi", 2) == 0);
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_MESSAGE && rx.op == 2 && rx.msg.len == 5 &&
          memcmp(rx.msg.p, "Hello", 5) == 0);
    bz_buf_put(&in, "\x88\x05\x03\xe8" "bye", 7);
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_CLOSE && rx.close_code == 1000 && rx.msg.len == 3);
    bz_buf_put(&in, "\x88\x00", 2);
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_CLOSE && rx.close_code == 1005);
    bz_buf_clear(&in);
    rx.off = 0;
    bz_buf_put(&in, "\x82\x85\x00\x00\x00\x00Hello", 11);      /* masked from a server */
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_ERROR && rx.close_code == 1002);
    bz_buf_clear(&in);
    rx.off = 0;
    bz_buf_put(&in, "\xc2\x00", 2);                             /* RSV1 */
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_ERROR);
    bz_buf_clear(&in);
    rx.off = 0;
    bz_buf_put(&in, "\x09\x00", 2);                             /* fragmented ping */
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_ERROR);
    bz_buf_clear(&in);
    rx.off = 0;
    bz_buf_put(&in, "\x80\x00", 2);                             /* continuation alone */
    bz_ws_rx_free(&rx);
    bz_ws_rx_init(&rx);
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_ERROR);
    bz_buf_clear(&in);
    rx.off = 0;
    bz_buf_put(&in, "\x82\x7f\x00\x00\x00\x00\x7f\xff\xff\xff", 10);  /* 2 GiB */
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_ERROR && rx.close_code == 1009);
    bz_buf_clear(&in);
    rx.off = 0;
    bz_buf_put(&in, "\x83\x00", 2);                             /* reserved opcode */
    CHECK(bz_ws_parse(&rx, &in) == BZ_WS_ERROR);
    bz_ws_rx_free(&rx);
    bz_buf_free(&in);
    bz_buf_free(&b);
}

/* ---- protocol ---- */

static void test_proto(void)
{
    unsigned char in[128];
    size_t n;
    struct bz_request r;
    unsigned cms;
    int inc, need;
    const unsigned char *sig;
    size_t sl;
    bz_buf b;
    bz_buf_init(&b);
    /* captured requests (PROTOCOL.md 9) */
    n = unhex(in, "a3000001a2001903e801f40206");        /* GetData {1000, false}, id 6 */
    CHECK(bz_request_parse(&r, in, n) && r.action == 0 && r.has_id && r.id == 6);
    bz_data_options(&r, &cms, &inc);
    CHECK(cms == 1000 && inc == 0);
    n = unhex(in, "a3000001a20019ea6001f50202");        /* {60000, true}, id 2 */
    CHECK(bz_request_parse(&r, in, n));
    bz_data_options(&r, &cms, &inc);
    CHECK(cms == 60000 && inc == 1);
    n = unhex(in, "a200040205");                        /* GetSmartData, id 5 */
    CHECK(bz_request_parse(&r, in, n) && r.action == 4 && r.id == 5 && !r.data);
    bz_reply_unsupported(&b, &r);
    CHECK(hexeq(&b, "a10005"));                         /* upstream sent {0:5} */
    bz_buf_clear(&b);
    n = unhex(in, "a3000701a2000001a400600160026004000203");
    CHECK(bz_request_parse(&r, in, n) && r.action == 7 && r.id == 3);
    bz_reply_unsupported(&b, &r);
    CHECK(hexeq(&b, "a2000307a100a408000a000b000c00"));   /* captured reply bytes */
    bz_buf_clear(&b);
    /* fingerprint request {0: h'..'(64), 1: false}, id 1 */
    {
        unsigned char fr[80];
        size_t k = 0;
        int i;
        fr[k++] = 0xa3; fr[k++] = 0x00; fr[k++] = 0x01; fr[k++] = 0x01; fr[k++] = 0xa2;
        fr[k++] = 0x00; fr[k++] = 0x58; fr[k++] = 0x40;
        for (i = 0; i < 64; i++) fr[k++] = (unsigned char)i;
        fr[k++] = 0x01; fr[k++] = 0xf4; fr[k++] = 0x02; fr[k++] = 0x01;
        CHECK(bz_request_parse(&r, fr, k) && r.action == 1 && r.id == 1);
        CHECK(bz_fingerprint_request(&r, &sig, &sl, &need) && sl == 64 && sig[63] == 63 && !need);
    }
    bz_reply_fingerprint(&b, &r, "9a1dfdd2a3a88e9a701aaa18461b7ab871342721caf26c6f", NULL, NULL, NULL);
    /* captured: {0: 1, 2: {0: "9a1d..."}} - the 48-character string has a 1-byte length */
    {
        bz_cbr c;
        uint64_t v;
        const char *t;
        size_t tn;
        bz_cbr_init(&c, b.p, b.len);
        CHECK(bz_cbr_map(&c, &v) && v == 2 && bz_cbr_uint(&c, &v) && v == 0 && bz_cbr_uint(&c, &v) && v == 1);
        CHECK(bz_cbr_uint(&c, &v) && v == 2 && bz_cbr_map(&c, &v) && v == 1);
        CHECK(bz_cbr_uint(&c, &v) && v == 0 && bz_cbr_text(&c, &t, &tn) && tn == 48 && c.pos == b.len);
    }
    bz_buf_clear(&b);
    bz_reply_fingerprint(&b, &r, "ab", "host", "45876", "");
    /* {0: 1, 2: {0: "ab", 1: "host", 2: "45876"}}: the empty name is omitted */
    CHECK(hexeq(&b, "a2000102a3006261620164686f737402653435383736"));
    bz_buf_clear(&b);
    /* malformed requests */
    CHECK(!bz_request_parse(&r, (const unsigned char *)"\xa1\x00", 2));
    CHECK(!bz_request_parse(&r, (const unsigned char *)"\xa1\x01\x00", 3));   /* no action */
    CHECK(!bz_request_parse(&r, (const unsigned char *)"\x81\x00", 2));
    n = unhex(in, "a200000201ff");                       /* trailing byte */
    CHECK(!bz_request_parse(&r, in, n));
    n = unhex(in, "a3000001a20019ea6001f5021b0000000100000000");   /* id > uint32 */
    CHECK(!bz_request_parse(&r, in, n));
    /* bad options give zero options, as upstream */
    n = unhex(in, "a3000001a2001a0001000001f50202");   /* cacheTimeMs 65536 */
    CHECK(bz_request_parse(&r, in, n));
    bz_data_options(&r, &cms, &inc);
    CHECK(cms == 0 && inc == 0);
    /* unknown action and ZFS */
    n = unhex(in, "a2000c0209");
    CHECK(bz_request_parse(&r, in, n));
    bz_reply_unsupported(&b, &r);
    CHECK(hexeq(&b, "a200090372756e6b6e6f776e20616374696f6e3a203132"));   /* 18-byte text */
    bz_buf_clear(&b);
    n = unhex(in, "a200060209");
    CHECK(bz_request_parse(&r, in, n));
    bz_reply_unsupported(&b, &r);
    CHECK(hexeq(&b, "a2000907a168636f6d706c657465f5"));
    bz_buf_free(&b);
}

/* ---- stats ---- */

static void fs_fake(void *ctx, const char *dev, struct bz_fs_usage *u)
{
    (void)ctx;
    memset(u, 0, sizeof *u);
    if (strcmp(dev, "DKA0:") == 0) {
        u->ok = 1;
        u->total = UINT64_C(21474836480);
        u->used = UINT64_C(10737418240);
    } else if (strcmp(dev, "DKA200:") == 0) {
        u->ok = 1;
        u->total = UINT64_C(1073741824);
        u->used = UINT64_C(268435456);
    } else if (strcmp(dev, "DKA300:") == 0) {
        u->ok = 1;                          /* rounds to 0 GiB: must be left out */
        u->total = 1000;
        u->used = 10;
    }
}

static void cpu_set(struct bz_cpu_times *t, uint64_t total, uint64_t idle, uint64_t user, uint64_t sys)
{
    memset(t, 0, sizeof *t);
    t->total = total;
    t->idle = idle;
    t->user = user;
    t->system = sys;
}

static void test_stats(void)
{
    static struct bz_stats st;
    static struct bz_sample s;
    static struct bz_stats_out o;
    bz_buf b;
    bz_stats_init(&st);
    bz_fs_spec_parse(&st.cfg.root, "DKA0: __ System ", 16);
    CHECK(strcmp(st.cfg.root.dev, "DKA0:") == 0 && strcmp(st.cfg.root.name, "System") == 0);
    CHECK(bz_stats_cfg_extra(&st.cfg, " DKA200:__Tools, ,DKA300:,DKA400:"));
    CHECK(st.cfg.nextra == 3);
    memset(&s, 0, sizeof s);
    s.ncpu = 2;
    cpu_set(&s.all, 2000, 1600, 200, 200);
    cpu_set(&s.cpu[0], 1000, 900, 50, 50);
    cpu_set(&s.cpu[1], 1000, 700, 150, 150);
    s.mem_total = UINT64_C(8589934592);
    s.mem_used = UINT64_C(3221225472);
    s.swap_total = UINT64_C(2147483648);
    s.nnic = 2;
    strcpy(s.nic[0].name, "EWA0");
    s.nic[0].tx = 1000;
    s.nic[0].rx = 5000;
    strcpy(s.nic[1].name, "EWB0");      /* no traffic: filtered */
    bz_stats_seed(&st, &s);

    /* 60 s later: 25% busy overall, CPU1 50%, CPU0 0% */
    cpu_set(&s.all, 2000 + 12000, 1600 + 9000, 200 + 1800, 200 + 1200);
    cpu_set(&s.cpu[0], 1000 + 6000, 900 + 6000, 50, 50);
    cpu_set(&s.cpu[1], 1000 + 6000, 700 + 3000, 150 + 1800, 150 + 1200);
    bz_stats_compute(&st, 60000, 1000, &s, fs_fake, NULL, &o);
    CHECK(o.cpu == 25 && o.cpub[0] == 15 && o.cpub[1] == 10 && o.cpub[4] == 75);
    CHECK(o.ncores == 2 && o.cores[0] == 0 && o.cores[1] == 50);
    CHECK(o.mem == 8 && o.mem_used == 3 && o.mem_pct == 37.5 && o.swap == 2 && o.swap_used == 0);
    CHECK(o.disk_total == 20 && o.disk_used == 10 && o.disk_pct == 50 && strcmp(o.rdn, "System") == 0);
    CHECK(o.nefs == 1 && strcmp(o.efs[0].key, "Tools") == 0 && o.efs[0].total == 1 && o.efs[0].used == 0.25);
    CHECK(o.nni == 1 && o.bw[0] == 0 && o.bw[1] == 0);  /* first network sample: no rate yet */
    CHECK(o.ni[0].v[2] == 1000 && o.ni[0].v[3] == 5000);

    /* a real-time request seeds from the 60000 CPU baseline, not from boot */
    cpu_set(&s.all, 14000 + 200, 10600 + 100, 2000 + 60, 1400 + 40);
    s.cpu[0].total += 100; s.cpu[0].idle += 50; s.cpu[0].user += 50;
    s.cpu[1].total += 100; s.cpu[1].idle += 50; s.cpu[1].system += 50;
    s.nic[0].tx += 2000;
    s.nic[0].rx += 10000;
    bz_stats_compute(&st, 1000, 2000, &s, fs_fake, NULL, &o);
    CHECK(o.cpu == 50 && o.cpub[0] == 30 && o.cpub[1] == 20);
    CHECK(o.bw[0] == 0);                /* the 1000 interval's first network sample */
    /* the 60000 interval's baseline did not move: its delta covers both steps */
    s.nic[0].tx += 1000;
    s.nic[0].rx += 1000;
    bz_stats_compute(&st, 60000, 61000, &s, fs_fake, NULL, &o);
    CHECK(o.cpu == 50);                 /* 100 busy of 200 since the 60000 sample */
    CHECK(o.bw[0] == 3000 * 1000 / 60000 && o.bw[1] == 11000 * 1000 / 60000);
    CHECK(o.ni[0].v[0] == 50 && o.ni[0].v[1] == 183);
    /* a counter that went down: per-NIC delta is the new value; the total takes the reset path */
    s.nic[0].tx = 100;
    bz_stats_compute(&st, 60000, 121000, &s, fs_fake, NULL, &o);
    CHECK(o.bw[0] == 0 && o.bw[1] == 0 && o.ni[0].v[0] == 1);
    /* no time passed: zeros, baseline kept */
    bz_stats_compute(&st, 60000, 121000, &s, fs_fake, NULL, &o);
    CHECK(o.cpu == 0 && o.cpub[4] == 0);

    /* NICS */
    bz_stats_cfg_nics(&st.cfg, "EWB0");
    CHECK(bz_nic_selected(&st.cfg, "EWB0", 0, 0) && !bz_nic_selected(&st.cfg, "EWA0", 5, 5));
    bz_stats_cfg_nics(&st.cfg, "-EW*0");
    CHECK(!bz_nic_selected(&st.cfg, "EWA0", 5, 5) && bz_nic_selected(&st.cfg, "EIA1", 5, 5));
    CHECK(!bz_nic_selected(&st.cfg, "EIA1", 5, 0));     /* blacklist keeps the traffic filter */
    bz_stats_cfg_nics(&st.cfg, "");
    CHECK(!bz_nic_selected(&st.cfg, "EWA0", 5, 5));     /* empty whitelist selects nothing */
    st.cfg.nics_set = 0;
    CHECK(bz_nic_selected(&st.cfg, "EWA0", 5, 5) && !bz_nic_selected(&st.cfg, "lo0", 5, 5));
    CHECK(bz_glob("ew?0", "EWA0") && bz_glob("*", "x") && !bz_glob("EW*1", "EWA0") && bz_glob("E*A*0", "EWA0"));

    /* load average: Linux damping, one step per 5 s */
    {
        struct bz_load l;
        memset(&l, 0, sizeof l);
        bz_load_sample(&l, 0, 2);
        CHECK(l.avg[0] == 0);
        bz_load_sample(&l, 4999, 2);
        CHECK(l.avg[0] == 0);
        bz_load_sample(&l, 5000, 2);
        CHECK(fabs(l.avg[0] - 2 * (1 - 1884.0 / 2048)) < 1e-12);
        bz_load_sample(&l, 5000 + 3600 * 1000, 2);       /* an hour later: settled */
        CHECK(fabs(l.avg[0] - 2) < 1e-6 && fabs(l.avg[1] - 2) < 1e-3 && l.avg[2] > 1.9);
    }

    /* encoding: field order, omit rules, byte-string cores */
    bz_buf_init(&b);
    memset(&o, 0, sizeof o);
    o.cpu = 16.45;
    o.mem = 7;
    o.la[0] = 1;
    o.cpub[4] = 100;
    o.ncores = 2;
    o.cores[0] = 0x13;
    o.cores[1] = 0x09;
    o.uptime = 5;
    o.threads = 2;
    bz_encode_combined(&b, &o, NULL, "0.21.0");
    {
        bz_cbr c;
        uint64_t v, k, i, prev = 0;
        int first = 1;
        bz_cbr_init(&c, b.p, b.len);
        CHECK(bz_cbr_map(&c, &v) && v == 4);
        CHECK(bz_cbr_uint(&c, &k) && k == 0 && bz_cbr_map(&c, &v));
        /* Stats keys ascend (declaration order equals key order for what we send) */
        for (i = 0; i < v; i++) {
            CHECK(bz_cbr_uint(&c, &k) && (first || k > prev));
            first = 0;
            prev = k;
            CHECK(k != 7 && k != 8 && k != 21 && k != 26 && k != 31);   /* omitted when empty */
            if (k == 34) {
                const unsigned char *p;
                size_t n;
                CHECK(bz_cbr_bytes(&c, &p, &n) && n == 2 && p[0] == 0x13);
            } else {
                CHECK(bz_cbr_skip(&c));
            }
        }
        CHECK(bz_cbr_uint(&c, &k) && k == 1 && bz_cbr_skip(&c));
        CHECK(bz_cbr_uint(&c, &k) && k == 2 && bz_cbr_is_null(&c));
        CHECK(bz_cbr_uint(&c, &k) && k == 6 && bz_cbr_map(&c, &v) && v == 0 && c.pos == b.len);
    }
    bz_buf_free(&b);
    bz_stats_free(&st);
}

/* ---- agent state machine ---- */

static EVP_PKEY *hub_pkey;
static unsigned char hub_pub[32];
static int fake_sample(void *ctx, struct bz_sample *s)
{
    (void)ctx;
    memset(s, 0, sizeof *s);
    s->ncpu = 1;
    cpu_set(&s->all, 100, 50, 25, 25);
    s->cpu[0] = s->all;
    s->mem_total = 1;
    return 1;
}
static double fake_runq(void *ctx) { (void)ctx; return 0; }
static int log_lines;
static char last_log[512];
static void fake_log(void *ctx, int level, const char *msg)
{
    (void)ctx;
    (void)level;
    log_lines++;
    snprintf(last_log, sizeof last_log, "%s", msg);
}

static void sign(const char *msg, unsigned char sig[64])
{
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    size_t sl = 64;
    EVP_DigestSignInit(c, NULL, NULL, NULL, hub_pkey);
    EVP_DigestSign(c, sig, &sl, (const unsigned char *)msg, strlen(msg));
    EVP_MD_CTX_free(c);
}

/* deliver one unmasked server frame */
static int deliver(struct bz_agent *a, uint64_t now, int op, const void *p, size_t n)
{
    unsigned char h[4];
    bz_buf f;
    int rc;
    bz_buf_init(&f);
    h[0] = (unsigned char)(0x80 | op);
    if (n < 126) {
        h[1] = (unsigned char)n;
        bz_buf_put(&f, h, 2);
    } else {
        h[1] = 126;
        h[2] = (unsigned char)(n >> 8);
        h[3] = (unsigned char)n;
        bz_buf_put(&f, h, 4);
    }
    bz_buf_put(&f, p, n);
    rc = bz_agent_on_data(a, now, f.p, f.len);
    bz_buf_free(&f);
    return rc;
}

/* unmask the agent's single queued frame into out */
static int sent_frame(struct bz_agent *a, bz_buf *out)
{
    size_t hl = 2, n, i;
    const unsigned char *m;
    if (a->out.len < 6 || !(a->out.p[1] & 0x80))
        return 0;
    n = a->out.p[1] & 127;
    if (n == 126) {
        n = ((size_t)a->out.p[2] << 8) | a->out.p[3];
        hl = 4;
    }
    m = a->out.p + hl;
    bz_buf_clear(out);
    for (i = 0; i < n; i++)
        bz_buf_putc(out, a->out.p[hl + 4 + i] ^ m[i & 3]);
    bz_buf_consume(&a->out, hl + 4 + n);
    return 1;
}

static void test_agent(void)
{
    static struct bz_agent a;
    static struct bz_agent_cfg cfg;
    struct bz_identity id;
    struct bz_env env;
    char err[128], key[25], resp[256];
    unsigned char sha[20], sig[64], req[128];
    bz_buf acc, msg;
    size_t k;
    {
        EVP_PKEY_CTX *pc = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
        size_t pl = 32;
        EVP_PKEY_keygen_init(pc);
        EVP_PKEY_keygen(pc, &hub_pkey);
        EVP_PKEY_CTX_free(pc);
        EVP_PKEY_get_raw_public_key(hub_pkey, hub_pub, &pl);
    }
    memset(&cfg, 0, sizeof cfg);
    CHECK(bz_url_parse(&cfg.url, "http://hub:8090", err, sizeof err));
    cfg.token = "tok-123";
    cfg.keys.n = 1;
    memcpy(cfg.keys.ed25519[0], hub_pub, 32);
    strcpy(cfg.fingerprint, "f00d");
    cfg.system_name = "vms1";
    cfg.port = "45876";
    cfg.version = "0.21.0";
    cfg.user_agent = "UA";
    memset(&id, 0, sizeof id);
    strcpy(id.hostname, "node");
    env.ctx = NULL;
    env.sample = fake_sample;
    env.fs_usage = fs_fake;
    env.runq = fake_runq;
    env.log = fake_log;
    bz_agent_init(&a, &cfg, &id, &env, 1000);
    a.st.cfg.version = "0.21.0";
    strcpy(a.st.cfg.root.dev, "DKA0:");

    /* pacing: connect at once; a failure waits 10 s */
    CHECK(bz_agent_should_connect(&a, 1000));
    bz_agent_on_connect_failed(&a, 1000, "refused");
    CHECK(!bz_agent_should_connect(&a, 10999) && bz_agent_should_connect(&a, 11000));

    /* upgrade */
    CHECK(bz_agent_on_connected(&a, 11000) && a.state == BZ_UPGRADING);
    CHECK(strstr((char *)a.out.p, "X-Token: tok-123\r\n") != NULL);
    {
        const char *kp = strstr((char *)a.out.p, "Sec-WebSocket-Key: ") + 19;
        memcpy(key, kp, 24);
        key[24] = 0;
    }
    bz_buf_clear(&a.out);
    snprintf(resp, sizeof resp, "%s%s", key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    bz_sha1(resp, strlen(resp), sha);
    bz_buf_init(&acc);
    bz_buf_init(&msg);
    bz_b64_encode(&acc, sha, 20);
    snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", (char *)acc.p);
    CHECK(bz_agent_on_data(&a, 11010, resp, strlen(resp)) && a.state == BZ_OPEN);

    /* before verification: GetData is ignored */
    k = unhex(req, "a3000001a20019ea6001f50202");
    CHECK(deliver(&a, 11020, 2, req, k) && a.out.len == 0 && strstr(last_log, "hub not verified"));

    /* a bad signature: no reply, still unverified */
    memset(sig, 7, 64);
    k = 0;
    req[k++] = 0xa3; req[k++] = 0x00; req[k++] = 0x01; req[k++] = 0x01; req[k++] = 0xa2;
    req[k++] = 0x00; req[k++] = 0x58; req[k++] = 0x40;
    memcpy(req + k, sig, 64); k += 64;
    req[k++] = 0x01; req[k++] = 0xf5; req[k++] = 0x02; req[k++] = 0x01;
    CHECK(deliver(&a, 11030, 2, req, k) && a.out.len == 0 && !a.verified);
    CHECK(strstr(last_log, "invalid signature") != NULL);

    /* the hub's signature over the token: verified, fingerprint with sys info */
    sign("tok-123", sig);
    memcpy(req + 8, sig, 64);
    CHECK(deliver(&a, 11040, 2, req, k) && a.verified);
    CHECK(sent_frame(&a, &msg));
    {
        bz_cbr c;
        uint64_t v;
        const char *t;
        size_t tn;
        bz_cbr_init(&c, msg.p, msg.len);
        CHECK(bz_cbr_map(&c, &v) && v == 2 && bz_cbr_uint(&c, &v) && bz_cbr_uint(&c, &v) && v == 1);
        CHECK(bz_cbr_uint(&c, &v) && v == 2 && bz_cbr_map(&c, &v) && v == 4);
        CHECK(bz_cbr_uint(&c, &v) && bz_cbr_text(&c, &t, &tn) && tn == 4 && memcmp(t, "f00d", 4) == 0);
        CHECK(bz_cbr_uint(&c, &v) && v == 1 && bz_cbr_text(&c, &t, &tn) && memcmp(t, "node", 4) == 0);
    }

    /* GetData with details, then a cached repeat without them */
    k = unhex(req, "a3000001a20019ea6001f50202");
    CHECK(deliver(&a, 12000, 2, req, k) && sent_frame(&a, &msg));
    {
        bz_cbr c;
        uint64_t v, key2;
        bz_cbr_init(&c, msg.p, msg.len);
        CHECK(bz_cbr_map(&c, &v) && v == 2 && bz_cbr_uint(&c, &v) && bz_cbr_uint(&c, &v) && v == 2);
        CHECK(bz_cbr_uint(&c, &key2) && key2 == 1 && bz_cbr_map(&c, &v) && v == 5);   /* with Details */
    }
    k = unhex(req, "a3000001a20019ea6001f50203");
    CHECK(deliver(&a, 12500, 2, req, k) && sent_frame(&a, &msg));
    {
        bz_cbr c;
        uint64_t v;
        bz_cbr_init(&c, msg.p, msg.len);
        CHECK(bz_cbr_map(&c, &v) && bz_cbr_uint(&c, &v) && bz_cbr_uint(&c, &v) && v == 3);
        CHECK(bz_cbr_uint(&c, &v) && v == 1 && bz_cbr_map(&c, &v) && v == 4);         /* cached: none */
    }

    /* ping gets a pong with the same payload; text frames are ignored */
    CHECK(deliver(&a, 13000, 9, "pq", 2) && sent_frame(&a, &msg) && msg.len == 2 && a.out.len == 0);
    CHECK(deliver(&a, 13000, 1, "hello", 5) && a.out.len == 0);

    /* deadline: nothing for 120 s closes */
    CHECK(bz_agent_tick(&a, 13000 + 120000));
    CHECK(!bz_agent_tick(&a, 13001 + 120000));
    bz_buf_clear(&a.out);
    /* a verified drop retries quickly: 5 s after the last attempt (11000), so now */
    bz_agent_on_closed(&a, 133002);
    CHECK(a.state == BZ_DOWN && bz_agent_should_connect(&a, 133002));

    /* a failed upgrade waits 10 s before the next attempt */
    CHECK(bz_agent_on_connected(&a, 140000));
    bz_buf_clear(&a.out);
    /* the old response no longer matches the new key: the upgrade is refused */
    CHECK(bz_agent_on_data(&a, 140010, resp, strlen(resp)) == 0);
    bz_agent_on_closed(&a, 140010);
    CHECK(!bz_agent_should_connect(&a, 149999) && bz_agent_should_connect(&a, 150000));

    /* review M4: an unverified peer gets at most 10 unusable messages */
    CHECK(bz_agent_on_connected(&a, 200000));
    {
        const char *kp = strstr((char *)a.out.p, "Sec-WebSocket-Key: ") + 19;
        char r2[256];
        memcpy(key, kp, 24);
        key[24] = 0;
        bz_buf_clear(&a.out);
        snprintf(resp, sizeof resp, "%s%s", key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        bz_sha1(resp, strlen(resp), sha);
        bz_buf_clear(&acc);
        bz_b64_encode(&acc, sha, 20);
        snprintf(r2, sizeof r2, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", (char *)acc.p);
        CHECK(bz_agent_on_data(&a, 200010, r2, strlen(r2)) && a.state == BZ_OPEN);
    }
    {
        int i, closed = 0;
        for (i = 0; i < 12 && !closed; i++)
            closed = !deliver(&a, 200020, 2, "\xff", 1);       /* not CBOR */
        CHECK(closed && i == 11 && strstr(last_log, "could not be used"));
    }
    bz_agent_on_closed(&a, 200030);
    /* review I6: a hub that never proves its key is dropped after 60 s */
    CHECK(bz_agent_on_connected(&a, 300000));
    a.state = BZ_OPEN;
    bz_buf_clear(&a.out);
    CHECK(bz_agent_tick(&a, 300000 + 60000));
    CHECK(!bz_agent_tick(&a, 300001 + 60000) && strstr(last_log, "did not prove its key"));
    bz_agent_on_closed(&a, 360002);

    bz_buf_free(&acc);
    bz_buf_free(&msg);
    bz_agent_free(&a);
    EVP_PKEY_free(hub_pkey);
}

int main(void)
{
    test_buf_text();
    test_cbor();
    test_url();
    test_conf();
    test_keys();
    test_ws();
    test_proto();
    test_stats();
    test_agent();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? EXIT_FAILURE : EXIT_SUCCESS;
}
