#define _POSIX_C_SOURCE 200809L
#include "metadata.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#define BUCKET_COUNT 257U
/* Registros esparsos: não alocamos todos os chunks ao cadastrar um arquivo grande. */
typedef struct Availability
{
    uint64_t index;
    NodeID peer;
    struct Availability *next;
} Availability;
typedef struct Entry
{
    FileMetadata document;
    Availability *available;
    MetadataChunk *chunks;
    struct Entry *next;
} Entry;
struct MetadataStore
{
    Entry *buckets[BUCKET_COUNT];
    pthread_mutex_t mutex;
};

static int fail(int error)
{
    errno = error;
    return -1;
}

int object_id_file(const char *path, ObjectID *output, uint64_t *file_size)
{
    unsigned char buffer[65536];
    ObjectID result;
    uint64_t total = 0;
    unsigned int length = 0;
    int error = 0;
    if (path == NULL || output == NULL || file_size == NULL) return fail(EINVAL);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) { fclose(file); return fail(ENOMEM); }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) error = EIO;
    while (error == 0)
    {
        size_t n = fread(buffer, 1, sizeof(buffer), file);
        if (n > UINT64_MAX - total) { error = EOVERFLOW; break; }
        total += n;
        if (n != 0 && EVP_DigestUpdate(ctx, buffer, n) != 1) { error = EIO; break; }
        if (n < sizeof(buffer)) { if (ferror(file)) error = EIO; break; }
    }
    if (error == 0 && (EVP_DigestFinal_ex(ctx, result.bytes, &length) != 1 || length != OBJECT_ID_SIZE)) error = EIO;
    EVP_MD_CTX_free(ctx);
    if (fclose(file) != 0 && error == 0) error = errno;
    if (error != 0) return fail(error);
    *output = result;
    *file_size = total;
    return 0;
}

int object_id_to_hex(const ObjectID *id, char *output, size_t capacity)
{
    const char hex[] = "0123456789abcdef";
    if (id == NULL || output == NULL || capacity < OBJECT_ID_HEX_SIZE) return fail(EINVAL);
    for (size_t i = 0; i < OBJECT_ID_SIZE; ++i) { output[i * 2] = hex[id->bytes[i] >> 4]; output[i * 2 + 1] = hex[id->bytes[i] & 15]; }
    output[64] = '\0';
    return 0;
}

/* Hash de todos os bytes; colisões são resolvidas por encadeamento e comparação do ID completo. */
static size_t bucket(const ObjectID *id)
{
    size_t value = 0;
    for (size_t i = 0; i < OBJECT_ID_SIZE; ++i) value = (value * 33U + id->bytes[i]) % BUCKET_COUNT;
    return value;
}

static int lock_store(MetadataStore *store)
{
    int error = pthread_mutex_lock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}

/* Todas as buscas internas exigem o mutex adquirido. */
static Entry **find_entry(MetadataStore *store, const ObjectID *id)
{
    Entry **entry = &store->buckets[bucket(id)];
    while (*entry != NULL && memcmp((*entry)->document.object_id, id->bytes, OBJECT_ID_SIZE) != 0) entry = &(*entry)->next;
    return entry;
}

static void free_entry(Entry *entry)
{
    while (entry->available != NULL) { Availability *next = entry->available->next; free(entry->available); entry->available = next; }
    free(entry->document.chunk_hashes);
    free(entry->chunks);
    free(entry);
}

void file_metadata_free(FileMetadata *document)
{
    if (document == NULL) return;
    free(document->chunk_hashes);
    document->chunk_hashes = NULL;
}

int metadata_create(MetadataStore **output)
{
    if (output == NULL) return fail(EINVAL);
    *output = NULL;
    MetadataStore *store = calloc(1, sizeof(*store));
    if (store == NULL) return -1;
    int error = pthread_mutex_init(&store->mutex, NULL);
    if (error != 0) { free(store); return fail(error); }
    *output = store;
    return 0;
}

void metadata_destroy(MetadataStore *store)
{
    if (store == NULL) return;
    for (size_t i = 0; i < BUCKET_COUNT; ++i) { Entry *entry = store->buckets[i]; while (entry != NULL) { Entry *next = entry->next; free_entry(entry); entry = next; } }
    pthread_mutex_destroy(&store->mutex);
    free(store);
}

int metadata_register_document(MetadataStore *store, const ObjectID *id, const char *name, uint64_t file_size)
{
    if (store == NULL || id == NULL || name == NULL || name[0] == '\0' || strnlen(name, METADATA_NAME_SIZE) == METADATA_NAME_SIZE) return fail(EINVAL);
    uint64_t chunk_count = file_size / METADATA_CHUNK_SIZE + (file_size % METADATA_CHUNK_SIZE != 0U);
    if (chunk_count > UINT32_MAX) return fail(EOVERFLOW);
    if (lock_store(store) != 0) return -1;
    Entry **slot = find_entry(store, id);
    int error = 0;
    if (*slot != NULL) { if ((*slot)->document.size != file_size) error = EEXIST; }
    else
    {
        Entry *entry = calloc(1, sizeof(*entry));
        if (entry == NULL) error = ENOMEM;
        else
        {
            memcpy(entry->document.object_id, id->bytes, OBJECT_ID_SIZE);
            strcpy(entry->document.filename, name);
            entry->document.size = file_size;
            entry->document.chunk_count = (uint32_t)chunk_count;
            entry->document.version = 1U;
            *slot = entry;
        }
    }
    pthread_mutex_unlock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}

int metadata_find_document(MetadataStore *store, const ObjectID *id, FileMetadata *output)
{
    if (store == NULL || id == NULL || output == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry *entry = *find_entry(store, id);
    int error = entry == NULL ? ENOENT : 0;
    FileMetadata copy;
    if (error == 0)
    {
        copy = entry->document;
        copy.chunk_hashes = NULL;
        size_t count = copy.chunk_count;
        if (entry->document.chunk_hashes != NULL)
        {
            if (count > SIZE_MAX / (sizeof(*copy.chunk_hashes) + OBJECT_ID_SIZE)) error = EOVERFLOW;
            else
            {
                copy.chunk_hashes = malloc(count * (sizeof(*copy.chunk_hashes) + OBJECT_ID_SIZE));
                if (copy.chunk_hashes == NULL) error = ENOMEM;
                else
                {
                    uint8_t *hashes = (uint8_t *)(copy.chunk_hashes + count);
                    for (size_t i = 0U; i < count; ++i)
                    {
                        copy.chunk_hashes[i] = hashes + i * OBJECT_ID_SIZE;
                        memcpy(copy.chunk_hashes[i], entry->document.chunk_hashes[i], OBJECT_ID_SIZE);
                    }
                }
            }
        }
        if (error == 0) *output = copy;
    }
    pthread_mutex_unlock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}

int metadata_remove_document(MetadataStore *store, const ObjectID *id)
{
    if (store == NULL || id == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry **slot = find_entry(store, id);
    Entry *entry = *slot;
    int found = entry != NULL;
    if (found) { *slot = entry->next; free_entry(entry); }
    pthread_mutex_unlock(&store->mutex);
    return found ? 0 : fail(ENOENT);
}

/* A mesma seção crítica cobre validação, busca de duplicata e alteração. */
static int change_chunk(MetadataStore *store, const ObjectID *id, uint64_t index, const NodeID *peer, int remove)
{
    if (store == NULL || id == NULL || peer == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry *entry = *find_entry(store, id);
    int error = 0;
    if (entry == NULL) error = ENOENT;
    else if (index >= entry->document.chunk_count) error = EINVAL;
    else
    {
        Availability **slot = &entry->available;
        while (*slot != NULL && ((*slot)->index != index || memcmp((*slot)->peer.bytes, peer->bytes, NODE_ID_SIZE) != 0)) slot = &(*slot)->next;
        if (remove)
        {
            if (*slot == NULL) error = ENOENT;
            else { Availability *old = *slot; *slot = old->next; free(old); }
        }
        else if (*slot == NULL)
        {
            Availability *item = calloc(1, sizeof(*item));
            if (item == NULL) error = ENOMEM;
            else { item->index = index; item->peer = *peer; *slot = item; }
        }
    }
    pthread_mutex_unlock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}

int metadata_register_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer)
{
    return change_chunk(store, id, chunk_index, peer, 0);
}

int metadata_unregister_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer)
{
    return change_chunk(store, id, chunk_index, peer, 1);
}

int metadata_chunk_peers(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, NodeID **output, size_t *count)
{
    if (store == NULL || id == NULL || output == NULL || count == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry *entry = *find_entry(store, id);
    int error = 0;
    size_t n = 0;
    NodeID *peers = NULL;
    if (entry == NULL) error = ENOENT;
    else if (chunk_index >= entry->document.chunk_count) error = EINVAL;
    else
    {
        for (Availability *item = entry->available; item != NULL; item = item->next) if (item->index == chunk_index) ++n;
        if (n > SIZE_MAX / sizeof(*peers)) error = EOVERFLOW;
        else if (n != 0 && (peers = malloc(n * sizeof(*peers))) == NULL) error = ENOMEM;
        if (error == 0)
        {
            size_t i = 0;
            for (Availability *item = entry->available; item != NULL; item = item->next) if (item->index == chunk_index) peers[i++] = item->peer;
            *output = peers;
            *count = n;
        }
    }
    pthread_mutex_unlock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}

/* Publicação atômica: aloca e valida uma cópia completa antes de trocar o bucket. */
int metadata_announce(MetadataStore *store, const FileMetadata *document, const MetadataChunk *chunks, const NodeID *owner)
{
    if (store == NULL || document == NULL || chunks == NULL || owner == NULL || document->chunk_count == 0U || document->chunk_count != document->size / METADATA_CHUNK_SIZE + (document->size % METADATA_CHUNK_SIZE != 0U) || document->filename[0] == '\0' || strnlen(document->filename, METADATA_NAME_SIZE) == METADATA_NAME_SIZE) return fail(EINVAL);
    Entry *candidate = calloc(1U, sizeof(*candidate));
    if (candidate == NULL) return -1;
    candidate->document = *document;
    candidate->document.chunk_hashes = NULL;
    candidate->document.version = 1U;
    candidate->chunks = malloc((size_t)document->chunk_count * sizeof(*chunks));
    if (candidate->chunks == NULL) { free_entry(candidate); return -1; }
    memcpy(candidate->chunks, chunks, (size_t)document->chunk_count * sizeof(*chunks));
    candidate->document.chunk_hashes = calloc((size_t)document->chunk_count, sizeof(*candidate->document.chunk_hashes));
    if (candidate->document.chunk_hashes == NULL) { free_entry(candidate); return -1; }
    for (uint64_t i = 0U; i < document->chunk_count; ++i)
    {
        uint64_t offset = i * METADATA_CHUNK_SIZE;
        uint64_t remaining = document->size - offset;
        if (chunks[i].index != i || chunks[i].offset != offset || chunks[i].raw_size != (remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining) || chunks[i].compressed_size == 0U || (document->chunk_hashes != NULL && (document->chunk_hashes[i] == NULL || memcmp(document->chunk_hashes[i], chunks[i].hash, OBJECT_ID_SIZE) != 0))) { free_entry(candidate); return fail(EINVAL); }
        candidate->document.chunk_hashes[i] = candidate->chunks[i].hash;
        Availability *item = calloc(1U, sizeof(*item));
        if (item == NULL) { free_entry(candidate); return -1; }
        item->index = i;
        item->peer = *owner;
        item->next = candidate->available;
        candidate->available = item;
    }
    if (lock_store(store) < 0) { free_entry(candidate); return -1; }
    ObjectID id;
    memcpy(id.bytes, document->object_id, OBJECT_ID_SIZE);
    Entry **slot = find_entry(store, &id);
    Entry *old = *slot;
    int error = 0;
    if (old != NULL)
    {
        if (old->document.size != document->size || old->document.chunk_count != document->chunk_count) error = EEXIST;
        for (uint64_t i = 0U; error == 0 && old->chunks != NULL && i < document->chunk_count; ++i)
            if (old->chunks[i].raw_size != chunks[i].raw_size || memcmp(old->chunks[i].hash, chunks[i].hash, OBJECT_ID_SIZE) != 0) error = EEXIST;
        for (Availability *item = old->available; error == 0 && item != NULL; item = item->next)
        {
            if (memcmp(item->peer.bytes, owner->bytes, NODE_ID_SIZE) == 0) continue;
            Availability *copy = malloc(sizeof(*copy));
            if (copy == NULL) { error = ENOMEM; break; }
            *copy = *item;
            copy->next = candidate->available;
            candidate->available = copy;
        }
        if (error == 0)
        {
            uint8_t **hashes = candidate->document.chunk_hashes;
            if (old->chunks != NULL) candidate->document = old->document;
            else strcpy(candidate->document.filename, old->document.filename);
            candidate->document.chunk_hashes = hashes;
            candidate->next = old->next;
        }
    }
    if (error == 0) { *slot = candidate; if (old != NULL) free_entry(old); }
    pthread_mutex_unlock(&store->mutex);
    if (error != 0) { free_entry(candidate); return fail(error); }
    return 0;
}

int metadata_find_name(MetadataStore *store, const char *name, ObjectID *id)
{
    if (store == NULL || name == NULL || id == NULL) return fail(EINVAL);
    if (lock_store(store) < 0) return -1;
    int found = 0;
    ObjectID result = {{0}};
    for (size_t i = 0U; i < BUCKET_COUNT; ++i)
        for (Entry *entry = store->buckets[i]; entry != NULL; entry = entry->next)
            if (strcmp(entry->document.filename, name) == 0)
            {
                if (found) { pthread_mutex_unlock(&store->mutex); return fail(ENOTUNIQ); }
                memcpy(result.bytes, entry->document.object_id, OBJECT_ID_SIZE);
                found = 1;
            }
    pthread_mutex_unlock(&store->mutex);
    if (!found) return fail(ENOENT);
    *id = result;
    return 0;
}

int metadata_chunk_descriptor(MetadataStore *store, const ObjectID *id, uint64_t index, MetadataChunk *output)
{
    if (store == NULL || id == NULL || output == NULL) return fail(EINVAL);
    if (lock_store(store) < 0) return -1;
    Entry *entry = *find_entry(store, id);
    int valid = entry != NULL && entry->chunks != NULL && index < entry->document.chunk_count;
    if (valid) *output = entry->chunks[index];
    pthread_mutex_unlock(&store->mutex);
    return valid ? 0 : fail(ENOENT);
}

int metadata_remove_peer(MetadataStore *store, const NodeID *peer)
{
    if (store == NULL || peer == NULL) return fail(EINVAL);
    if (lock_store(store) < 0) return -1;
    for (size_t i = 0U; i < BUCKET_COUNT; ++i)
        for (Entry *entry = store->buckets[i]; entry != NULL; entry = entry->next)
        {
            Availability **slot = &entry->available;
            while (*slot != NULL)
            {
                if (memcmp((*slot)->peer.bytes, peer->bytes, NODE_ID_SIZE) == 0) { Availability *old = *slot; *slot = old->next; free(old); }
                else slot = &(*slot)->next;
            }
        }
    pthread_mutex_unlock(&store->mutex);
    return 0;
}
