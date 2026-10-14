/* bz_platform.h - the whole operating-system surface of the agent.
 *
 * No OpenVMS (or Linux) types appear here.  Implementations:
 *   bz_net_ossl.c, bz_crypto_ossl.c   sockets, TLS and crypto on OpenSSL
 *                                     (SSL3 on OpenVMS), shared by all platforms
 *   vms/bz_plat_vms.c                 system services (Phase 2)
 *   stub/bz_plat_stub.c               Linux: scripted counters for tests
 */
#ifndef BZ_PLATFORM_H
#define BZ_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "bz_buf.h"
#include "bz_stats.h"
#include "bz_sys.h"

/* ---- time ---- */
uint64_t bz_plat_now_ms(void);          /* monotonic enough for intervals */
void bz_plat_sleep_ms(unsigned ms);

/* ---- files ---- */
int bz_plat_read_file(const char *path, bz_buf *out);
/* atomically replace a small file, readable by the owner only */
int bz_plat_write_file(const char *path, const void *p, size_t n);
int bz_plat_exists(const char *path);
int bz_plat_remove(const char *path);
/* 1 if group or world can read it (the config holds the token) */
int bz_plat_file_exposed(const char *path);
/* "<dir>/<name>" in the platform's syntax */
void bz_plat_data_path(char *out, size_t cap, const char *dir, const char *name);
const char *bz_plat_default_config(void);
const char *bz_plat_default_data_dir(void);
const char *bz_plat_default_ca_file(void);  /* NULL: the system store */
const char *bz_plat_default_root_fs(void);

/* ---- configuration overrides (DECISIONS.md D5) ---- */
int bz_plat_conf_override(void *ctx, const char *name, bz_buf *out);

/* ---- logging ---- */
void bz_plat_log(int level, const char *msg);

/* ---- the system ---- */
int bz_plat_init(void);
int bz_plat_identity(struct bz_identity *id);
int bz_plat_sample(struct bz_sample *s);
void bz_plat_fs_usage(const char *dev, struct bz_fs_usage *u);
double bz_plat_runq(void);
int bz_plat_stop_requested(void);

/* ---- network ---- */
typedef struct bz_sock bz_sock;
bz_sock *bz_net_open(const char *host, const char *port, int tls, const char *ca_file,
                     unsigned timeout_ms, char *err, size_t errlen);
int bz_net_send(bz_sock *s, const void *p, size_t n);     /* all or nothing: 1/0 */
/* >0 bytes, 0 the peer closed, -1 error, -2 nothing yet (a partial TLS record) */
long bz_net_recv(bz_sock *s, void *p, size_t cap);
/* 1 readable, 0 timeout, -1 error */
int bz_net_wait(bz_sock *s, unsigned timeout_ms);
void bz_net_close(bz_sock *s);

#endif
