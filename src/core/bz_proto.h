/* bz_proto.h - hub requests and agent replies (PROTOCOL.md 2-3).
 *
 * HubRequest   {0: action, 1: data, 2: id}
 * AgentResponse{0: id, 1: SystemData, 2: Fingerprint, 3: Error, 4: String, 7: Data}
 * 0.21.0 still answers the handshake in key 2 and GetData in key 1.
 */
#ifndef BZ_PROTO_H
#define BZ_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "bz_buf.h"

enum {
    BZ_ACT_GET_DATA = 0,
    BZ_ACT_CHECK_FINGERPRINT = 1,
    BZ_ACT_GET_CONTAINER_LOGS = 2,
    BZ_ACT_GET_CONTAINER_INFO = 3,
    BZ_ACT_GET_SMART_DATA = 4,
    BZ_ACT_GET_SYSTEMD_INFO = 5,
    BZ_ACT_GET_ZFS_DATA = 6,
    BZ_ACT_SYNC_NETWORK_MONITORS = 7,
    BZ_ACT_GET_PACKAGE_UPDATES = 8,
    BZ_ACT_GET_SYSTEMD_LOGS = 9
};

struct bz_request {
    uint64_t action;
    int has_id;
    uint32_t id;
    const unsigned char *data;  /* the raw CBOR item of key 1, or NULL */
    size_t data_len;
};

int bz_request_parse(struct bz_request *r, const unsigned char *p, size_t n);

/* FingerprintRequest {0: signature, 1: needSysInfo} */
int bz_fingerprint_request(const struct bz_request *r, const unsigned char **sig, size_t *sig_len,
                           int *need_sysinfo);
/* DataRequestOptions {0: cacheTimeMs, 1: includeDetails}; zero options on any
   decode error, as upstream (agent/handlers.go:103-104) */
void bz_data_options(const struct bz_request *r, unsigned *cache_ms, int *include_details);

/* Start a reply: {0: id, <key>: ...}.  With no id, a legacy hub expects the
   bare payload, so nothing is written and the caller writes the payload only.
   `extra` is the number of entries after the id. */
void bz_reply_begin(bz_buf *b, const struct bz_request *r, size_t extra);
void bz_reply_fingerprint(bz_buf *b, const struct bz_request *r, const char *fp,
                          const char *hostname, const char *port, const char *name);
void bz_reply_error(bz_buf *b, const struct bz_request *r, const char *msg);
/* the silent replies for features OpenVMS doesn't have (PROTOCOL.md 3.3) */
void bz_reply_unsupported(bz_buf *b, const struct bz_request *r);

#endif
