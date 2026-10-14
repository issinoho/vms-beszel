/* bz_ws.h - the client side of RFC 6455, as the hub needs it (PROTOCOL.md 1.4).
 *
 * No extensions and no subprotocol are offered or accepted.  Client frames are
 * masked and sent unfragmented.  The parser accepts fragmented messages,
 * interleaved control frames, and rejects what RFC 6455 forbids a server to
 * send (masked frames, RSV bits, oversized or fragmented control frames).
 */
#ifndef BZ_WS_H
#define BZ_WS_H

#include <stddef.h>
#include <stdint.h>

#include "bz_buf.h"

#define BZ_WS_OP_CONT 0
#define BZ_WS_OP_TEXT 1
#define BZ_WS_OP_BINARY 2
#define BZ_WS_OP_CLOSE 8
#define BZ_WS_OP_PING 9
#define BZ_WS_OP_PONG 10

/* largest message we accept from the hub; its requests are a few hundred bytes */
#define BZ_WS_MAX_MESSAGE (1024 * 1024)

/* 16 random bytes -> the 24-character Sec-WebSocket-Key */
void bz_ws_make_key(char key[25], const unsigned char rnd[16]);
void bz_ws_request(bz_buf *out, const char *host_header, const char *target, const char *key,
                   const char *token, const char *version, const char *user_agent);

/* Checks a complete response head (up to and including the blank line).
   Returns 1 if the upgrade succeeded.  On failure *status is the HTTP status
   (0 if unparsable) and err says why. */
int bz_ws_check_response(const char *head, size_t len, const char *key, int *status,
                         char *err, size_t errlen);
/* length of the response head including CRLFCRLF, or 0 if not complete yet */
size_t bz_ws_head_len(const unsigned char *p, size_t n);

void bz_ws_frame(bz_buf *out, int opcode, const void *payload, size_t n,
                 const unsigned char mask[4]);
void bz_ws_close_frame(bz_buf *out, unsigned code, const unsigned char mask[4]);

enum bz_ws_event {
    BZ_WS_NEED_MORE = 0,
    BZ_WS_MESSAGE,   /* a complete text or binary message: op, msg */
    BZ_WS_PING,      /* payload in msg */
    BZ_WS_PONG,
    BZ_WS_CLOSE,     /* close_code (1005 if none), reason in msg */
    BZ_WS_ERROR      /* protocol error: close_code is what to send back */
};

typedef struct bz_ws_rx {
    bz_buf msg;          /* the message (or control payload) being returned */
    bz_buf frag;         /* a fragmented data message being assembled */
    int frag_op;         /* its opcode, or -1 if none in progress */
    int op;              /* opcode of the returned message */
    unsigned close_code;
    const char *error;
    size_t off;          /* parsed bytes at the front of the input, removed at NEED_MORE */
} bz_ws_rx;

void bz_ws_rx_init(bz_ws_rx *rx);
void bz_ws_rx_free(bz_ws_rx *rx);
/* Parse the next event from the front of in.  Consumed bytes are removed in
   one move when the parser needs more (not after every frame: review L2).
   Only append to in between calls; to start over, set rx->off to 0. */
enum bz_ws_event bz_ws_parse(bz_ws_rx *rx, bz_buf *in);

#endif
