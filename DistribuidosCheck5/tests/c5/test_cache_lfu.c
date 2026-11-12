#define _POSIX_C_SOURCE 200809L
#include "storage.h"
#include "compression.h"
#include "content.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    Storage *storage;
    ObjectID id;
    int failed;
} CacheReadWork;

static size_t create_document(Storage *storage, const char *name, const uint8_t *bytes, size_t size, TransferDocument *document)
{
    uint8_t *compressed = NULL;
    size_t compressed_size = 0U;
    TransferChunk chunk;
    TransferDocument committed;
    memset(document, 0, sizeof(*document));
    strcpy(document->name, name);
    document->file_size = (uint64_t)size;
    document->chunk_count = 1U;
    document->compression = COMPRESSION_LZ4;
    assert(content_sha256(bytes, size, document->id.bytes) == 0);
    assert(compression_lz4_compress(bytes, size, &compressed, &compressed_size) == 0);
    memset(&chunk, 0, sizeof(chunk));
    chunk.id = document->id;
    chunk.raw_size = (uint32_t)size;
    chunk.compressed_size = (uint32_t)compressed_size;
    memcpy(chunk.hash, document->id.bytes, OBJECT_ID_SIZE);
    chunk.data = compressed;
    assert(storage_begin(storage, document) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    assert(storage_commit(storage, &document->id, &committed) == 0);
    free(compressed);
    return compressed_size;
}

static int read_chunk(Storage *storage, const ObjectID *id)
{
    TransferChunk chunk;
    uint8_t *owned = NULL;
    int status = storage_read_chunk(storage, id, 0U, &chunk, &owned);
    if (status == 0) free(owned);
    return status;
}

static void *read_repeatedly(void *argument)
{
    CacheReadWork *work = argument;
    for (size_t i = 0U; i < 100U; ++i) if (read_chunk(work->storage, &work->id) < 0) work->failed = 1;
    return NULL;
}

int main(void)
{
    char root[] = "/tmp/c5-cache-XXXXXX";
    const uint8_t a[] = {1U, 2U, 3U};
    const uint8_t b[] = {4U, 5U, 6U};
    const uint8_t c[] = {7U, 8U, 9U};
    NodeID owner = {{1U}};
    Storage *storage = NULL;
    TransferDocument documents[3];
    StorageCacheStats stats;
    size_t chunk_size;
    assert(mkdtemp(root) != NULL);
    assert(storage_create(root, &owner, &storage) == 0);
    chunk_size = create_document(storage, "a.pdf", a, sizeof(a), &documents[0]);
    assert(create_document(storage, "b.pdf", b, sizeof(b), &documents[1]) == chunk_size);
    assert(create_document(storage, "c.pdf", c, sizeof(c), &documents[2]) == chunk_size);
    assert(storage_set_cache_capacity(storage, chunk_size * 2U) == 0);
    assert(read_chunk(storage, &documents[0].id) == 0);
    assert(read_chunk(storage, &documents[1].id) == 0);
    assert(read_chunk(storage, &documents[0].id) == 0);
    assert(read_chunk(storage, &documents[2].id) == 0);
    assert(storage_get_cache_stats(storage, &stats) == 0);
    assert(stats.hits == 1U && stats.misses == 3U && stats.evictions == 1U && stats.entries == 2U && stats.used_bytes == chunk_size * 2U);
    assert(read_chunk(storage, &documents[0].id) == 0);
    assert(read_chunk(storage, &documents[1].id) == 0);
    assert(storage_get_cache_stats(storage, &stats) == 0 && stats.hits == 2U && stats.misses == 4U);
    assert(storage_set_cache_capacity(storage, 0U) == 0);
    assert(storage_get_cache_stats(storage, &stats) == 0 && stats.capacity_bytes == 0U && stats.entries == 0U && stats.used_bytes == 0U);
    assert(read_chunk(storage, &documents[2].id) == 0);
    assert(storage_set_cache_capacity(storage, chunk_size * 2U) == 0);
    assert(read_chunk(storage, &documents[0].id) == 0);
    assert(read_chunk(storage, &documents[1].id) == 0);
    assert(read_chunk(storage, &documents[2].id) == 0);
    assert(read_chunk(storage, &documents[1].id) == 0);
    assert(read_chunk(storage, &documents[0].id) == 0);
    assert(storage_get_cache_stats(storage, &stats) == 0 && stats.evictions >= 4U);
    CacheReadWork work = {.storage = storage, .id = documents[1].id, .failed = 0};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, read_repeatedly, &work) == 0);
    for (size_t i = 0U; i < 100U; ++i) assert(storage_set_cache_capacity(storage, i % 2U == 0U ? chunk_size * 2U : 0U) == 0);
    assert(pthread_join(thread, NULL) == 0 && work.failed == 0);
    storage_destroy(storage);
    puts("LFU local: hit/miss/frequency, eviction, durable reread, zero capacity and concurrent reconfiguration OK");
    return 0;
}
