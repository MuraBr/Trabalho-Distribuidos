CC ?= gcc
CFLAGS ?= -std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
CRYPTO_LIB ?= -Wl,-l:libcrypto.so.3
LDLIBS ?= -pthread $(CRYPTO_LIB) -lz

BIN_DIR := bin

.PHONY: all clean test test-aluno2

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

test: all test-aluno2
	$(MAKE) -C tests/c1
	./tests/c1/test_protocol

clean:
	rm -f $(BIN_DIR)/node $(BIN_DIR)/client $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer
	$(MAKE) -C tests/c1 clean

# API local do aluno 2: não depende de sockets nem altera executáveis versionados.
$(BIN_DIR)/test_metadata: metadata.c metadata.h node.h common.h tests/c2/test_metadata.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread -I. metadata.c tests/c2/test_metadata.c -o $@ $(CRYPTO_LIB)

$(BIN_DIR)/test_node_superpeer: node.c node.h superpeer.c superpeer.h common.h test_node_superpeer.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread -I. node.c superpeer.c test_node_superpeer.c -o $@ $(CRYPTO_LIB)

test-aluno2: $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer
	./$(BIN_DIR)/test_node_superpeer
	./$(BIN_DIR)/test_metadata
