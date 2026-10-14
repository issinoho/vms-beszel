/* bz_key.c - parse the hub's public keys (see bz_key.h). */
#include <stdio.h>
#include <string.h>

#include "bz_buf.h"
#include "bz_key.h"
#include "bz_text.h"

/* next whitespace-separated token; double quotes protect spaces (options) */
static int token(const char **s, size_t *n, const char **t, size_t *tn)
{
    size_t i = 0;
    int q = 0;
    while (i < *n && ((*s)[i] == ' ' || (*s)[i] == '\t'))
        i++;
    *s += i;
    *n -= i;
    if (!*n)
        return 0;
    for (i = 0; i < *n; i++) {
        if ((*s)[i] == '"')
            q = !q;
        else if (!q && ((*s)[i] == ' ' || (*s)[i] == '\t'))
            break;
    }
    *t = *s;
    *tn = i;
    *s += i;
    *n -= i;
    return 1;
}

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

static int is_keytype(const char *t, size_t n)
{
    static const char *const types[] = {
        "ssh-ed25519", "ssh-rsa", "ssh-dss", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521", "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com",
        NULL
    };
    int i;
    for (i = 0; types[i]; i++)
        if (strlen(types[i]) == n && memcmp(types[i], t, n) == 0)
            return 1;
    return 0;
}

int bz_keys_parse(bz_keys *k, const char *text, size_t len, char *err, size_t errlen)
{
    size_t i = 0;
    int line = 0;
    k->n = 0;
    while (i < len) {
        const char *s = text + i, *t = NULL, *b = NULL;
        size_t n = 0, tn = 0, bn = 0;
        bz_buf blob;
        while (i + n < len && text[i + n] != '\n')
            n++;
        i += n + 1;
        line++;
        bz_trim(&s, &n);
        if (n == 0 || s[0] == '#')
            continue;
        /* skip authorized_keys options until the key type */
        while (token(&s, &n, &t, &tn) && !is_keytype(t, tn))
            ;
        if (!t || !is_keytype(t, tn)) {
            snprintf(err, errlen, "key line %d: no key type found", line);
            return 0;
        }
        if (!(tn == 11 && memcmp(t, "ssh-ed25519", 11) == 0)) {
            snprintf(err, errlen, "key line %d: %.*s keys are not supported; the hub key is ssh-ed25519",
                     line, (int)tn, t);
            return 0;
        }
        if (!token(&s, &n, &b, &bn)) {
            snprintf(err, errlen, "key line %d: missing key data", line);
            return 0;
        }
        bz_buf_init(&blob);
        /* SSH wire format: string "ssh-ed25519", string key(32) = 51 bytes */
        if (!bz_b64_decode(&blob, b, bn) || blob.len != 51 || be32(blob.p) != 11 ||
            memcmp(blob.p + 4, "ssh-ed25519", 11) != 0 || be32(blob.p + 15) != 32) {
            bz_buf_free(&blob);
            snprintf(err, errlen, "key line %d: invalid ssh-ed25519 key data", line);
            return 0;
        }
        if (k->n == BZ_KEY_MAX) {
            bz_buf_free(&blob);
            snprintf(err, errlen, "key line %d: more than %d keys", line, BZ_KEY_MAX);
            return 0;
        }
        memcpy(k->ed25519[k->n++], blob.p + 19, 32);
        bz_buf_free(&blob);
    }
    if (k->n == 0) {
        snprintf(err, errlen, "no key provided");
        return 0;
    }
    return 1;
}
