#ifndef METADATA_H
#define METADATA_H

#include "node.h"

#define OBJECT_ID_SIZE 32U
#define OBJECT_ID_HEX_SIZE 65U
#define METADATA_NAME_SIZE 256U
/* Convenção de integração: 4 MiB de conteúdo original por chunk. */
#define METADATA_CHUNK_SIZE UINT64_C(4194304)

typedef struct { uint8_t bytes[OBJECT_ID_SIZE]; } ObjectID;
typedef struct MetadataStore MetadataStore;
typedef struct
{
    ObjectID id;
    char name[METADATA_NAME_SIZE];
    uint64_t file_size;
    uint64_t chunk_count;
} MetadataDocument;

/* Retornos: 0 em sucesso, -1 com errno em erro. Saídas preservadas em erro, salvo indicação. */
/* SHA-256 em leitura incremental; o chamador deve impedir alterações no arquivo durante a leitura. */
int object_id_file(const char *path, ObjectID *output, uint64_t *file_size);
int object_id_to_hex(const ObjectID *id, char *output, size_t capacity);
/* Cada instância é independente; create zera output em erro. */
int metadata_create(MetadataStore **output);
/* Encerrar e aguardar todas as threads usuárias antes de destruir. */
void metadata_destroy(MetadataStore *store);
/* Registro idempotente por ID/tamanho; nomes alternativos preservam o primeiro nome. Tamanho divergente: EEXIST. */
int metadata_register_document(MetadataStore *store, const ObjectID *id, const char *name, uint64_t file_size);
int metadata_find_document(MetadataStore *store, const ObjectID *id, MetadataDocument *output);
int metadata_remove_document(MetadataStore *store, const ObjectID *id);
/* Índice começa em zero. Registro repetido do mesmo peer é idempotente. Não valida membership. */
int metadata_register_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer);
int metadata_unregister_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer);
/* Retorna cópia consistente alocada; liberar com free(). Sem peers: NULL/0. Ordem não definida. */
int metadata_chunk_peers(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, NodeID **output, size_t *count);

#endif
