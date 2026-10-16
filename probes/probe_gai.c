/* probe_gai.c - list what getaddrinfo() returns for a host (SOCK_STREAM hints),
   and try socket() on each entry: which one gives EPROTOTYPE (NOTES.md 45)?
   VMS TCP/IP answers each address twice, TCP (6) and SCTP (132); with "tcp"
   the hints ask for IPPROTO_TCP as well.
   Usage: probe_gai <host> <port> [tcp] */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct addrinfo hints, *res, *ai;
    char host[64];
    int rc, n = 0, fd;
    if (argc < 3) {
        fprintf(stderr, "usage: probe_gai <host> <port> [tcp]\n");
        return 1;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (argc > 3 && strcmp(argv[3], "tcp") == 0)
        hints.ai_protocol = IPPROTO_TCP;
    rc = getaddrinfo(argv[1], argv[2], &hints, &res);
    if (rc != 0) {
        printf("getaddrinfo: %s\n", gai_strerror(rc));
        return 1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, host, sizeof host, NULL, 0, NI_NUMERICHOST) != 0)
            strcpy(host, "?");
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        printf("%d: family %d socktype %d protocol %d addrlen %d flags %d %s  socket: %s\n", n++,
               ai->ai_family, ai->ai_socktype, ai->ai_protocol, (int)ai->ai_addrlen, ai->ai_flags, host,
               fd < 0 ? strerror(errno) : "ok");
        if (fd >= 0)
            close(fd);
    }
    freeaddrinfo(res);
    return 0;
}
