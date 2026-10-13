# vms-beszel

A native OpenVMS agent for [beszel](https://github.com/henrygd/beszel), the lightweight
server monitor. OpenVMS x86-64 and Integrity (IA64) systems report CPU, memory, swap,
disk, network and load to an unmodified beszel hub, with charts, history and alerts,
alongside the hub's other systems.

**Status:** planning. See [docs/PLAN.md](docs/PLAN.md).

- Implements the agent protocol of beszel **0.21.0** (WebSocket transport).
- Written in C for VSI C; uses VSI's SSL3 kit for TLS and for verifying the hub's key.
- Part of the [vms-* family](https://openvms.issinoho.com).
