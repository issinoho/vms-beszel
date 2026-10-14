/* bz_ws.c - WebSocket client framing (see bz_ws.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_crypto.h"
#include "bz_text.h"
#include "bz_ws.h"

static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void bz_ws_make_key(char key[25], const unsigned char rnd[16])
{
    bz_buf b;
    bz_buf_init(&b);
    bz_b64_encode(&b, rnd, 16);
    if (b.oom || b.len != 24)
        key[0] = 0;
    else
        memcpy(key, b.p, 25);
    bz_buf_free(&b);
}

void bz_ws_request(bz_buf *out, const char *host_header, const char *target, const char *key,
                   const char *token, const char *version, const char *user_agent)
{
    /* the same headers as upstream's agent (PROTOCOL.md 1.2) */
    bz_buf_printf(out, "GET %s HTTP/1.1\r\n", target);
    bz_buf_printf(out, "Host: %s\r\n", host_header);
    bz_buf_printf(out, "User-Agent: %s\r\n", user_agent);
    bz_buf_puts(out, "Connection: Upgrade\r\n");
    bz_buf_printf(out, "Sec-WebSocket-Key: %s\r\n", key);
    bz_buf_puts(out, "Sec-WebSocket-Version: 13\r\n");
    bz_buf_puts(out, "Upgrade: websocket\r\n");
    bz_buf_printf(out, "X-Beszel: %s\r\n", version);
    bz_buf_printf(out, "X-Token: %s\r\n", token);
    bz_buf_puts(out, "\r\n");
}

size_t bz_ws_head_len(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 3; i < n; i++)
        if (p[i] == '\n' && p[i - 1] == '\r' && p[i - 2] == '\n' && p[i - 3] == '\r')
            return i + 1;
    return 0;
}

/* find header `name` (case-insensitive); value trimmed into *v,*vn */
static int header(const char *head, size_t len, const char *name, const char **v, size_t *vn)
{
    size_t i = 0, nl = strlen(name);
    /* skip the status line */
    while (i < len && head[i] != '\n')
        i++;
    i++;
    while (i < len) {
        const char *line = head + i;
        size_t n = 0;
        while (i + n < len && head[i + n] != '\n')
            n++;
        i += n + 1;
        if (n > nl && line[nl] == ':') {
            size_t k;
            int same = 1;
            for (k = 0; k < nl; k++) {
                int a = (unsigned char)line[k], b = (unsigned char)name[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                *v = line + nl + 1;
                *vn = n - nl - 1;
                bz_trim(v, vn);
                return 1;
            }
        }
    }
    return 0;
}

/* does a comma-separated header value contain token (case-insensitive)? */
static int has_token(const char *v, size_t vn, const char *tok)
{
    while (vn) {
        const char *t = v;
        size_t tn = 0;
        while (tn < vn && v[tn] != ',')
            tn++;
        v += tn < vn ? tn + 1 : tn;
        vn -= tn < vn ? tn + 1 : tn;
        bz_trim(&t, &tn);
        if (bz_ieqn(t, tn, tok))
            return 1;
    }
    return 0;
}

int bz_ws_check_response(const char *head, size_t len, const char *key, int *status,
                         char *err, size_t errlen)
{
    const char *v;
    size_t vn;
    char buf[80];
    unsigned char sha[20];
    bz_buf want;
    int ok;
    *status = 0;
    if (len < 12 || memcmp(head, "HTTP/1.", 7) != 0 || head[8] != ' ' ||
        sscanf(head + 9, "%3d", status) != 1) {
        snprintf(err, errlen, "not an HTTP response");
        return 0;
    }
    if (*status != 101) {
        /* the status line, for the log: "401 Unauthorized" etc. */
        size_t n = 0;
        while (9 + n < len && head[9 + n] != '\r' && head[9 + n] != '\n' && n < 60)
            n++;
        snprintf(err, errlen, "hub answered %.*s", (int)n, head + 9);
        return 0;
    }
    if (!header(head, len, "Upgrade", &v, &vn) || !bz_ieqn(v, vn, "websocket")) {
        snprintf(err, errlen, "missing Upgrade: websocket");
        return 0;
    }
    if (!header(head, len, "Connection", &v, &vn) || !has_token(v, vn, "upgrade")) {
        snprintf(err, errlen, "missing Connection: Upgrade");
        return 0;
    }
    if (header(head, len, "Sec-WebSocket-Extensions", &v, &vn) && vn) {
        snprintf(err, errlen, "hub selected an extension we did not offer");
        return 0;
    }
    if (header(head, len, "Sec-WebSocket-Protocol", &v, &vn) && vn) {
        snprintf(err, errlen, "hub selected a subprotocol we did not offer");
        return 0;
    }
    if (!header(head, len, "Sec-WebSocket-Accept", &v, &vn)) {
        snprintf(err, errlen, "missing Sec-WebSocket-Accept");
        return 0;
    }
    snprintf(buf, sizeof buf, "%s%s", key, guid);
    bz_sha1(buf, strlen(buf), sha);
    bz_buf_init(&want);
    bz_b64_encode(&want, sha, 20);
    ok = !want.oom && want.len == vn && memcmp(want.p, v, vn) == 0;
    bz_buf_free(&want);
    if (!ok)
        snprintf(err, errlen, "wrong Sec-WebSocket-Accept");
    return ok;
}

static void frame_head(bz_buf *out, int opcode, size_t n, const unsigned char mask[4])
{
    unsigned char h[14];
    size_t hl;
    h[0] = (unsigned char)(0x80 | opcode);  /* FIN */
    if (n < 126) {
        h[1] = (unsigned char)(0x80 | n);
        hl = 2;
    } else if (n <= 0xFFFF) {
        h[1] = 0x80 | 126;
        h[2] = (unsigned char)(n >> 8);
        h[3] = (unsigned char)n;
        hl = 4;
    } else {
        uint64_t v = n;
        int i;
        h[1] = 0x80 | 127;
        for (i = 9; i >= 2; i--) {
            h[i] = (unsigned char)(v & 0xFF);
            v >>= 8;
        }
        hl = 10;
    }
    memcpy(h + hl, mask, 4);
    bz_buf_put(out, h, hl + 4);
}

void bz_ws_frame(bz_buf *out, int opcode, const void *payload, size_t n,
                 const unsigned char mask[4])
{
    const unsigned char *p = payload;
    size_t i, start;
    frame_head(out, opcode, n, mask);
    start = out->len;
    bz_buf_put(out, payload, n);
    if (out->oom)
        return;
    for (i = 0; i < n; i++)
        out->p[start + i] = (unsigned char)(p[i] ^ mask[i & 3]);
}

void bz_ws_close_frame(bz_buf *out, unsigned code, const unsigned char mask[4])
{
    unsigned char c[2];
    c[0] = (unsigned char)(code >> 8);
    c[1] = (unsigned char)code;
    bz_ws_frame(out, BZ_WS_OP_CLOSE, c, 2, mask);
}

void bz_ws_rx_init(bz_ws_rx *rx)
{
    bz_buf_init(&rx->msg);
    bz_buf_init(&rx->frag);
    rx->frag_op = -1;
    rx->op = 0;
    rx->close_code = 0;
    rx->error = NULL;
}

void bz_ws_rx_free(bz_ws_rx *rx)
{
    bz_buf_free(&rx->msg);
    bz_buf_free(&rx->frag);
}

static enum bz_ws_event proto_error(bz_ws_rx *rx, unsigned code, const char *why)
{
    rx->close_code = code;
    rx->error = why;
    return BZ_WS_ERROR;
}

enum bz_ws_event bz_ws_parse(bz_ws_rx *rx, bz_buf *in)
{
    for (;;) {
        const unsigned char *p = in->p;
        size_t n = in->len, hl = 2;
        uint64_t plen;
        int fin, op;
        if (n < 2)
            return BZ_WS_NEED_MORE;
        fin = p[0] >> 7;
        op = p[0] & 15;
        if (p[0] & 0x70)
            return proto_error(rx, 1002, "reserved bits set");
        if (p[1] & 0x80)
            return proto_error(rx, 1002, "masked frame from the hub");
        plen = p[1] & 127;
        if (plen == 126) {
            if (n < 4)
                return BZ_WS_NEED_MORE;
            plen = ((uint64_t)p[2] << 8) | p[3];
            hl = 4;
        } else if (plen == 127) {
            int i;
            if (n < 10)
                return BZ_WS_NEED_MORE;
            plen = 0;
            for (i = 2; i < 10; i++)
                plen = (plen << 8) | p[i];
            hl = 10;
        }
        if (op >= 8) {
            if (!fin || plen > 125)
                return proto_error(rx, 1002, "invalid control frame");
            if (op > BZ_WS_OP_PONG)
                return proto_error(rx, 1002, "reserved opcode");
        } else if (op > BZ_WS_OP_BINARY) {
            return proto_error(rx, 1002, "reserved opcode");
        }
        if (plen > BZ_WS_MAX_MESSAGE)
            return proto_error(rx, 1009, "message too big");
        if (n - hl < plen)
            return BZ_WS_NEED_MORE;

        if (op >= 8) {
            bz_buf_set(&rx->msg, p + hl, (size_t)plen);
            bz_buf_consume(in, hl + (size_t)plen);
            if (rx->msg.oom)
                return proto_error(rx, 1011, "out of memory");
            rx->op = op;
            if (op == BZ_WS_OP_PING)
                return BZ_WS_PING;
            if (op == BZ_WS_OP_PONG)
                return BZ_WS_PONG;
            /* close: optional 2-byte code, then a UTF-8 reason */
            if (rx->msg.len == 1)
                return proto_error(rx, 1002, "invalid close payload");
            if (rx->msg.len >= 2) {
                rx->close_code = ((unsigned)rx->msg.p[0] << 8) | rx->msg.p[1];
                bz_buf_consume(&rx->msg, 2);
            } else {
                rx->close_code = 1005;
            }
            return BZ_WS_CLOSE;
        }

        if (op == BZ_WS_OP_CONT) {
            if (rx->frag_op < 0)
                return proto_error(rx, 1002, "continuation without a message");
        } else if (rx->frag_op >= 0) {
            return proto_error(rx, 1002, "new message inside a fragmented one");
        }
        if (op != BZ_WS_OP_CONT && fin) {
            /* the common case: a whole message in one frame */
            bz_buf_set(&rx->msg, p + hl, (size_t)plen);
            bz_buf_consume(in, hl + (size_t)plen);
            if (rx->msg.oom)
                return proto_error(rx, 1011, "out of memory");
            rx->op = op;
            return BZ_WS_MESSAGE;
        }
        if (op != BZ_WS_OP_CONT) {
            bz_buf_clear(&rx->frag);
            rx->frag_op = op;
        }
        if (rx->frag.len + plen > BZ_WS_MAX_MESSAGE)
            return proto_error(rx, 1009, "message too big");
        bz_buf_put(&rx->frag, p + hl, (size_t)plen);
        bz_buf_consume(in, hl + (size_t)plen);
        if (rx->frag.oom)
            return proto_error(rx, 1011, "out of memory");
        if (fin) {
            bz_buf_set(&rx->msg, rx->frag.p, rx->frag.len);
            rx->op = rx->frag_op;
            rx->frag_op = -1;
            bz_buf_clear(&rx->frag);
            if (rx->msg.oom)
                return proto_error(rx, 1011, "out of memory");
            return BZ_WS_MESSAGE;
        }
        /* more fragments to come: loop for the next frame */
    }
}
