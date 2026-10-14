/* bz_conf.c - agent configuration (see bz_conf.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_conf.h"
#include "bz_text.h"

/* Upstream's names that this agent implements; nothing else is accepted. */
static const char *const known[] = {
    "HUB_URL", "TOKEN", "TOKEN_FILE", "KEY", "KEY_FILE", "CA_CERT_FILE",
    "SYSTEM_NAME", "LISTEN", "PORT", "FILESYSTEM", "EXTRA_FILESYSTEMS", "NICS",
    "LOG_LEVEL", "DATA_DIR", "EXIT_ON_DNS_ERROR", NULL
};

int bz_conf_known(const char *name)
{
    int i;
    for (i = 0; known[i]; i++)
        if (strcmp(known[i], name) == 0)
            return 1;
    return 0;
}

void bz_conf_init(bz_conf *c, bz_conf_override_fn fn, void *ctx)
{
    memset(c, 0, sizeof *c);
    c->override = fn;
    c->override_ctx = ctx;
    bz_buf_init(&c->scratch);
}

void bz_conf_free(bz_conf *c)
{
    int i;
    for (i = 0; i < c->n; i++) {
        free(c->name[i]);
        if (c->value[i]) {
            /* the token and keys are secrets: clear them before freeing */
            memset(c->value[i], 0, strlen(c->value[i]));
            free(c->value[i]);
        }
    }
    if (c->scratch.p)
        memset(c->scratch.p, 0, c->scratch.cap);
    bz_buf_free(&c->scratch);
    c->n = 0;
}

static char *dupn(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

int bz_conf_parse(bz_conf *c, const char *text, size_t len, char *err, size_t errlen)
{
    size_t i = 0;
    int line = 0;
    while (i < len) {
        const char *s = text + i, *eq, *v;
        size_t n = 0, nn, vn;
        char name[32];
        int j;
        while (i + n < len && text[i + n] != '\n')
            n++;
        i += n + 1;
        line++;
        bz_trim(&s, &n);
        if (n == 0 || s[0] == '#')
            continue;
        eq = memchr(s, '=', n);
        if (!eq) {
            snprintf(err, errlen, "line %d: expected NAME=value", line);
            return 0;
        }
        nn = (size_t)(eq - s);
        v = eq + 1;
        vn = n - nn - 1;
        bz_trim(&s, &nn);
        bz_trim(&v, &vn);
        if (vn >= 2 && v[0] == '"' && v[vn - 1] == '"') {
            v++;
            vn -= 2;
        }
        if (nn == 0 || nn >= sizeof name || !bz_strlcpyn(name, sizeof name, s, nn) ||
            !bz_conf_known(name)) {
            snprintf(err, errlen, "line %d: unknown setting \"%.*s\"", line,
                     (int)(nn > 40 ? 40 : nn), s);
            return 0;
        }
        for (j = 0; j < c->n; j++)
            if (strcmp(c->name[j], name) == 0)
                break;
        if (j < c->n) {
            snprintf(err, errlen, "line %d: %s is set twice", line, name);
            return 0;
        }
        if (c->n == BZ_CONF_MAX) {
            snprintf(err, errlen, "line %d: too many settings", line);
            return 0;
        }
        c->name[c->n] = dupn(name, strlen(name));
        c->value[c->n] = dupn(v, vn);
        if (!c->name[c->n] || !c->value[c->n]) {
            snprintf(err, errlen, "out of memory");
            return 0;
        }
        c->n++;
    }
    return 1;
}

const char *bz_conf_get(bz_conf *c, const char *name)
{
    int i;
    if (c->override) {
        bz_buf_clear(&c->scratch);
        if (c->override(c->override_ctx, name, &c->scratch) && !c->scratch.oom)
            return c->scratch.p ? (const char *)c->scratch.p : "";
    }
    for (i = 0; i < c->n; i++)
        if (strcmp(c->name[i], name) == 0)
            return c->value[i];
    return NULL;
}

int bz_conf_token_file(const char *text, size_t len, bz_buf *token)
{
    size_t i = 0;
    int found = 0;
    bz_buf_clear(token);
    while (i < len) {
        const char *s = text + i;
        size_t n = 0;
        while (i + n < len && text[i + n] != '\n')
            n++;
        i += n + 1;
        bz_trim(&s, &n);
        if (n == 0 || s[0] == '#')
            continue;
        if (found)
            return 0;
        bz_buf_put(token, s, n);
        found = 1;
    }
    return !token->oom;
}
