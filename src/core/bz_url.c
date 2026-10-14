/* bz_url.c - HUB_URL parsing (see bz_url.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_text.h"
#include "bz_url.h"

void bz_path_clean(char *out, const char *in)
{
    /* Go's path.Clean for a rooted path: collapse "//", drop ".", resolve "..",
       no trailing slash except for "/" itself. */
    size_t o = 1;
    const char *p = in;
    out[0] = '/';
    while (*p) {
        const char *seg;
        size_t n;
        while (*p == '/')
            p++;
        seg = p;
        while (*p && *p != '/')
            p++;
        n = (size_t)(p - seg);
        if (n == 0 || (n == 1 && seg[0] == '.'))
            continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (o > 1) {
                o--;                        /* drop the last segment */
                while (o > 1 && out[o - 1] != '/')
                    o--;
                if (o > 1)
                    o--;                    /* and its separator */
                else
                    o = 1;
            }
            continue;
        }
        if (o > 1)
            out[o++] = '/';
        memcpy(out + o, seg, n);
        o += n;
    }
    out[o] = 0;
}

int bz_url_parse(bz_url *u, const char *s, char *err, size_t errlen)
{
    const char *p, *auth, *aend, *h, *hend, *path, *query;
    char *joined, *clean;
    size_t plen, need;
    memset(u, 0, sizeof *u);
    if (strncmp(s, "https://", 8) == 0) {
        u->tls = 1;
        p = s + 8;
    } else if (strncmp(s, "http://", 7) == 0) {
        p = s + 7;
    } else {
        snprintf(err, errlen, "HUB_URL must start with http:// or https:// (e.g. http://hub.example.com:8090)");
        return 0;
    }
    auth = p;
    aend = auth + strcspn(auth, "/?#");
    /* drop any userinfo */
    h = auth;
    {
        const char *at = NULL, *q;
        for (q = auth; q < aend; q++)
            if (*q == '@')
                at = q;
        if (at)
            h = at + 1;
    }
    if (*h == '[') {
        hend = memchr(h, ']', (size_t)(aend - h));
        if (!hend || hend == h + 1) {
            snprintf(err, errlen, "HUB_URL has an invalid IPv6 host");
            return 0;
        }
        if (!bz_strlcpyn(u->host, sizeof u->host, h + 1, (size_t)(hend - h - 1)))
            goto toolong;
        hend++;
    } else {
        hend = h;
        while (hend < aend && *hend != ':')
            hend++;
        if (!bz_strlcpyn(u->host, sizeof u->host, h, (size_t)(hend - h)))
            goto toolong;
    }
    if (!u->host[0]) {
        snprintf(err, errlen, "HUB_URL must include a host (e.g. http://hub.example.com:8090)");
        return 0;
    }
    if (hend < aend && *hend == ':') {
        const char *d;
        long port;
        if (hend + 1 == aend) {
            strcpy(u->port, u->tls ? "443" : "80");
        } else {
            for (d = hend + 1; d < aend; d++)
                if (*d < '0' || *d > '9') {
                    snprintf(err, errlen, "HUB_URL has an invalid port");
                    return 0;
                }
            if (aend - hend - 1 > 5 || (port = strtol(hend + 1, NULL, 10)) < 1 || port > 65535) {
                snprintf(err, errlen, "HUB_URL has an invalid port");
                return 0;
            }
            sprintf(u->port, "%ld", port);
        }
    } else if (hend != aend) {
        snprintf(err, errlen, "HUB_URL has an invalid host");
        return 0;
    } else {
        strcpy(u->port, u->tls ? "443" : "80");
    }
    if (!bz_strlcpyn(u->host_header, sizeof u->host_header, h, (size_t)(aend - h)))
        goto toolong;
    path = aend;
    plen = strcspn(path, "?#");
    query = path[plen] == '?' ? path + plen : NULL;
    need = plen + sizeof "/api/beszel/agent-connect" + 2;
    joined = malloc(need);
    clean = malloc(need + 1);
    if (!joined || !clean) {
        free(joined);
        free(clean);
        snprintf(err, errlen, "out of memory");
        return 0;
    }
    memcpy(joined, path, plen);
    strcpy(joined + plen, "/api/beszel/agent-connect");
    bz_path_clean(clean, joined);
    free(joined);
    if (!bz_strlcpy(u->target, clean, sizeof u->target)) {
        free(clean);
        goto toolong;
    }
    free(clean);
    if (query) {
        size_t qn = strcspn(query, "#");
        size_t tl = strlen(u->target);
        if (tl + qn >= sizeof u->target)
            goto toolong;
        memcpy(u->target + tl, query, qn);
        u->target[tl + qn] = 0;
    }
    return 1;
toolong:
    snprintf(err, errlen, "HUB_URL is too long");
    return 0;
}
