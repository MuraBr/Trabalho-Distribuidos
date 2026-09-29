#ifndef STORAGE_H
#define STORAGE_H

#include "transfer_protocol.h"

#include <stddef.h>

typedef struct Storage Storage;

int storage_create(const char *root, const NodeID *owner, Storage **output);
void storage_destroy(Storage *storage);
int storage_begin(Storage *storage, const TransferDocument *document);
int storage_put_chunk(Storage *storage, const TransferChunk *chunk);
int storage_commit(Storage *storage, const ObjectID *id, TransferDocument *document);
int storage_find(Storage *storage, TransferSelectorType type, const ObjectID *id, const char *name, TransferDocument *document);
int storage_read_chunk(Storage *storage, const ObjectID *id, uint64_t index, TransferChunk *chunk, uint8_t **owned_data);
int storage_list(Storage *storage, TransferDocument **documents, size_t *count);

#endif
