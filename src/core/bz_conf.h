/* bz_conf.h - agent configuration (DECISIONS.md D5, PROTOCOL.md 7).
 *
 * A file of NAME=value lines with upstream's variable names.  Blank lines and
 * lines starting with '#' are ignored; the value may be wrapped in double
 * quotes.  An unknown name or a malformed line is an error, reported with its
 * line number, so a typo never fails silently.  A platform override (logical
 * name BESZEL$<NAME> on OpenVMS, environment BESZEL_AGENT_<NAME> on Linux)
 * takes precedence over the file, as upstream's BESZEL_AGENT_ prefix does.
 */
#ifndef BZ_CONF_H
#define BZ_CONF_H

#include <stddef.h>

#include "bz_buf.h"

#define BZ_CONF_MAX 32

typedef int (*bz_conf_override_fn)(void *ctx, const char *name, bz_buf *out);

typedef struct bz_conf {
    int n;
    char *name[BZ_CONF_MAX];
    char *value[BZ_CONF_MAX];
    bz_conf_override_fn override;
    void *override_ctx;
    bz_buf scratch;                         /* the last override value returned */
} bz_conf;

void bz_conf_init(bz_conf *c, bz_conf_override_fn fn, void *ctx);
void bz_conf_free(bz_conf *c);
/* parse a whole file's text; on error returns 0 and writes a message to err */
int bz_conf_parse(bz_conf *c, const char *text, size_t len, char *err, size_t errlen);
/* the value, or NULL if unset; valid until the next bz_conf_get */
const char *bz_conf_get(bz_conf *c, const char *name);
int bz_conf_known(const char *name);

/* TOKEN_FILE contents: one non-comment line; 0 if more than one (PROTOCOL.md 7) */
int bz_conf_token_file(const char *text, size_t len, bz_buf *token);

#endif
