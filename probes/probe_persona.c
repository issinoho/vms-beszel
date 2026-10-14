/* probe_persona.c - Phase 3: does a process created while a persona is
 * assumed get that persona's identity (username, UIC, authorized privileges)?
 *   probe_persona <user> <image> <output-file> [<input-file>]
 * Needs IMPERSONATE (a system startup has it).  Creates one detached process
 * named BZPERSONA running <image>, with SYS$OUTPUT to <output-file>.
 */
#include <stdio.h>
#include <string.h>
#include <descrip.h>
#include <prcdef.h>
#include <starlet.h>

static void dsc(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length = (unsigned short)strlen(s);
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

int main(int argc, char **argv)
{
    struct dsc$descriptor_s user, image, out, name, in;
    char up[32];
    size_t i;
    unsigned int h = 0, natural = 1, pid = 0;
    int st;
    if (argc != 4 && argc != 5) {
        printf("usage: probe_persona <user> <image> <output-file> [<input-file>]\n");
        return 1;
    }
    for (i = 0; argv[1][i] && i < sizeof up - 1; i++)       /* DCL lowercased it */
        up[i] = (char)(argv[1][i] >= 'a' && argv[1][i] <= 'z' ? argv[1][i] - 32 : argv[1][i]);
    up[i] = 0;
    dsc(&user, up);
    if (argc == 5)
        dsc(&in, argv[4]);
    dsc(&image, argv[2]);
    dsc(&out, argv[3]);
    dsc(&name, "BZPERSONA");
    st = sys$persona_create(&h, &user, 0, 0, 0);
    printf("persona_create %s: %d\n", argv[1], st);
    if (!(st & 1)) return st;
    st = sys$persona_assume(&h, 0, 0, 0);
    printf("persona_assume: %d\n", st);
    if (!(st & 1)) return st;
    st = sys$creprc(&pid, &image, argc == 5 ? &in : 0, &out, &out, 0, 0, &name, 4, 0, 0, PRC$M_DETACH, 0, 0);
    printf("creprc: %d pid %08x\n", st, pid);
    sys$persona_assume(&natural, 0, 0, 0);
    sys$persona_delete(&h);
    return 1;
}
