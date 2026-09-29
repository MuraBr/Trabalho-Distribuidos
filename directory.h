#ifndef DIRECTORY_H
#define DIRECTORY_H

#include "metadata.h"
#include "superpeer.h"
#include "transfer_protocol.h"

typedef struct Directory Directory;

int directory_create(MetadataStore *metadata, SuperPeer *superpeer, Directory **output);
void directory_destroy(Directory *directory);
int directory_announce(Directory *directory, const TransferDocument *document, const NodeID *owner);
int directory_lookup(Directory *directory, TransferSelectorType type, const ObjectID *id, const char *name, TransferLookupResult *result);

#endif
