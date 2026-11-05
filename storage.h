#ifndef STORAGE_H
#define STORAGE_H

#include "transfer_types.h"

#include <stddef.h>

typedef struct Storage Storage;

typedef struct
{
    size_t capacity_bytes;
    size_t used_bytes;
    size_t entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} StorageCacheStats;

int storage_create(const char *root, const NodeID *owner, Storage **output);
int storage_set_cache_capacity(Storage *storage, size_t capacity_bytes);
int storage_get_cache_stats(Storage *storage, StorageCacheStats *output);
int storage_descriptors(Storage *storage, const ObjectID *id, MetadataChunk **output);
void storage_destroy(Storage *storage);
int storage_begin(Storage *storage, const TransferDocument *document);
int storage_put_chunk(Storage *storage, const TransferChunk *chunk);
int storage_commit(Storage *storage, const ObjectID *id, TransferDocument *document);
int storage_find(Storage *storage, TransferSelectorType type, const ObjectID *id, const char *name, TransferDocument *document);
int storage_read_chunk(Storage *storage, const ObjectID *id, uint64_t index, TransferChunk *chunk, uint8_t **owned_data);
int storage_read_chunk_with_cache_info(Storage *storage, const ObjectID *id, uint64_t index, TransferChunk *chunk, uint8_t **owned_data, int *cache_hit, uint64_t *cache_frequency);
int storage_list(Storage *storage, TransferDocument **documents, size_t *count);

#endif
