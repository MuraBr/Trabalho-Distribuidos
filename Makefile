CC ?= gcc
CFLAGS ?= -std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
CRYPTO_LIB ?= $(shell pkg-config --libs libcrypto 2>/dev/null || echo -Wl,-l:libcrypto.so.3)
LZ4_LIB ?= $(shell pkg-config --libs liblz4 2>/dev/null || echo -Wl,-l:liblz4.so.1)
LDLIBS ?= -pthread $(CRYPTO_LIB) -lz

BIN_DIR ?= bin
CPPFLAGS ?=
ifneq ($(wildcard .deps/usr/include/openssl/evp.h),)
CPPFLAGS += -I$(CURDIR)/.deps/usr/include -I$(CURDIR)/.deps/usr/include/x86_64-linux-gnu
endif
LDFLAGS ?=

.PHONY: all clean test test-aluno2 test-c2 deps

# Headers oficiais locais, sem sudo; somente para Debian/Ubuntu sem pacotes -dev.
deps:
	mkdir -p .deps/packages
	cd .deps/packages && apt-get download libssl-dev liblz4-dev
	cd .deps/packages && dpkg-deb -x ./libssl-dev_*.deb .. && dpkg-deb -x ./liblz4-dev_*.deb ..

all: $(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/node $(BIN_DIR)/client

$(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer $(BIN_DIR)/test_transfer_negative $(BIN_DIR)/test_storage_atomic: $(wildcard *.h)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)


$(BIN_DIR)/superpeer: app_config.c app_config.h transfer_types.h remote_error.h network.c network.h concurrent_server.c concurrent_server.h protocol.c protocol.h transfer_protocol.c transfer_protocol.h chord.c chord.h chord_network.c chord_network.h \
                      node.c node.h common.h metadata.c metadata.h directory.c directory.h \
                      superpeer.c superpeer.h rpc.c rpc.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread app_config.c network.c concurrent_server.c protocol.c node.c chord.c chord_network.c \
		transfer_protocol.c rpc.c metadata.c directory.c superpeer.c \
		-o $@ $(LDLIBS)

$(BIN_DIR)/peer: app_config.c app_config.h transfer_types.h remote_error.h network.c network.h concurrent_server.c concurrent_server.h protocol.c protocol.h transfer_protocol.c transfer_protocol.h \
                 compression.c compression.h content.c content.h storage.c storage.h rpc.c rpc.h \
                 node.c node.h metadata.c metadata.h \
                 file_client.c file_client.h local_control.c local_control.h peer.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. app_config.c network.c concurrent_server.c protocol.c transfer_protocol.c compression.c content.c \
		storage.c rpc.c node.c metadata.c file_client.c local_control.c peer.c \
		-o $@ $(LDLIBS) $(LZ4_LIB)

$(BIN_DIR)/node: $(BIN_DIR)/superpeer Makefile | $(BIN_DIR)
	ln -sf superpeer $@

$(BIN_DIR)/client: $(BIN_DIR)/peer Makefile | $(BIN_DIR)
	ln -sf peer $@

test: all test-aluno2
	$(MAKE) -C tests/c1
	./tests/c1/test_protocol
	$(MAKE) test-c2

clean:
	rm -f $(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/node $(BIN_DIR)/client $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer $(BIN_DIR)/test_transfer_negative $(BIN_DIR)/test_storage_atomic $(BIN_DIR)/test_chord
	$(MAKE) -C tests/c1 clean

# API local do aluno 2: não depende de sockets nem altera executáveis versionados.
$(BIN_DIR)/test_metadata: metadata.c metadata.h node.h common.h tests/c2/test_metadata.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. metadata.c tests/c2/test_metadata.c -o $@ $(CRYPTO_LIB)

$(BIN_DIR)/test_node_superpeer: node.c node.h superpeer.c superpeer.h common.h test_node_superpeer.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -DSUPERPEER_MEMBERSHIP_ONLY -pthread -I. node.c superpeer.c test_node_superpeer.c -o $@ $(CRYPTO_LIB)

test-aluno2: $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer
	$(BIN_DIR)/test_node_superpeer
	$(BIN_DIR)/test_metadata
	$(MAKE) test-c3-local

$(BIN_DIR)/test_chord: chord.c chord.h node.c node.h tests/c3/test_chord.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Werror -pthread -I. chord.c node.c tests/c3/test_chord.c -o $@ $(CRYPTO_LIB)

.PHONY: test-c3-local
test-c3-local: $(BIN_DIR)/test_chord
	$(BIN_DIR)/test_chord

$(BIN_DIR)/test_transfer_negative: network.c protocol.c transfer_protocol.c compression.c content.c storage.c metadata.c tests/c2/test_transfer_negative.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. network.c protocol.c transfer_protocol.c compression.c content.c storage.c metadata.c tests/c2/test_transfer_negative.c -o $@ $(LDLIBS) $(LZ4_LIB)

$(BIN_DIR)/test_storage_atomic: storage.c content.c compression.c metadata.c tests/c2/test_storage_atomic.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. storage.c content.c compression.c metadata.c tests/c2/test_storage_atomic.c -o $@ $(LDLIBS) $(LZ4_LIB)

test-c2: $(BIN_DIR)/test_transfer_negative $(BIN_DIR)/test_storage_atomic
	$(BIN_DIR)/test_transfer_negative
	$(BIN_DIR)/test_storage_atomic
