/* bz_url.h - HUB_URL and the agent-connect address (PROTOCOL.md 1.1).
 *
 * Upstream turns https into wss and every other scheme into plain ws, so
 * HUB_URL=wss://... silently connects without TLS.  We accept only http and
 * https and reject anything else at startup (DECISIONS.md D11).  The path is
 * path.Join(<HUB_URL path>, "api/beszel/agent-connect") with path.Clean's
 * rules; the query string is kept.
 */
#ifndef BZ_URL_H
#define BZ_URL_H

#include <stddef.h>

typedef struct bz_url {
    int tls;
    char host[256];        /* without brackets for IPv6 literals */
    char port[8];
    char host_header[272]; /* host[:port] as it appeared, for the Host header */
    char target[1024];     /* cleaned path + "?query" for the request line */
} bz_url;

int bz_url_parse(bz_url *u, const char *s, char *err, size_t errlen);
/* path.Clean, for absolute paths; out must hold strlen(in)+2 bytes */
void bz_path_clean(char *out, const char *in);

#endif
