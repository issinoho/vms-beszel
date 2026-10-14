# vms-beszel - Linux build: the agent on the stub platform, and the tests.
#
# OpenVMS builds with build_vms.com from the same sources (DECISIONS.md D2).
# -std=c99 -pedantic is deliberate: VSI C is the target compiler, so C11 must
# not creep in.  Do not weaken these flags.

CC      ?= cc
CFLAGS  ?= -std=c99 -pedantic -Wall -Wextra -Wshadow -O2 -g
# glibc hides getaddrinfo, localtime_r and friends under strict -std=c99.
CFLAGS  += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
# Includes are by bare name: VSI C does not resolve "../" in #include paths.
CFLAGS  += -Isrc/core -Isrc/plat
LDLIBS  ?= -lssl -lcrypto -lm

CORE = src/core/bz_buf.c src/core/bz_cbor.c src/core/bz_text.c src/core/bz_conf.c \
       src/core/bz_key.c src/core/bz_url.c src/core/bz_ws.c src/core/bz_proto.c \
       src/core/bz_stats.c src/core/bz_agent.c
OSSL = src/plat/bz_crypto_ossl.c src/plat/bz_net_ossl.c
STUB = src/plat/stub/bz_plat_stub.c
HDRS = $(wildcard src/core/*.h src/plat/*.h)

B = build
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

all: $(B)/beszel_agent $(B)/test_core

$(B):
	mkdir -p $(B)

$(B)/beszel_agent: $(CORE) $(OSSL) $(STUB) src/main/bz_main.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(B)/beszel_agent-asan: $(CORE) $(OSSL) $(STUB) src/main/bz_main.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(SAN) -o $@ $(filter %.c,$^) $(LDLIBS)

$(B)/test_core: $(CORE) src/plat/bz_crypto_ossl.c tests/unit/test_core.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(B)/test_core-asan: $(CORE) src/plat/bz_crypto_ossl.c tests/unit/test_core.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(SAN) -o $@ $(filter %.c,$^) $(LDLIBS)

$(B)/fuzz_core: $(CORE) src/plat/bz_crypto_ossl.c tests/unit/fuzz_core.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(SAN) -o $@ $(filter %.c,$^) $(LDLIBS)

test: $(B)/test_core
	$(B)/test_core

test-asan: $(B)/test_core-asan
	$(B)/test_core-asan

fuzz: $(B)/fuzz_core
	$(B)/fuzz_core 300000

clean:
	rm -rf $(B)

.PHONY: all test test-asan fuzz clean

$(B)/gen_fixtures: $(CORE) src/plat/bz_crypto_ossl.c tests/conformance/gen_fixtures.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

conformance: $(B)/gen_fixtures
	tools/conformance.sh

.PHONY: conformance
