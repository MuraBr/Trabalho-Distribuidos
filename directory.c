#define _POSIX_C_SOURCE 200809L
#include "directory.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct DirectoryName
{
    ObjectID id;
    char name[METADATA_NAME_SIZE];
    struct DirectoryName *next;
} DirectoryName;

struct Directory
{
    MetadataStore *metadata;
    SuperPeer *superpeer;
    pthread_mutex_t mutex;
    DirectoryName *names;
};

int directory_create(MetadataStore *metadata, SuperPeer *superpeer, Directory **output)
{
    Directory *directory;
    int error;

    if (metadata == NULL || superpeer == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    directory = calloc(1U, sizeof(*directory));
    if (directory == NULL)
    {
        return -1;
    }
    error = pthread_mutex_init(&directory->mutex, NULL);
    if (error != 0)
    {
        free(directory);
        errno = error;
        return -1;
    }
    directory->metadata = metadata;
    directory->superpeer = superpeer;
    *output = directory;
    return 0;
}

void directory_destroy(Directory *directory)
{
    DirectoryName *name;

    if (directory == NULL)
    {
        return;
    }
    name = directory->names;
    while (name != NULL)
    {
        DirectoryName *next = name->next;

        free(name);
        name = next;
    }
    pthread_mutex_destroy(&directory->mutex);
    free(directory);
}

int directory_announce(Directory *directory, const TransferDocument *document, const NodeID *owner)
{
    DirectoryName *name;
    uint64_t index;
    int error;

    if (directory == NULL || document == NULL || owner == NULL || document->compression != COMPRESSION_LZ4)
    {
        errno = EINVAL;
        return -1;
    }
    if (metadata_register_document(directory->metadata, &document->id, document->name, document->file_size) < 0)
    {
        return -1;
    }
    for (index = 0U; index < document->chunk_count; ++index)
    {
        if (metadata_register_chunk(directory->metadata, &document->id, index, owner) < 0)
        {
            return -1;
        }
    }
    error = pthread_mutex_lock(&directory->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    for (name = directory->names; name != NULL; name = name->next)
    {
        if (memcmp(name->id.bytes, document->id.bytes, OBJECT_ID_SIZE) == 0)
        {
            pthread_mutex_unlock(&directory->mutex);
            return 0;
        }
    }
    name = calloc(1U, sizeof(*name));
    if (name == NULL)
    {
        pthread_mutex_unlock(&directory->mutex);
        return -1;
    }
    name->id = document->id;
    strcpy(name->name, document->name);
    name->next = directory->names;
    directory->names = name;
    pthread_mutex_unlock(&directory->mutex);
    return 0;
}

static int resolve_name(Directory *directory, const char *name, ObjectID *id)
{
    DirectoryName *current;
    int found = 0;
    int error = pthread_mutex_lock(&directory->mutex);

    if (error != 0)
    {
        errno = error;
        return -1;
    }
    for (current = directory->names; current != NULL; current = current->next)
    {
        if (strcmp(current->name, name) == 0)
        {
            if (found && memcmp(id->bytes, current->id.bytes, OBJECT_ID_SIZE) != 0)
            {
                pthread_mutex_unlock(&directory->mutex);
                errno = ENOTUNIQ;
                return -1;
            }
            *id = current->id;
            found = 1;
        }
    }
    pthread_mutex_unlock(&directory->mutex);
    if (!found)
    {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int directory_lookup(Directory *directory, TransferSelectorType type, const ObjectID *id, const char *name, TransferLookupResult *result)
{
    ObjectID selected;
    MetadataDocument metadata_document;
    uint64_t chunk_index;

    if (directory == NULL || result == NULL || (type == TRANSFER_SELECTOR_OBJECT_ID && id == NULL) || (type == TRANSFER_SELECTOR_NAME && name == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (type == TRANSFER_SELECTOR_OBJECT_ID)
    {
        selected = *id;
    }
    else if (resolve_name(directory, name, &selected) < 0)
    {
        return -1;
    }
    if (metadata_find_document(directory->metadata, &selected, &metadata_document) < 0 || metadata_document.chunk_count > SIZE_MAX / sizeof(*result->chunks))
    {
        return -1;
    }
    result->document.id = metadata_document.id;
    strcpy(result->document.name, metadata_document.name);
    result->document.file_size = metadata_document.file_size;
    result->document.chunk_count = metadata_document.chunk_count;
    result->document.compression = COMPRESSION_LZ4;
    if (metadata_document.chunk_count != 0U)
    {
        result->chunks = calloc((size_t)metadata_document.chunk_count, sizeof(*result->chunks));
        if (result->chunks == NULL)
        {
            return -1;
        }
    }
    for (chunk_index = 0U; chunk_index < metadata_document.chunk_count; ++chunk_index)
    {
        NodeID *owners = NULL;
        size_t owner_count = 0U;
        size_t owner_index;
        size_t valid_count = 0U;
        TransferEndpoint *endpoints = NULL;

        if (metadata_chunk_peers(directory->metadata, &selected, chunk_index, &owners, &owner_count) < 0)
        {
            transfer_lookup_result_free(result);
            return -1;
        }
        if (owner_count != 0U)
        {
            endpoints = calloc(owner_count, sizeof(*endpoints));
            if (endpoints == NULL)
            {
                free(owners);
                transfer_lookup_result_free(result);
                return -1;
            }
        }
        for (owner_index = 0U; owner_index < owner_count; ++owner_index)
        {
            SuperPeerMember member;

            if (superpeer_find_member(directory->superpeer, &owners[owner_index], &member) == 0)
            {
                TransferEndpoint *endpoint = &endpoints[valid_count++];

                endpoint->node_id = owners[owner_index];
                strcpy(endpoint->ip, member.node.config.ip);
                endpoint->port = member.node.config.port;
            }
        }
        free(owners);
        result->chunks[chunk_index].peers = endpoints;
        result->chunks[chunk_index].peer_count = valid_count;
        if (valid_count == 0U)
        {
            transfer_lookup_result_free(result);
            errno = ENODATA;
            return -1;
        }
    }
    return 0;
}
