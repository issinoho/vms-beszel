/* gen_fixtures.c - write our encodings of every reply as files, for the Go
 * conformance test that decodes them with upstream's own structs (D6).
 *
 *   gen_fixtures <dir>
 *
 * Synthetic values only (D10).  Fixtures whose names end in "-1" have at most
 * one entry in each string-keyed map, so the test can also compare bytes with
 * upstream's encoder (Go randomises the order of larger maps).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_buf.h"
#include "bz_cbor.h"
#include "bz_proto.h"
#include "bz_stats.h"
#include "bz_text.h"

static const char *dir;

static void put(const char *name, const bz_buf *b)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.cbor", dir, name);
    f = fopen(path, "wb");
    if (!f || fwrite(b->p, 1, b->len, f) != b->len || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
}

static void stats(struct bz_stats_out *o, int many)
{
    memset(o, 0, sizeof *o);
    o->cpu = 16.45;
    o->cpub[0] = 10.2; o->cpub[1] = 6.25; o->cpub[4] = 83.55;
    o->ncores = 2; o->cores[0] = 19; o->cores[1] = 14;
    o->la[0] = 0.52; o->la[1] = 0.31; o->la[2] = 0.2;
    o->mem = 7.09; o->mem_used = 1.03; o->mem_pct = 14.57; o->mem_bc = 0.17;
    o->swap = 1.78; o->swap_used = 0.31;
    o->disk_total = 21.46; o->disk_used = 11.92; o->disk_pct = 55.57;
    strcpy(o->rdn, "System");
    o->bw[0] = 5120; o->bw[1] = 81920;
    o->nni = many ? 2 : 1;
    strcpy(o->ni[0].name, "EIA0");
    o->ni[0].v[0] = 5120; o->ni[0].v[1] = 81920;
    o->ni[0].v[2] = UINT64_C(82151203); o->ni[0].v[3] = UINT64_C(88139130);
    strcpy(o->ni[1].name, "EWB0");
    o->ni[1].v[2] = UINT64_C(524193154); o->ni[1].v[3] = UINT64_C(3927428471);
    o->nefs = many ? 2 : 1;
    strcpy(o->efs[0].key, "Tools");
    o->efs[0].total = 0.94; o->efs[0].used = 0.41;
    strcpy(o->efs[1].key, "DKA300:");
    o->efs[1].total = 16; o->efs[1].used = 11.18;
    o->uptime = 28020;
    o->threads = 2;
}

int main(int argc, char **argv)
{
    static struct bz_stats_out o;
    struct bz_identity id;
    struct bz_request r;
    bz_buf b;
    int a;
    if (argc != 2) {
        fprintf(stderr, "usage: gen_fixtures <dir>\n");
        return EXIT_FAILURE;
    }
    dir = argv[1];
    bz_buf_init(&b);
    memset(&id, 0, sizeof id);
    strcpy(id.hostname, "x86vms.example");
    strcpy(id.kernel, "OpenVMS E9.2-4");
    strcpy(id.os_name, "OpenVMS E9.2-4 (x86-64)");
    strcpy(id.cpu_model, "QEMU Standard PC (Q35 + ICH9, 2009)");
    strcpy(id.arch, "x86_64");
    id.cores = id.threads = 2;
    id.mem_total = UINT64_C(7620722688);

    /* CombinedData alone */
    stats(&o, 0);
    bz_encode_combined(&b, &o, &id, "0.21.0"); put("combined-details-1", &b); bz_buf_clear(&b);
    bz_encode_combined(&b, &o, NULL, "0.21.0"); put("combined-1", &b); bz_buf_clear(&b);
    stats(&o, 1);
    bz_encode_combined(&b, &o, &id, "0.21.0"); put("combined-many", &b); bz_buf_clear(&b);
    {   /* an idle system: everything optional left out */
        struct bz_stats_out z;
        memset(&z, 0, sizeof z);
        bz_encode_combined(&b, &z, NULL, "0.21.0"); put("combined-empty-1", &b); bz_buf_clear(&b);
    }

    /* AgentResponse envelopes, as the agent sends them */
    memset(&r, 0, sizeof r);
    r.has_id = 1;
    r.id = 4242;
    r.action = 0;
    stats(&o, 0);
    bz_reply_begin(&b, &r, 1);
    bz_cb_uint(&b, 1);
    bz_encode_combined(&b, &o, &id, "0.21.0");
    put("reply-getdata-1", &b); bz_buf_clear(&b);

    r.action = 1;
    bz_reply_fingerprint(&b, &r, "9a1dfdd2a3a88e9a701aaa18461b7ab871342721caf26c6f", NULL, NULL, NULL);
    put("reply-fingerprint-1", &b); bz_buf_clear(&b);
    bz_reply_fingerprint(&b, &r, "9a1dfdd2a3a88e9a701aaa18461b7ab871342721caf26c6f", "x86vms", "45876", "VMS x86");
    put("reply-fingerprint-sysinfo-1", &b); bz_buf_clear(&b);

    for (a = 2; a <= 10; a++) {
        static const char *const names[] = { NULL, NULL, "containerlogs", "containerinfo", "smart",
                                             "systemdinfo", "zfs", "sync", "packages", "systemdlogs",
                                             "unknown" };
        char name[64];
        r.action = (uint64_t)a;
        bz_reply_unsupported(&b, &r);
        snprintf(name, sizeof name, "reply-%s-1", names[a]);
        put(name, &b);
        bz_buf_clear(&b);
    }
    bz_buf_free(&b);
    return EXIT_SUCCESS;
}
