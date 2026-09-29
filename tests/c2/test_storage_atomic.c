#define _POSIX_C_SOURCE 200809L
#include "storage.h"
#include "content.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Falhas reais de filesystem: o caminho do temporário é ocupado por diretório. */
int main(void)
{
    char root[] = "/tmp/c2-atomic-XXXXXX";
    char object[OBJECT_ID_HEX_SIZE], obstacle[512];
    NodeID owner = {{1}};
    Storage *storage = NULL;
    TransferDocument document = {.name = "atomic.pdf", .file_size = 3U, .chunk_count = 1U, .compression = COMPRESSION_LZ4};
    TransferDocument committed;
    uint8_t *compressed = NULL;
    size_t size;
    assert(mkdtemp(root) != NULL);
    assert(content_sha256((const uint8_t *)"abc", 3U, document.id.bytes) == 0);
    assert(object_id_to_hex(&document.id, object, sizeof(object)) == 0);
    assert(compression_lz4_compress((const uint8_t *)"abc", 3U, &compressed, &size) == 0);
    TransferChunk chunk = {.id = document.id, .index = 0U, .offset = 0U, .raw_size = 3U, .compressed_size = (uint32_t)size, .data = compressed};
    memcpy(chunk.hash, document.id.bytes, OBJECT_ID_SIZE);
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    (void)snprintf(obstacle, sizeof(obstacle), "%s/pending/%s/manifest.tmp-%ld", root, object, (long)getpid());
    assert(mkdir(obstacle, 0700) == 0);
    assert(storage_put_chunk(storage, &chunk) == -1);
    assert(storage_commit(storage, &document.id, &committed) == -1 && errno == ENODATA);
    assert(rmdir(obstacle) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    assert(mkdir(obstacle, 0700) == 0);
    assert(storage_commit(storage, &document.id, &committed) == -1);
    assert(rmdir(obstacle) == 0);
    assert(storage_commit(storage, &document.id, &committed) == 0);
    free(compressed);
    assert(compression_lz4_compress((const uint8_t *)"xyz", 3U, &compressed, &size) == 0);
    chunk.data = compressed;
    chunk.compressed_size = (uint32_t)size;
    assert(content_sha256((const uint8_t *)"xyz", 3U, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == -1 && errno == EEXIST);
    free(compressed);
    storage_destroy(storage);
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_find(storage, TRANSFER_SELECTOR_OBJECT_ID, &document.id, NULL, &committed) == 0);
    storage_destroy(storage);
    printf("storage atomic/fault tests: ok; evidências: %s\n", root);
    return 0;
}
