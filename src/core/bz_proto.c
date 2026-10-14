/* bz_proto.c - hub requests and agent replies (see bz_proto.h). */
#include <stdio.h>
#include <string.h>

#include "bz_cbor.h"
#include "bz_proto.h"

int bz_request_parse(struct bz_request *r, const unsigned char *p, size_t n)
{
    bz_cbr c;
    uint64_t entries, i, key, v;
    int have_action = 0;
    memset(r, 0, sizeof *r);
    bz_cbr_init(&c, p, n);
    if (!bz_cbr_map(&c, &entries))
        return 0;
    for (i = 0; i < entries; i++) {
        if (bz_cbr_peek(&c) != BZ_CB_UINT) {
            if (!bz_cbr_skip(&c) || !bz_cbr_skip(&c))   /* a key we don't know */
                return 0;
            continue;
        }
        if (!bz_cbr_uint(&c, &key))
            return 0;
        if (key == 0) {
            if (!bz_cbr_uint(&c, &r->action))
                return 0;
            have_action = 1;
        } else if (key == 1) {
            size_t start = c.pos;
            if (!bz_cbr_skip(&c))
                return 0;
            r->data = p + start;
            r->data_len = c.pos - start;
        } else if (key == 2) {
            if (bz_cbr_is_null(&c))
                continue;
            if (!bz_cbr_uint(&c, &v) || v > 0xFFFFFFFFu)
                return 0;
            r->has_id = 1;
            r->id = (uint32_t)v;
        } else if (!bz_cbr_skip(&c)) {
            return 0;
        }
    }
    return have_action && c.pos == n && !c.err;
}

int bz_fingerprint_request(const struct bz_request *r, const unsigned char **sig, size_t *sig_len,
                           int *need_sysinfo)
{
    bz_cbr c;
    uint64_t entries, i, key;
    *sig = NULL;
    *sig_len = 0;
    *need_sysinfo = 0;
    if (!r->data)
        return 0;
    bz_cbr_init(&c, r->data, r->data_len);
    if (!bz_cbr_map(&c, &entries))
        return 0;
    for (i = 0; i < entries; i++) {
        if (bz_cbr_peek(&c) != BZ_CB_UINT) {
            if (!bz_cbr_skip(&c) || !bz_cbr_skip(&c))
                return 0;
            continue;
        }
        if (!bz_cbr_uint(&c, &key))
            return 0;
        if (key == 0) {
            if (!bz_cbr_bytes(&c, sig, sig_len))
                return 0;
        } else if (key == 1) {
            if (!bz_cbr_bool(&c, need_sysinfo))
                return 0;
        } else if (!bz_cbr_skip(&c)) {
            return 0;
        }
    }
    return *sig != NULL && !c.err;
}

void bz_data_options(const struct bz_request *r, unsigned *cache_ms, int *include_details)
{
    bz_cbr c;
    uint64_t entries, i, key, v;
    int b;
    unsigned cm = 0;
    int inc = 0;
    *cache_ms = 0;
    *include_details = 0;
    if (!r->data)
        return;
    bz_cbr_init(&c, r->data, r->data_len);
    if (!bz_cbr_map(&c, &entries))
        return;
    for (i = 0; i < entries; i++) {
        if (bz_cbr_peek(&c) != BZ_CB_UINT) {
            if (!bz_cbr_skip(&c) || !bz_cbr_skip(&c))
                return;
            continue;
        }
        if (!bz_cbr_uint(&c, &key))
            return;
        if (key == 0) {
            if (!bz_cbr_uint(&c, &v) || v > 0xFFFF)     /* uint16 in Go */
                return;
            cm = (unsigned)v;
        } else if (key == 1) {
            if (!bz_cbr_bool(&c, &b))
                return;
            inc = b;
        } else if (!bz_cbr_skip(&c)) {
            return;
        }
    }
    if (!c.err) {
        *cache_ms = cm;
        *include_details = inc;
    }
}

void bz_reply_begin(bz_buf *b, const struct bz_request *r, size_t extra)
{
    if (!r->has_id)
        return;
    bz_cb_map(b, 1 + extra);
    bz_cb_uint(b, 0);
    bz_cb_uint(b, r->id);
}

void bz_reply_fingerprint(bz_buf *b, const struct bz_request *r, const char *fp,
                          const char *hostname, const char *port, const char *name)
{
    /* FingerprintResponse {0: fp, 1: hostname, 2: port, 3: name}, omitzero */
    size_t n = 1 + (hostname && *hostname) + (port && *port) + (name && *name);
    bz_reply_begin(b, r, 1);
    if (r->has_id)
        bz_cb_uint(b, 2);           /* legacy Fingerprint field (PROTOCOL.md 2.4) */
    bz_cb_map(b, n);
    bz_cb_uint(b, 0);
    bz_cb_text(b, fp);
    if (hostname && *hostname) { bz_cb_uint(b, 1); bz_cb_text(b, hostname); }
    if (port && *port) { bz_cb_uint(b, 2); bz_cb_text(b, port); }
    if (name && *name) { bz_cb_uint(b, 3); bz_cb_text(b, name); }
}

void bz_reply_error(bz_buf *b, const struct bz_request *r, const char *msg)
{
    if (!r->has_id)
        return;                     /* a legacy hub has no error field to read */
    bz_reply_begin(b, r, 1);
    bz_cb_uint(b, 3);
    bz_cb_text(b, msg);
}

void bz_reply_unsupported(bz_buf *b, const struct bz_request *r)
{
    /* PROTOCOL.md 3.3: what upstream sends on a host without the feature,
       wherever that is silent on the hub */
    if (!r->has_id)
        return;
    switch ((int)r->action) {
    case BZ_ACT_GET_SMART_DATA:             /* {0:id}: captured from upstream */
        bz_cb_map(b, 1);
        bz_cb_uint(b, 0);
        bz_cb_uint(b, r->id);
        break;
    case BZ_ACT_SYNC_NETWORK_MONITORS:      /* zero SyncResponse: captured */
        bz_reply_begin(b, r, 1);
        bz_cb_uint(b, 7);
        bz_cb_map(b, 1);
        bz_cb_uint(b, 0);
        bz_cb_map(b, 4);
        bz_cb_uint(b, 8); bz_cb_uint(b, 0);
        bz_cb_uint(b, 10); bz_cb_uint(b, 0);
        bz_cb_uint(b, 11); bz_cb_uint(b, 0);
        bz_cb_uint(b, 12); bz_cb_uint(b, 0);
        break;
    case BZ_ACT_GET_ZFS_DATA:               /* ZfsData{Complete: true}, no backends */
        bz_reply_begin(b, r, 1);
        bz_cb_uint(b, 7);
        bz_cb_map(b, 1);
        bz_cb_text(b, "complete");
        bz_cb_bool(b, 1);
        break;
    case BZ_ACT_GET_CONTAINER_LOGS:
    case BZ_ACT_GET_CONTAINER_INFO:         /* String "" */
        bz_reply_begin(b, r, 1);
        bz_cb_uint(b, 4);
        bz_cb_text(b, "");
        break;
    case BZ_ACT_GET_PACKAGE_UPDATES:        /* PackageUpdates{} = {3: null} */
        bz_reply_begin(b, r, 1);
        bz_cb_uint(b, 7);
        bz_cb_map(b, 1);
        bz_cb_uint(b, 3);
        bz_cb_null(b);
        break;
    case BZ_ACT_GET_SYSTEMD_INFO:
    case BZ_ACT_GET_SYSTEMD_LOGS:
        bz_reply_error(b, r, "unsupported operation");
        break;
    default:
        {
            char msg[48];
            snprintf(msg, sizeof msg, "unknown action: %lu",
                     (unsigned long)(r->action > 0xFFFFFFFFu ? 0xFFFFFFFFu : r->action));
            bz_reply_error(b, r, msg);
        }
        break;
    }
}
