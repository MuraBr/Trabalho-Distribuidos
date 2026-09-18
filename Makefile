CC ?= gcc
CFLAGS ?= -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
CRYPTO_LIB ?= -Wl,-l:libcrypto.so.3
LDLIBS ?= -pthread $(CRYPTO_LIB) -lz

BIN_DIR := bin

.PHONY: all clean test

all: $(BIN_DIR)/node $(BIN_DIR)/client

$(BIN_DIR):
	mkdir -p $(BIN_DIR)


$(BIN_DIR)/node: network.c network.h protocol.c protocol.h \
                 node.c node.h common.h \
                 superpeer.c superpeer.h peer.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread network.c protocol.c node.c \
		superpeer.c peer.c \
		-o $@ $(LDLIBS)

$(BIN_DIR)/client: $(BIN_DIR)/node Makefile | $(BIN_DIR)
	ln -sf node $@

test: all
	$(MAKE) -C tests/c1
	./tests/c1/test_protocol

clean:
	rm -f $(BIN_DIR)/node $(BIN_DIR)/client
	$(MAKE) -C tests/c1 clean
