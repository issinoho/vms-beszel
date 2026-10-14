/* probe_who.c - Phase 3: who is a process really?  Username, UIC, authorized
 * and current privilege masks, as $GETJPI reports them. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <starlet.h>
#include <jpidef.h>
#include <efndef.h>

struct item { unsigned short len, code; void *buf; unsigned short *retlen; };
struct iosb { unsigned short st, cnt; unsigned int dev; };

int main(void)
{
    char user[13];
    unsigned int uic = 0;
    uint64_t auth = 0, cur = 0, rl[4] = { 0, 0, 0, 0 };
    struct item il[5];
    struct iosb iosb;
    int st;
    memset(il, 0, sizeof il);
    memset(user, 0, sizeof user);
    il[0].len = 12; il[0].code = JPI$_USERNAME; il[0].buf = user; il[0].retlen = (unsigned short *)&rl[0];
    il[1].len = 4; il[1].code = JPI$_UIC; il[1].buf = &uic; il[1].retlen = (unsigned short *)&rl[1];
    il[2].len = 8; il[2].code = JPI$_AUTHPRIV; il[2].buf = &auth; il[2].retlen = (unsigned short *)&rl[2];
    il[3].len = 8; il[3].code = JPI$_CURPRIV; il[3].buf = &cur; il[3].retlen = (unsigned short *)&rl[3];
    st = sys$getjpiw(EFN$C_ENF, 0, 0, il, (void *)&iosb, 0, 0);
    printf("WHO st=%d user=%s uic=[%o,%o] authpriv=%016llx curpriv=%016llx\n", st, user,
           uic >> 16, uic & 0xFFFF, (unsigned long long)auth, (unsigned long long)cur);
    return 1;
}
