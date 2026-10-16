/* bz_version.h - our release and the upstream protocol version we speak. */
#ifndef BZ_VERSION_H
#define BZ_VERSION_H

/* X-Beszel and Info.AgentVersion: the pinned upstream release (DECISIONS.md D7) */
#define BZ_PROTOCOL_VERSION "0.21.0"
/* this port: <upstream>-vms<N>, as the sibling ports tag their releases */
#define BZ_PORT_VERSION "0.21.0-vms1"
/* the PCSI kit version: upstream 0.21.0 with our level as the ECO (as vms-curl: V8.22-0E2) */
#define BZ_PCSI_VERSION "V0.21-0E1"

#endif
