/* bz_launch.c - start the agent's detached process as its own account (D12, D14).
 *
 *   beszel_launch <account> <command-procedure> <log-file> <process-name>
 *
 * Run by BESZEL$STARTUP.COM START when the agent's account isn't the caller's
 * (at boot the caller is SYSTEM).  RUN/DETACHED/UIC can't do this: LOGINOUT
 * keeps the creator's username and UAF record, so the process gets the
 * creator's authorized privileges (NOTES.md 36).  Instead this assumes the
 * account's persona and creates the LOGINOUT process under it; LOGINOUT then
 * logs in as the account, with its UIC, its quotas and only its authorized
 * privileges.
 *
 * Needs IMPERSONATE (and DETACH), which a system startup has.  Exits with the
 * failing status, so DCL reports it.
 */
#include <stdio.h>
#include <string.h>

#include <descrip.h>
#include <prcdef.h>
#include <ssdef.h>
#include <starlet.h>

#ifndef ISS$C_ID_NATURAL
#define ISS$C_ID_NATURAL 1
#endif

static void dsc(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length = (unsigned short)strlen(s);
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

static void upcase(char *out, size_t cap, const char *in)
{
    size_t i;
    for (i = 0; in[i] && i < cap - 1; i++)
        out[i] = (char)(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
    out[i] = 0;
}

int main(int argc, char **argv)
{
    static const char loginout[] = "SYS$SYSTEM:LOGINOUT.EXE";
    struct dsc$descriptor_s user, image, in, out, name;
    char u[33], n[16];
    unsigned int persona = 0, natural = ISS$C_ID_NATURAL, pid = 0;
    int st, st2;
    if (argc != 5) {
        printf("usage: beszel_launch <account> <command-procedure> <log-file> <process-name>\n");
        return SS$_INSFARG;
    }
    /* DCL lowercases unquoted arguments; account and process names are upper case */
    upcase(u, sizeof u, argv[1]);
    upcase(n, sizeof n, argv[4]);
    dsc(&user, u);
    dsc(&image, loginout);
    dsc(&in, argv[2]);
    dsc(&out, argv[3]);
    dsc(&name, n);
    st = sys$persona_create(&persona, &user, 0, 0, 0);
    if (!(st & 1)) {
        printf("beszel_launch: can't take on the account %s (status %d)\n", u, st);
        return st;
    }
    st = sys$persona_assume(&persona, 0, 0, 0);
    if (st & 1) {
        /* LOGINOUT, detached: it logs in as the persona's account and runs
           the procedure; base priority 4, as an interactive user */
        st = sys$creprc(&pid, &image, &in, &out, &out, 0, 0, &name, 4, 0, 0, PRC$M_DETACH, 0, 0);
        st2 = sys$persona_assume(&natural, 0, 0, 0);
        if ((st & 1) && !(st2 & 1))
            st = st2;
    }
    sys$persona_delete(&persona);
    if (!(st & 1)) {
        printf("beszel_launch: can't create the process as %s (status %d)\n", u, st);
        return st;
    }
    printf("beszel_launch: process %08X started as %s\n", pid, u);
    return SS$_NORMAL;
}
