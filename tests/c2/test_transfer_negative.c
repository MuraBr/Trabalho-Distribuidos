#define _POSIX_C_SOURCE 200809L
#include "compression.h"
#include "content.h"
#include "network.h"
#include "protocol.h"
#include "storage.h"
#include "transfer_protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void make_wire_message(uint8_t output[HEADER_WIRE_SIZE + 4U])
{
    int pair[2];
    Message message;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(message_init(&message) == 0);
    message.header.message_type = M_PING;
    message.header.payload_size = 4U;
    message.payload = (uint8_t *)"PING";
    assert(protocol_send_message(pair[0], &message) == PROTOCOL_OK);
    assert(network_recv_exact(pair[1], output, HEADER_WIRE_SIZE + 4U) == (ssize_t)(HEADER_WIRE_SIZE + 4U));
    message.payload = NULL;
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);
}

static void test_protocol_rejections(void)
{
    uint8_t wire[HEADER_WIRE_SIZE + 4U];
    int pair[2];
    Message received;

    make_wire_message(wire);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    wire[HEADER_WIRE_SIZE] ^= 1U;
    assert(network_send_all(pair[0], wire, sizeof(wire)) == (ssize_t)sizeof(wire));
    assert(message_init(&received) == 0);
    assert(protocol_receive_message(pair[1], &received) == PROTOCOL_ERROR);
    message_free(&received);
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);

    make_wire_message(wire);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(network_send_all(pair[0], wire, HEADER_WIRE_SIZE + 2U) == (ssize_t)(HEADER_WIRE_SIZE + 2U));
    assert(shutdown(pair[0], SHUT_WR) == 0);
    assert(message_init(&received) == 0);
    assert(protocol_receive_message(pair[1], &received) == PROTOCOL_ERROR);
    message_free(&received);
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);

    make_wire_message(wire);
    wire[90] = 0U;
    wire[91] = 0x50U;
    wire[92] = 0U;
    wire[93] = 1U;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(network_send_all(pair[0], wire, HEADER_WIRE_SIZE) == (ssize_t)HEADER_WIRE_SIZE);
    assert(message_init(&received) == 0);
    assert(protocol_receive_message(pair[1], &received) == PROTOCOL_ERROR);
    message_free(&received);
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);
}

static void cleanup_storage(const char *root, const ObjectID *id)
{
    char hex[OBJECT_ID_HEX_SIZE];
    char path[512];

    assert(object_id_to_hex(id, hex, sizeof(hex)) == 0);
    assert(snprintf(path, sizeof(path), "%s/pending/%s/manifest.bin", root, hex) > 0);
    (void)unlink(path);
    assert(snprintf(path, sizeof(path), "%s/pending/%s/chunk-%020u.lz4", root, hex, 0U) > 0);
    (void)unlink(path);
    assert(snprintf(path, sizeof(path), "%s/pending/%s/chunk-%020u.lz4", root, hex, 1U) > 0);
    (void)unlink(path);
    assert(snprintf(path, sizeof(path), "%s/pending/%s", root, hex) > 0);
    (void)rmdir(path);
    assert(snprintf(path, sizeof(path), "%s/pending", root) > 0);
    (void)rmdir(path);
    assert(snprintf(path, sizeof(path), "%s/objects", root) > 0);
    (void)rmdir(path);
    (void)rmdir(root);
}

static void test_storage_validation(void)
{
    static const uint8_t raw[] = "%PDF-1.4\n%%EOF\n";
    char root_template[] = "/tmp/p2p-storage-test-XXXXXX";
    char *root = mkdtemp(root_template);
    NodeID owner;
    Storage *storage = NULL;
    TransferDocument document;
    TransferChunk chunk;
    uint8_t *compressed = NULL;
    uint8_t *incompatible = NULL;
    size_t compressed_size = 0U;

    assert(root != NULL);
    memset(&owner, 0x5a, sizeof(owner));
    memset(&document, 0, sizeof(document));
    assert(content_sha256(raw, sizeof(raw) - 1U, document.id.bytes) == 0);
    strcpy(document.name, "negative.pdf");
    document.file_size = sizeof(raw) - 1U;
    document.chunk_count = 1U;
    document.compression = COMPRESSION_LZ4;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(compression_lz4_compress(raw, sizeof(raw) - 1U, &compressed, &compressed_size) == 0);
    memset(&chunk, 0, sizeof(chunk));
    chunk.id = document.id;
    chunk.raw_size = (uint32_t)(sizeof(raw) - 1U);
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    memset(chunk.hash, 0, sizeof(chunk.hash));
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0 && errno == EBADMSG);
    assert(content_sha256(raw, sizeof(raw) - 1U, chunk.hash) == 0);
    chunk.index = 1U;
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0 && errno == EINVAL);
    chunk.index = 0U;
    chunk.offset = 1U;
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0 && errno == EINVAL);
    chunk.offset = 0U;
    assert(storage_put_chunk(storage, &chunk) == 0);
    incompatible = malloc(compressed_size + 1U);
    assert(incompatible != NULL);
    memcpy(incompatible, compressed, compressed_size);
    incompatible[compressed_size] = 0U;
    chunk.data = incompatible;
    chunk.compressed_size++;
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0);
    free(incompatible);
    free(compressed);
    storage_destroy(storage);
    cleanup_storage(root, &document.id);
}

static void test_object_id_mismatch(void)
{
    static const uint8_t raw[] = "%PDF-1.4\nobjeto divergente\n%%EOF\n";
    char root_template[] = "/tmp/p2p-object-test-XXXXXX";
    char *root = mkdtemp(root_template);
    NodeID owner;
    Storage *storage = NULL;
    TransferDocument document;
    TransferDocument committed;
    TransferChunk chunk;
    uint8_t *compressed = NULL;
    size_t compressed_size = 0U;

    assert(root != NULL);
    memset(&owner, 0x3c, sizeof(owner));
    memset(&document, 0, sizeof(document));
    assert(content_sha256(raw, sizeof(raw) - 1U, document.id.bytes) == 0);
    document.id.bytes[0] ^= 1U;
    strcpy(document.name, "mismatch.pdf");
    document.file_size = sizeof(raw) - 1U;
    document.chunk_count = 1U;
    document.compression = COMPRESSION_LZ4;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(compression_lz4_compress(raw, sizeof(raw) - 1U, &compressed, &compressed_size) == 0);
    memset(&chunk, 0, sizeof(chunk));
    chunk.id = document.id;
    chunk.raw_size = (uint32_t)(sizeof(raw) - 1U);
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    assert(content_sha256(raw, sizeof(raw) - 1U, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    errno = 0;
    assert(storage_commit(storage, &document.id, &committed) < 0 && errno == EBADMSG);
    free(compressed);
    storage_destroy(storage);
    cleanup_storage(root, &document.id);
}

static void test_pending_restart(void)
{
    static const uint8_t tail[] = "x";
    char root_template[] = "/tmp/p2p-resume-test-XXXXXX";
    char *root = mkdtemp(root_template);
    NodeID owner;
    Storage *storage = NULL;
    TransferDocument document;
    TransferChunk chunk;
    uint8_t *raw = NULL;
    uint8_t *compressed = NULL;
    size_t compressed_size = 0U;

    assert(root != NULL);
    raw = calloc((size_t)METADATA_CHUNK_SIZE, 1U);
    assert(raw != NULL);
    memset(&owner, 0x17, sizeof(owner));
    memset(&document, 0, sizeof(document));
    document.id.bytes[0] = 0x42U;
    strcpy(document.name, "resume.pdf");
    document.file_size = METADATA_CHUNK_SIZE + 1U;
    document.chunk_count = 2U;
    document.compression = COMPRESSION_LZ4;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(compression_lz4_compress(raw, (size_t)METADATA_CHUNK_SIZE, &compressed, &compressed_size) == 0);
    memset(&chunk, 0, sizeof(chunk));
    chunk.id = document.id;
    chunk.raw_size = (uint32_t)METADATA_CHUNK_SIZE;
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    assert(content_sha256(raw, (size_t)METADATA_CHUNK_SIZE, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    storage_destroy(storage);
    storage = NULL;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    free(compressed);
    compressed = NULL;
    assert(compression_lz4_compress(tail, sizeof(tail) - 1U, &compressed, &compressed_size) == 0);
    chunk.index = 1U;
    chunk.offset = METADATA_CHUNK_SIZE;
    chunk.raw_size = (uint32_t)(sizeof(tail) - 1U);
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    assert(content_sha256(tail, sizeof(tail) - 1U, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    free(compressed);
    free(raw);
    storage_destroy(storage);
    cleanup_storage(root, &document.id);
}

int main(void)
{
    test_protocol_rejections();
    test_storage_validation();
    test_object_id_mismatch();
    test_pending_restart();
    puts("transfer/storage negative tests: ok");
    return 0;
}
