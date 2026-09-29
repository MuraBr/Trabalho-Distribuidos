#define _POSIX_C_SOURCE 200809L
#include "metadata.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(((FileMetadata *)0)->object_id) == 32U, "ObjectID deve ter 32 bytes");
_Static_assert(sizeof(((FileMetadata *)0)->filename) == 256U, "Nome deve ter 256 bytes");
_Static_assert(_Generic(((FileMetadata *)0)->size, uint64_t: 1, default: 0), "Tamanho deve ter 64 bits");
_Static_assert(_Generic(((FileMetadata *)0)->chunk_count, uint32_t: 1, default: 0), "Contagem deve ter 32 bits");
_Static_assert(_Generic(((FileMetadata *)0)->chunk_hashes, uint8_t **: 1, default: 0), "Hashes devem ser um vetor de ponteiros");
_Static_assert(_Generic(((FileMetadata *)0)->version, uint32_t: 1, default: 0), "Versao deve ter 32 bits");
_Static_assert(_Generic(((FileMetadata *)0)->owner, uint32_t: 1, default: 0), "Proprietario deve ter 32 bits");

/* Exercita o contrato público que será usado pelo aluno 1. */
static MetadataStore *store;
static ObjectID object;
static void *register_peer(void *arg)
{
    NodeID peer = {{0}};
    peer.bytes[0] = *(unsigned char *)arg;
    assert(metadata_register_chunk(store, &object, 0, &peer) == 0);
    return NULL;
}

int main(void)
{
    char path[] = "/tmp/aluno2-metadata-XXXXXX";
    int fd = mkstemp(path);
    ObjectID id, empty;
    uint64_t size = 99;
    char hex[65];
    FileMetadata doc;
    NodeID a = {{1}}, b = {{2}}, *peers = NULL;
    size_t count;
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(object_id_file(path, &empty, &size) == 0 && size == 0);
    assert(object_id_to_hex(&empty, hex, sizeof(hex)) == 0);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fwrite("abc", 1, 3, file) == 3 && fclose(file) == 0);
    assert(object_id_file(path, &id, &size) == 0 && size == 3);
    assert(object_id_to_hex(&id, hex, sizeof(hex)) == 0);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    /* O hash deve atravessar vários buffers de leitura sem carregar o arquivo inteiro. */
    file = fopen(path, "wb");
    assert(file != NULL);
    for (size_t i = 0; i < 200000; ++i) assert(fputc('a', file) == 'a');
    assert(fclose(file) == 0);
    ObjectID large;
    assert(object_id_file(path, &large, &size) == 0 && size == 200000);
    assert(object_id_to_hex(&large, hex, sizeof(hex)) == 0);
    assert(strcmp(hex, "2287d207f24a941ff3b56c04c8a25ad56b63e3023207b3bb5b4ac0c9869d74be") == 0);
    assert(unlink(path) == 0);
    assert(object_id_file(path, &id, &size) == -1 && errno == ENOENT);
    assert(metadata_create(&store) == 0);
    object = id;
    assert(metadata_register_document(store, &id, "exemplo.pdf", METADATA_CHUNK_SIZE + 1) == 0);
    assert(metadata_register_document(store, &id, "exemplo.pdf", METADATA_CHUNK_SIZE + 1) == 0);
    assert(metadata_register_document(store, &id, "outro.pdf", 3) == -1 && errno == EEXIST);
    assert(metadata_find_document(store, &id, &doc) == 0 && doc.chunk_count == 2);
    assert(metadata_register_chunk(store, &id, 2, &a) == -1 && errno == EINVAL);
    assert(metadata_register_chunk(store, &empty, 0, &a) == -1 && errno == ENOENT);
    assert(metadata_register_chunk(store, &id, 0, &a) == 0);
    assert(metadata_register_chunk(store, &id, 0, &a) == 0);
    assert(metadata_register_chunk(store, &id, 0, &b) == 0);
    assert(metadata_chunk_peers(store, &id, 0, &peers, &count) == 0 && count == 2);
    free(peers);
    assert(metadata_chunk_peers(store, &id, 1, &peers, &count) == 0 && count == 0 && peers == NULL);
    assert(metadata_unregister_chunk(store, &id, 0, &a) == 0);
    assert(metadata_unregister_chunk(store, &id, 0, &a) == -1 && errno == ENOENT);
    pthread_t threads[8];
    unsigned char ids[8];
    for (size_t i = 0; i < 8; ++i) { ids[i] = (unsigned char)(i + 3); assert(pthread_create(&threads[i], NULL, register_peer, &ids[i]) == 0); }
    for (size_t i = 0; i < 8; ++i) { assert(pthread_join(threads[i], NULL) == 0); }
    assert(metadata_chunk_peers(store, &id, 0, &peers, &count) == 0 && count == 9);
    free(peers);
    /* Mais documentos que buckets garantem colisões sem perda de registros. */
    for (unsigned int i = 0; i < 600; ++i) { ObjectID key = {{0}}; key.bytes[0] = (unsigned char)i; key.bytes[1] = (unsigned char)(i >> 8); assert(metadata_register_document(store, &key, "colisao.pdf", 0) == 0); }
    for (unsigned int i = 0; i < 600; ++i) { ObjectID key = {{0}}; key.bytes[0] = (unsigned char)i; key.bytes[1] = (unsigned char)(i >> 8); assert(metadata_find_document(store, &key, &doc) == 0 && doc.chunk_count == 0); }
    assert(metadata_remove_document(store, &id) == 0);
    assert(metadata_find_document(store, &id, &doc) == -1 && errno == ENOENT);
    assert(metadata_register_document(store, &empty, "", 0) == -1 && errno == EINVAL);
    /* Fronteiras de tamanho, aliases e saídas independentes da tabela. */
    assert(metadata_register_document(store, &empty, "vazio.pdf", 0) == 0);
    assert(metadata_register_chunk(store, &empty, 0, &a) == -1 && errno == EINVAL);
    assert(metadata_register_document(store, &id, "inteiro.pdf", METADATA_CHUNK_SIZE) == 0);
    assert(metadata_register_document(store, &id, "alias.pdf", METADATA_CHUNK_SIZE) == 0);
    assert(metadata_find_document(store, &id, &doc) == 0 && doc.chunk_count == 1 && strcmp(doc.filename, "inteiro.pdf") == 0);
    assert(metadata_register_chunk(store, &id, 0, &a) == 0);
    assert(metadata_chunk_peers(store, &id, 0, &peers, &count) == 0 && count == 1);
    assert(metadata_remove_document(store, &id) == 0);
    assert(memcmp(peers[0].bytes, a.bytes, NODE_ID_SIZE) == 0);
    free(peers);
    assert(metadata_register_document(store, &id, "maximo.pdf", UINT64_MAX) == -1 && errno == EOVERFLOW);
    assert(metadata_register_document(store, &id, "maximo.pdf", (uint64_t)UINT32_MAX * METADATA_CHUNK_SIZE) == 0);
    assert(metadata_find_document(store, &id, &doc) == 0 && doc.chunk_count == UINT32_MAX);
    assert(metadata_register_chunk(store, &id, (uint64_t)doc.chunk_count - 1U, &a) == 0);
    char long_name[METADATA_NAME_SIZE + 1];
    memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    assert(metadata_register_document(store, &id, long_name, 0) == -1 && errno == EINVAL);
    assert(metadata_create(NULL) == -1 && errno == EINVAL);
    assert(metadata_find_document(NULL, &id, &doc) == -1 && errno == EINVAL);
    assert(object_id_to_hex(&id, hex, 64) == -1 && errno == EINVAL);
    /* Em erro, a API não sobrescreve os dados que pertencem ao chamador. */
    FileMetadata saved = doc;
    assert(metadata_find_document(store, &large, &doc) == -1 && errno == ENOENT);
    assert(memcmp(&saved, &doc, sizeof(doc)) == 0);
    peers = &a;
    count = 42;
    assert(metadata_chunk_peers(store, &large, 0, &peers, &count) == -1 && errno == ENOENT);
    assert(peers == &a && count == 42);
    ObjectID previous = id;
    size = 42;
    assert(object_id_file(path, &id, &size) == -1 && errno == ENOENT);
    assert(memcmp(previous.bytes, id.bytes, OBJECT_ID_SIZE) == 0 && size == 42);
    /* Anúncio completo deve publicar todos os chunks ou preservar o registro anterior. */
    FileMetadata announced = {.filename = "completo.pdf", .size = 3U, .chunk_count = 1U, .version = 1U, .owner = 7U};
    memcpy(announced.object_id, large.bytes, OBJECT_ID_SIZE);
    MetadataChunk descriptor = {.index = 0U, .offset = 0U, .raw_size = 3U, .compressed_size = 4U, .hash = {7U}};
    assert(metadata_announce(store, &announced, &descriptor, &a) == 0);
    assert(metadata_announce(store, &announced, &descriptor, &b) == 0);
    FileMetadata file_metadata;
    assert(metadata_find_document(store, &large, &file_metadata) == 0 && file_metadata.chunk_count == 1U && file_metadata.chunk_hashes != NULL && file_metadata.chunk_hashes[0][0] == 7U);
    file_metadata.chunk_hashes[0][0] = 99U;
    file_metadata_free(&file_metadata);
    assert(metadata_chunk_descriptor(store, &large, 0U, &descriptor) == 0 && descriptor.hash[0] == 7U);
    assert(metadata_find_document(store, &large, &doc) == 0 && doc.version == 1U && doc.owner == 7U);
    file_metadata_free(&doc);
    /* O primeiro anúncio completa um registro esparso e mantém seu nome original. */
    ObjectID sparse = {{9U, 0U, 1U}};
    assert(metadata_register_document(store, &sparse, "original.pdf", 3U) == 0);
    memcpy(announced.object_id, sparse.bytes, OBJECT_ID_SIZE);
    announced.owner = 42U;
    assert(metadata_announce(store, &announced, &descriptor, &a) == 0);
    assert(metadata_find_document(store, &sparse, &doc) == 0 && strcmp(doc.filename, "original.pdf") == 0 && doc.owner == 42U && doc.chunk_hashes[0][0] == 7U);
    file_metadata_free(&doc);
    memcpy(announced.object_id, large.bytes, OBJECT_ID_SIZE);
    announced.owner = 7U;
    assert(metadata_chunk_peers(store, &large, 0U, &peers, &count) == 0 && count == 2U);
    free(peers);
    descriptor.hash[0] = 8U;
    assert(metadata_announce(store, &announced, &descriptor, &a) == -1 && errno == EEXIST);
    assert(metadata_chunk_descriptor(store, &large, 0U, &descriptor) == 0 && descriptor.hash[0] == 7U);
    assert(metadata_remove_peer(store, &a) == 0);
    assert(metadata_chunk_peers(store, &large, 0U, &peers, &count) == 0 && count == 1U);
    free(peers);
    assert(metadata_find_name(store, "completo.pdf", &previous) == 0 && memcmp(previous.bytes, large.bytes, OBJECT_ID_SIZE) == 0);
    metadata_destroy(store);
    puts("metadata C2: ok");
    return 0;
}
