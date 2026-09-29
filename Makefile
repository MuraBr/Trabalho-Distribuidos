CC ?= gcc
CFLAGS ?= -std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
CRYPTO_LIB ?= -Wl,-l:libcrypto.so.3
LZ4_LIB ?= -Wl,-l:liblz4.so.1
LDLIBS ?= -pthread $(CRYPTO_LIB) -lz

BIN_DIR := bin

.PHONY: all clean test test-aluno2 test-c2

all: $(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/node $(BIN_DIR)/client

$(BIN_DIR):
	mkdir -p $(BIN_DIR)


$(BIN_DIR)/superpeer: network.c network.h concurrent_server.c concurrent_server.h protocol.c protocol.h transfer_protocol.c transfer_protocol.h \
                      node.c node.h common.h metadata.c metadata.h directory.c directory.h \
                      superpeer.c superpeer.h superpeer_app.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -DSUPERPEER_EXECUTABLE -pthread network.c concurrent_server.c protocol.c node.c \
		transfer_protocol.c metadata.c directory.c superpeer.c superpeer_app.c \
		-o $@ $(LDLIBS)

$(BIN_DIR)/peer: network.c network.h concurrent_server.c concurrent_server.h protocol.c protocol.h transfer_protocol.c transfer_protocol.h \
                 compression.c compression.h content.c content.h storage.c storage.h rpc.c rpc.h \
                 node.c node.h metadata.c metadata.h peer_service.c peer_service.h \
                 file_client.c file_client.h peer.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread -I. network.c concurrent_server.c protocol.c transfer_protocol.c compression.c content.c \
		storage.c rpc.c node.c metadata.c peer_service.c file_client.c peer.c \
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
	rm -f $(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/node $(BIN_DIR)/client $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer $(BIN_DIR)/test_transfer_negative
	$(MAKE) -C tests/c1 clean

# API local do aluno 2: não depende de sockets nem altera executáveis versionados.
$(BIN_DIR)/test_metadata: metadata.c metadata.h node.h common.h tests/c2/test_metadata.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread -I. metadata.c tests/c2/test_metadata.c -o $@ $(CRYPTO_LIB)

$(BIN_DIR)/test_node_superpeer: node.c node.h superpeer.c superpeer.h common.h test_node_superpeer.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread -I. node.c superpeer.c test_node_superpeer.c -o $@ $(CRYPTO_LIB)

test-aluno2: $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer
	./$(BIN_DIR)/test_node_superpeer
	./$(BIN_DIR)/test_metadata

$(BIN_DIR)/test_transfer_negative: network.c protocol.c transfer_protocol.c compression.c content.c storage.c metadata.c tests/c2/test_transfer_negative.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread -I. network.c protocol.c transfer_protocol.c compression.c content.c storage.c metadata.c tests/c2/test_transfer_negative.c -o $@ $(LDLIBS) $(LZ4_LIB)

test-c2: $(BIN_DIR)/test_transfer_negative
	./$(BIN_DIR)/test_transfer_negative
