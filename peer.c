#define _POSIX_C_SOURCE 200809L
#include "app_config.h"
#include "local_control.h"
#include "heartbeat.h"
#include "wire.h"
#include <pthread.h>

#include "concurrent_server.h"
#include "c4_log.h"
#include "chord_network.h"
#include "network.h"
#include "node.h"
#include "file_client.h"
#include "protocol.h"
#include "rpc.h"
#include "remote_error.h"
#include "storage.h"
#include "transfer_protocol.h"
#include "wire.h"

#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PEER_BACKLOG 32
#define PEER_STORAGE_PATH_SIZE 512U

typedef struct
{
    int server_fd;
    Node node;
    NodeID superpeer_id;
    LocalControl *control;
    Storage *storage;
    ConcurrentServer *runtime;
    char superpeer_host[NODE_ADDRESS_SIZE];
    uint16_t superpeer_port;
    Heartbeat *heartbeat;
    pthread_mutex_t identity_mutex;
} PeerService;

static int reconnect_service(void *context);

static int configure_cache_capacity(Storage *storage)
{
    const char *text = getenv("PEER_CACHE_BYTES");
    char *end;
    unsigned long long parsed;
    if (text == NULL) return 0;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (text[0] == '-' || errno != 0 || end == text || *end != '\0' || parsed > (unsigned long long)SIZE_MAX) { errno = EINVAL; return -1; }
    return storage_set_cache_capacity(storage, (size_t)parsed);
}

static NodeID superpeer_identity(PeerService *service)
{
    pthread_mutex_lock(&service->identity_mutex);
    NodeID id = service->superpeer_id;
    pthread_mutex_unlock(&service->identity_mutex);
    return id;
}

static volatile sig_atomic_t service_server_fd = -1;

static void stop_service(int signal_number)
{
    (void)signal_number;
    if (service_server_fd >= 0)
    {
        (void)shutdown(service_server_fd, SHUT_RDWR);
    }
}

static int send_response(PeerService *service, int socket_fd, const Message *request, Message_Type type, const uint8_t *payload, uint32_t payload_size)
{
    Message response;
    int status;
    uint8_t error_payload[2];

    if (type == M_ERROR) { remote_error_encode(errno, error_payload); payload = error_payload; payload_size = sizeof(error_payload); }

    if (message_init(&response) < 0)
    {
        return -1;
    }
    response.header.message_type = (uint8_t)type;
    memcpy(response.header.source_node, service->node.id.bytes, NODE_ID_SIZE);
    memcpy(response.header.destination_node, request->header.source_node, NODE_ID_SIZE);
    memcpy(response.header.transaction_id, request->header.transaction_id, TRANSACTION_ID_SIZE);
    response.header.timestamp = (uint64_t)time(NULL);
    if (payload_size != 0U)
    {
        response.payload = malloc(payload_size);
        if (response.payload == NULL)
        {
            return -1;
        }
        memcpy(response.payload, payload, payload_size);
        response.header.payload_size = payload_size;
    }
    status = protocol_send_message(socket_fd, &response);
    message_free(&response);
    return status == PROTOCOL_OK ? 0 : -1;
}

static int join_superpeer(PeerService *service)
{
    uint8_t payload[JOIN_PAYLOAD_WIRE_SIZE + 9U];
    Message response;
    int status;

    payload[JOIN_PAYLOAD_WIRE_SIZE] = 1U;
    wire_put_u64(payload + JOIN_PAYLOAD_WIRE_SIZE + 1U, heartbeat_incarnation(service->heartbeat));
    if (rpc_encode_join_payload(&service->node.config, payload) < 0 || rpc_call_deadline(service->superpeer_host, service->superpeer_port, &service->node.id, NULL, M_JOIN, payload, sizeof(payload), &response, heartbeat_budget(service->heartbeat)) < 0)
    {
        return -1;
    }
    status = response.header.message_type == (uint8_t)M_ACK ? 0 : -1;
    if (status == 0)
    {
        NodeConfig remote_config;
        Node remote_node;
        if (rpc_decode_join_payload(response.payload, response.header.payload_size, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0 || memcmp(remote_node.id.bytes, response.header.source_node, NODE_ID_SIZE) != 0 || heartbeat_peer_target(service->heartbeat, &remote_config, reconnect_service) < 0) status = -1;
    }
    if (status == 0) { pthread_mutex_lock(&service->identity_mutex); memcpy(service->superpeer_id.bytes, response.header.source_node, NODE_ID_SIZE); pthread_mutex_unlock(&service->identity_mutex); }
    message_free(&response);
    if (status < 0)
    {
        errno = EACCES;
    }
    return status;
}

static int announce_document(PeerService *service, const TransferDocument *document)
{
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    Message response;
    int status = -1;

    MetadataChunk *chunks = NULL;
    NodeID remote_id = superpeer_identity(service);
    if (storage_descriptors(service->storage, &document->id, &chunks) < 0) return -1;
    int encoded = transfer_encode_announcement(document, chunks, &payload, &payload_size);
    free(chunks);
    if (encoded < 0 || rpc_call_deadline(service->superpeer_host, service->superpeer_port, &service->node.id, &remote_id, M_STORE, payload, payload_size, &response, heartbeat_budget(service->heartbeat)) < 0)
    {
        free(payload);
        return -1;
    }
    if (response.header.message_type == (uint8_t)M_ACK)
    {
        status = 0;
    }
    else
    {
        errno = remote_error_decode(response.payload, response.header.payload_size);
    }
    message_free(&response);
    free(payload);
    return status;
}

static int announce_catalog(PeerService *service)
{
    TransferDocument *documents = NULL;
    size_t count = 0U;
    size_t index;

    if (storage_list(service->storage, &documents, &count) < 0)
    {
        return -1;
    }
    for (index = 0U; index < count; ++index)
    {
        if (announce_document(service, &documents[index]) < 0)
        {
            free(documents);
            return -1;
        }
    }
    free(documents);
    return 0;
}

static int handle_store(PeerService *service, int socket_fd, const Message *message)
{
    int status = -1;

    if (message->header.payload_size == 0U)
    {
        errno = EBADMSG;
    }
    else if (message->payload[0] == TRANSFER_STORE_BEGIN)
    {
        TransferDocument document;

        if (transfer_decode_document(message->payload, message->header.payload_size, TRANSFER_STORE_BEGIN, &document) == 0)
        {
            status = storage_begin(service->storage, &document);
        }
    }
    else if (message->payload[0] == TRANSFER_STORE_CHUNK)
    {
        TransferChunk chunk;

        if (transfer_decode_chunk(message->payload, message->header.payload_size, TRANSFER_STORE_CHUNK, &chunk) == 0)
        {
            status = storage_put_chunk(service->storage, &chunk);
        }
    }
    else if (message->payload[0] == TRANSFER_STORE_COMMIT)
    {
        ObjectID id;
        TransferDocument document;

        if (transfer_decode_object_operation(message->payload, message->header.payload_size, TRANSFER_STORE_COMMIT, &id) == 0 && storage_commit(service->storage, &id, &document) == 0)
        {
            status = announce_document(service, &document);
        }
    }
    else
    {
        errno = ENOTSUP;
    }
    return send_response(service, socket_fd, message, status == 0 ? M_ACK : M_ERROR, NULL, 0U);
}

static int handle_download(PeerService *service, int socket_fd, const Message *message)
{
    ObjectID id;
    uint64_t index;
    TransferChunk chunk;
    uint8_t *owned_data = NULL;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    uint64_t cache_frequency = 0U;
    int cache_hit = 0;
    int status;

    if (transfer_decode_chunk_request(message->payload, message->header.payload_size, &id, &index) < 0 || storage_read_chunk_with_cache_info(service->storage, &id, index, &chunk, &owned_data, &cache_hit, &cache_frequency) < 0 || transfer_encode_chunk(&chunk, TRANSFER_DOWNLOAD_CHUNK, &payload, &payload_size) < 0)
    {
        free(owned_data);
        free(payload);
        return send_response(service, socket_fd, message, M_ERROR, NULL, 0U);
    }
    printf("LFU CACHE %s index=%" PRIu64 " frequency=%" PRIu64 "\n", cache_hit ? "HIT" : "MISS", index, cache_frequency);
    fflush(stdout);
    status = send_response(service, socket_fd, message, M_DOWNLOAD_REP, payload, payload_size);
    free(payload);
    free(owned_data);
    return status;
}

static void serve_connection(void *context, int socket_fd)
{
    PeerService *service = context;
    Message message;

    message_init(&message);
    if (protocol_receive_message(socket_fd, &message) == PROTOCOL_OK)
    {
        const uint8_t zero[NODE_ID_SIZE] = {0};
        int c2 = message.header.message_type == M_STORE || message.header.message_type == M_DOWNLOAD_REQ;
        printf("Peer RX tipo=%u origem=", (unsigned)message.header.message_type);
        for (size_t i = 0U; i < NODE_ID_SIZE; ++i) printf("%02x", (unsigned)message.header.source_node[i]);
        struct sockaddr_in remote;
        socklen_t remote_size = sizeof(remote);
        char ip[INET_ADDRSTRLEN] = "desconhecido";
        if (getpeername(socket_fd, (struct sockaddr *)&remote, &remote_size) == 0) (void)inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        printf(" ip_origem=%s\n", ip);
        fflush(stdout);
        if (c2 && (memcmp(message.header.source_node, zero, NODE_ID_SIZE) == 0 || memcmp(message.header.destination_node, service->node.id.bytes, NODE_ID_SIZE) != 0))
        {
            errno = EACCES;
            (void)send_response(service, socket_fd, &message, M_ERROR, NULL, 0U);
            message_free(&message);
            return;
        }
        if (message.header.message_type == M_HEARTBEAT)
        {
            uint8_t *payload = NULL; uint32_t size = 0U; Message_Type type = M_ERROR;
            if (heartbeat_handle(service->heartbeat, &message, &type, &payload, &size) < 0) type = M_ERROR;
            (void)send_response(service, socket_fd, &message, type, payload, size);
            free(payload);
        }
        else if (message.header.message_type == (uint8_t)M_STORE)
        {
            (void)handle_store(service, socket_fd, &message);
        }
        else if (message.header.message_type == (uint8_t)M_DOWNLOAD_REQ)
        {
            (void)handle_download(service, socket_fd, &message);
        }
        else if (message.header.message_type == (uint8_t)M_PING && message.header.payload_size == 4U && memcmp(message.payload, "PING", 4U) == 0)
        {
            (void)send_response(service, socket_fd, &message, M_PONG, (const uint8_t *)"PONG", 4U);
        }
        else
        {
            errno = ENOTSUP;
            (void)send_response(service, socket_fd, &message, M_ERROR, NULL, 0U);
        }
    }
    message_free(&message);
}

static int initialize_service(PeerService *service, uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port)
{
    NodeConfig config;
    char storage_path[PEER_STORAGE_PATH_SIZE];
    int length;

    memset(service, 0, sizeof(*service));
    service->server_fd = -1;
    length = snprintf(storage_path, sizeof(storage_path), ".peer_storage/%" PRIu16, local_port);
    if (superpeer_host == NULL || length < 0 || (size_t)length >= sizeof(storage_path) || strlen(superpeer_host) >= sizeof(service->superpeer_host))
    {
        errno = EINVAL;
        return -1;
    }
    if (app_config.data_dir[0] != '\0') { strcpy(storage_path, app_config.data_dir); }
    if (app_identity(storage_path, &config, local_port) < 0 || node_init(&service->node, &config) < 0)
    {
        return -1;
    }
    strcpy(service->superpeer_host, superpeer_host);
    service->superpeer_port = superpeer_port;
    if (storage_create(storage_path, &service->node.id, &service->storage) < 0)
    {
        return -1;
    }
    if (configure_cache_capacity(service->storage) < 0) { storage_destroy(service->storage); service->storage = NULL; return -1; }
    int error = pthread_mutex_init(&service->identity_mutex, NULL);
    if (error != 0) { storage_destroy(service->storage); errno = error; return -1; }
    if (heartbeat_create(&service->node, NULL, storage_path, NULL, service, &service->heartbeat) < 0) { pthread_mutex_destroy(&service->identity_mutex); storage_destroy(service->storage); return -1; }
    return 0;
}

/* Reconexão ao endpoint configurado: mantém identidade e anuncia os manifests finalizados. */
static int reconnect_service(void *context)
{
    PeerService *service = context;
    if (join_superpeer(service) < 0 || announce_catalog(service) < 0) return -1;
    printf("Peer reconnected; catalog announced\n"); fflush(stdout);
    return 0;
}

static int peer_service_run(uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port)
{
    PeerService service;
    int status = EXIT_FAILURE;
    int joined = 0;

    if (initialize_service(&service, local_port, superpeer_host, superpeer_port) < 0)
    {
        perror("peer initialization");
        return EXIT_FAILURE;
    }
    /* Identidade já está durável antes de abrir TCP; facilita diagnóstico durante o JOIN. */
    printf("NodeID: ");
    for (size_t index = 0U; index < NODE_ID_SIZE; ++index) printf("%02x", (unsigned)service.node.id.bytes[index]);
    printf("\n"); fflush(stdout);
    service.server_fd = network_create_server(local_port, PEER_BACKLOG);
    if (service.server_fd < 0 || concurrent_server_create(service.server_fd, serve_connection, &service, &service.runtime) < 0)
    {
        goto cleanup;
    }
    if (join_superpeer(&service) < 0)
    {
        perror("peer registration");
        goto cleanup;
    }
    joined = 1;
    if (announce_catalog(&service) < 0 || local_control_start(local_port, &service.node.id, &service.control) < 0)
    {
        perror("peer registration/control");
        goto cleanup;
    }
    service_server_fd = service.server_fd;
    if (heartbeat_start(service.heartbeat) < 0) { perror("heartbeat start"); goto cleanup; }
    (void)signal(SIGINT, stop_service);
    (void)signal(SIGTERM, stop_service);
    printf("Peer storage started\n");
    if (app_config.data_dir[0] != '\0') printf("Storage: %s\n", app_config.data_dir);
    else printf("Storage: .peer_storage/%" PRIu16 "\n", local_port);
    fflush(stdout);
    status = concurrent_server_run(service.runtime) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    service_server_fd = -1;
    /* Os handlers podem usar o detector: interrompe o listener e aguarda conexões primeiro. */
    if (service.runtime != NULL) concurrent_server_stop(service.runtime);
    local_control_stop(service.control);
    if (service.runtime != NULL) { concurrent_server_destroy(service.runtime); service.runtime = NULL; service.server_fd = -1; }
    heartbeat_stop(service.heartbeat);
    uint8_t leave[9] = {1U};
    wire_put_u64(leave + 1U, heartbeat_incarnation(service.heartbeat));
    heartbeat_destroy(service.heartbeat);
    service.heartbeat = NULL;
    if (joined)
    {
        Message response;
        if (rpc_call_deadline(service.superpeer_host, service.superpeer_port, &service.node.id, &service.superpeer_id, M_LEAVE, leave, sizeof(leave), &response, 1000U) == 0) message_free(&response);
    }
    if (service.runtime != NULL)
    {
        concurrent_server_destroy(service.runtime);
    }
    else if (service.server_fd >= 0)
    {
        (void)network_shutdown(service.server_fd);
    }
    storage_destroy(service.storage);
    pthread_mutex_destroy(&service.identity_mutex);
    return status;
}

/* Comandos e entrada do Peer. */
/* Ponto de entrada do Peer de armazenamento e dos comandos do cliente. */
#include "file_client.h"
#include "app_config.h"
#include "local_control.h"
#include "node.h"
#include "rpc.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SUPERPEER_HOST "127.0.0.1"
#define DEFAULT_SUPERPEER_PORT 55101U
#define DEFAULT_CLIENT_SUPERPEER_PORT 5101U
#define DEFAULT_PEER_HOST "127.0.0.1"
#define DEFAULT_PEER_PORT 55102U
static uint16_t local_peer_port = DEFAULT_PEER_PORT;

/* Cria uma identidade temporária para operações de cliente direto ao Super Peer. */
static int initialize_command_session(FileSession *session)
{
    NodeConfig config;
    Node local_node;
    if (session == NULL || node_config_init(&config, DEFAULT_PEER_HOST, 1U) < 0 || node_init(&local_node, &config) < 0) return -1;
    session->source = local_node.id;
    session->output = stdout;
    session->directory = ".";
    return 0;
}

/* Mostra a resposta de metadados; o servidor aquece a cache ao responder M_LOOKUP. */
static int print_document_lookup(FileSession *session, const char *selector, const char *host, uint16_t port)
{
    TransferLookupResult result;
    char object_hex[OBJECT_ID_HEX_SIZE];
    NodeID object_node;
    if (file_client_lookup(session, selector, host, port, &result) < 0) return -1;
    memcpy(object_node.bytes, result.document.id.bytes, NODE_ID_SIZE);
    if (node_id_to_hex(&object_node, object_hex, sizeof(object_hex)) < 0) { transfer_lookup_result_free(&result); return -1; }
    printf("Lookup concluido\nFile: %s\nObjectID: %s\nSize: %" PRIu64 " bytes\nChunks: %" PRIu64 "\n", result.document.name, object_hex, result.document.file_size, result.document.chunk_count);
    transfer_lookup_result_free(&result);
    return 0;
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || port == NULL || text[0] == '\0')
    {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL || value > UINT16_MAX)
    {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static const char *message_name(Message_Type type)
{
    switch (type)
    {
    case M_JOIN:
        return "JOIN";
    case M_LEAVE:
        return "LEAVE";
    case M_PING:
        return "PING";
    case M_PONG:
        return "PONG";
    case M_ACK:
        return "ACK";
    default:
        return "ERROR";
    }
}

static int legacy_command(const char *command, const char *host, uint16_t port)
{
    Message_Type request_type;
    Message_Type expected_type;
    const uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    uint8_t join_payload[JOIN_PAYLOAD_WIRE_SIZE];
    Node local_node;
    NodeConfig config;
    const NodeID *source = NULL;
    Message response;
    int status = -1;

    if (strcmp(command, "ping") == 0)
    {
        request_type = M_PING;
        expected_type = M_PONG;
        payload = (const uint8_t *)"PING";
        payload_size = 4U;
    }
    else if (strcmp(command, "join") == 0)
    {
        request_type = M_JOIN;
        expected_type = M_ACK;
        if (node_config_init(&config, "127.0.0.1", 65534U) < 0 || node_init(&local_node, &config) < 0 || rpc_encode_join_payload(&config, join_payload) < 0)
        {
            return -1;
        }
        source = &local_node.id;
        payload = join_payload;
        payload_size = JOIN_PAYLOAD_WIRE_SIZE;
    }
    else if (strcmp(command, "leave") == 0)
    {
        request_type = M_LEAVE;
        expected_type = M_ACK;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }
    printf("TX %s\n", message_name(request_type));
    if (rpc_call(host, port, source, NULL, request_type, payload, payload_size, &response) < 0)
    {
        return -1;
    }
    if (response.header.message_type == (uint8_t)expected_type && (expected_type != M_PONG || (response.header.payload_size == 4U && memcmp(response.payload, "PONG", 4U) == 0)))
    {
        printf("RX %s\n", message_name(expected_type));
        status = 0;
    }
    else
    {
        errno = EREMOTEIO;
    }
    message_free(&response);
    return status;
}

static int chord_client_call(const char *host, uint16_t port, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message *response)
{
    if (rpc_call(host, port, NULL, NULL, type, payload, payload_size, response) < 0) return -1;
    if (response->header.message_type != (uint8_t)type) { message_free(response); errno = EREMOTEIO; return -1; }
    return 0;
}

static int print_chord_peer(const char *label, const uint8_t *payload, uint32_t size)
{
    NodeConfig config;
    Node node;
    if (rpc_decode_join_payload(payload, size, &config) < 0 || node_init(&node, &config) < 0) return -1;
    printf("%s %s:%u NodeID=", label, config.ip, (unsigned)config.port);
    for (size_t i = 0U; i < NODE_ID_SIZE; ++i) printf("%02x", (unsigned)node.id.bytes[i]);
    putchar('\n');
    return 0;
}

static int topology_command(const char *host, uint16_t port)
{
    Message response;
    uint8_t index = 0U;
    if (chord_client_call(host, port, M_CHORD_FINGER, &index, 1U, &response) < 0) return -1;
    int status = response.header.payload_size == CHORD_PEER_WIRE_SIZE ? print_chord_peer("Successor:", response.payload, response.header.payload_size) : -1;
    if (status == 0) status = print_chord_peer("Finger[0]:", response.payload, response.header.payload_size);
    message_free(&response);
    if (status < 0) { errno = EBADMSG; return -1; }

    if (chord_client_call(host, port, M_CHORD_PREDECESSOR, NULL, 0U, &response) < 0) return -1;
    if (response.header.payload_size == 0U) printf("Predecessor: indisponivel\n");
    else if (response.header.payload_size != CHORD_PEER_WIRE_SIZE || print_chord_peer("Predecessor:", response.payload, response.header.payload_size) < 0) status = -1;
    message_free(&response);
    if (status < 0) { errno = EBADMSG; return -1; }

    index = UINT8_MAX;
    if (chord_client_call(host, port, M_CHORD_FINGER, &index, 1U, &response) < 0) return -1;
    status = response.header.payload_size == CHORD_PEER_WIRE_SIZE ? print_chord_peer("Finger[255]:", response.payload, response.header.payload_size) : -1;
    message_free(&response);
    if (status < 0) { errno = EBADMSG; return -1; }
    return 0;
}

static int lookup_command(const char *host, uint16_t port, const char *object_id)
{
    NodeID key;
    NodeID visited[CHORD_FINGER_COUNT];
    size_t visited_count = 0U;
    char current_host[NODE_ADDRESS_SIZE];
    if (node_id_from_hex(&key, object_id) < 0 || strlen(host) >= sizeof(current_host)) { errno = EINVAL; return -1; }
    strcpy(current_host, host);
    for (unsigned hop = 0U; hop < CHORD_FINGER_COUNT; ++hop)
    {
        Message response;
        if (chord_client_call(current_host, port, M_CHORD_ROUTE, key.bytes, NODE_ID_SIZE, &response) < 0) return -1;
        if (response.header.payload_size != CHORD_ROUTE_WIRE_SIZE || response.payload[0] > 1U) { message_free(&response); errno = EBADMSG; return -1; }
        NodeID current;
        memcpy(current.bytes, response.header.source_node, NODE_ID_SIZE);
        for (size_t i = 0U; i < visited_count; ++i) if (node_id_equal(&visited[i], &current)) { message_free(&response); errno = ELOOP; return -1; }
        visited[visited_count++] = current;
        int complete = response.payload[0] != 0U;
        NodeConfig next;
        Node owner;
        int status = rpc_decode_join_payload(response.payload + 1U, CHORD_PEER_WIRE_SIZE, &next);
        if (status == 0) status = node_init(&owner, &next);
        message_free(&response);
        if (status < 0) return -1;
        if (complete)
        {
            printf("Lookup path:");
            for (size_t path_index = 0U; path_index < visited_count; ++path_index)
            {
                char path_node[NODE_ID_SIZE * 2U + 1U];
                if (node_id_to_hex(&visited[path_index], path_node, sizeof(path_node)) < 0) return -1;
                printf(" %s", path_node);
            }
            printf(" -> %s:%u\nLookup concluido em %u saltos\nOwner: %s:%u NodeID=", next.ip, (unsigned)next.port, hop + 1U, next.ip, (unsigned)next.port);
            for (size_t i = 0U; i < NODE_ID_SIZE; ++i) printf("%02x", (unsigned)owner.id.bytes[i]);
            putchar('\n');
            return 0;
        }
        strcpy(current_host, next.ip);
        port = next.port;
    }
    errno = ELOOP;
    return -1;
}

typedef struct
{
    char name[METADATA_NAME_SIZE];
    uint64_t version;
} ClientStateObject;

/* Lê o log C4 local em modo somente leitura para os asserts do cluster.sh. */
static int client_state_command(const char *directory)
{
    char path[600];
    FILE *file;
    ClientStateObject *objects = NULL;
    size_t object_count = 0U;
    size_t object_capacity = 0U;
    uint64_t last_index = 0U;
    if (directory == NULL || directory[0] == '\0' || snprintf(path, sizeof(path), "%s/c4-operation.log", directory) >= (int)sizeof(path)) { errno = EINVAL; return -1; }
    file = fopen(path, "rb");
    if (file == NULL) return -1;
    for (;;)
    {
        uint8_t header[C4_LOG_HEADER_SIZE];
        size_t header_size = fread(header, 1U, sizeof(header), file);
        if (header_size == 0U && feof(file)) break;
        if (header_size != sizeof(header)) { errno = EBADMSG; goto state_fail; }
        uint32_t payload_size = wire_get_u32(header + 36U);
        if (payload_size > MAX_PAYLOAD_SIZE || payload_size > UINT32_MAX - C4_LOG_HEADER_SIZE) { errno = EMSGSIZE; goto state_fail; }
        uint32_t encoded_size = C4_LOG_HEADER_SIZE + payload_size;
        uint8_t *encoded = malloc(encoded_size);
        if (encoded == NULL) goto state_fail;
        memcpy(encoded, header, sizeof(header));
        if (payload_size > 0U && fread(encoded + C4_LOG_HEADER_SIZE, 1U, payload_size, file) != payload_size) { free(encoded); errno = EBADMSG; goto state_fail; }
        C4LogEntry entry;
        if (c4_log_entry_decode(encoded, encoded_size, &entry) < 0) { free(encoded); goto state_fail; }
        free(encoded);
        if (entry.index != last_index + 1U || entry.payload_size <= NODE_ID_SIZE) { c4_log_entry_free(&entry); errno = EBADMSG; goto state_fail; }
        uint64_t version = entry.version;
        TransferDocument document;
        MetadataChunk *chunks = NULL;
        if (transfer_decode_announcement(entry.payload + NODE_ID_SIZE, entry.payload_size - NODE_ID_SIZE, &document, &chunks) < 0) { c4_log_entry_free(&entry); goto state_fail; }
        free(chunks);
        c4_log_entry_free(&entry);
        size_t object_index = 0U;
        while (object_index < object_count && strcmp(objects[object_index].name, document.name) != 0) ++object_index;
        if (object_index == object_count)
        {
            if (object_count == object_capacity)
            {
                size_t next_capacity = object_capacity == 0U ? 8U : object_capacity * 2U;
                if (next_capacity < object_capacity || next_capacity > SIZE_MAX / sizeof(*objects)) { errno = EOVERFLOW; goto state_fail; }
                ClientStateObject *next = realloc(objects, next_capacity * sizeof(*objects));
                if (next == NULL) goto state_fail;
                objects = next;
                object_capacity = next_capacity;
            }
            strcpy(objects[object_count].name, document.name);
            object_index = object_count++;
        }
        objects[object_index].version = version;
        ++last_index;
    }
    if (ferror(file)) { errno = EIO; goto state_fail; }
    fclose(file);
    printf("LogIndex %llu\nObjects: %zu\n", (unsigned long long)last_index, object_count);
    for (size_t i = 0U; i < object_count; ++i) printf("%s v%llu\n", objects[i].name, (unsigned long long)objects[i].version);
    free(objects);
    return 0;

state_fail:
    {
        int saved_errno = errno;
        fclose(file);
        free(objects);
        errno = saved_errno;
    }
    return -1;
}

static int option_command(int argc, char **argv)
{
    const char *command = NULL;
    const char *host = NULL;
    const char *file = NULL;
    const char *output = NULL;
    const char *object_id = NULL;
    uint16_t port = 0U;
    int index;

    for (index = 1; index < argc; index += 2)
    {
        if (index + 1 >= argc)
        {
            errno = EINVAL;
            return -1;
        }
        if (strcmp(argv[index], "--cmd") == 0)
        {
            command = argv[index + 1];
        }
        else if (strcmp(argv[index], "--host") == 0)
        {
            host = argv[index + 1];
        }
        else if (strcmp(argv[index], "--port") == 0)
        {
            if (parse_port(argv[index + 1], &port) < 0)
            {
                return -1;
            }
        }
        else if ((strcmp(argv[index], "--file") == 0 || strcmp(argv[index], "--name") == 0))
        {
            file = argv[index + 1];
        }
        else if (strcmp(argv[index], "--object-id") == 0)
        {
            object_id = argv[index + 1];
        }
        else if (strcmp(argv[index], "--output") == 0)
        {
            output = argv[index + 1];
        }
        else
        {
            errno = EINVAL;
            return -1;
        }
    }
    if (command != NULL && strcmp(command, "state") == 0) return client_state_command(app_config.data_dir);
    if (command != NULL && (strcmp(command, "upload") == 0 || strcmp(command, "download") == 0))
    {
        if (host == NULL) host = "127.0.0.1";
        if (port == 0U) port = DEFAULT_CLIENT_SUPERPEER_PORT;
    }
    if (command == NULL || host == NULL || port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(command, "topology") == 0)
    {
        if (file != NULL || output != NULL || object_id != NULL) { errno = EINVAL; return -1; }
        return topology_command(host, port);
    }
    if (strcmp(command, "lookup") == 0)
    {
        if (output != NULL || (file == NULL) == (object_id == NULL)) { errno = EINVAL; return -1; }
        if (file != NULL)
        {
            FileSession session;
            if (initialize_command_session(&session) < 0) return -1;
            return print_document_lookup(&session, file, host, port);
        }
        if (strlen(object_id) != NODE_ID_SIZE * 2U) { errno = EINVAL; return -1; }
        return lookup_command(host, port, object_id);
    }
    if (strcmp(command, "upload") == 0)
    {
        FileSession session;
        if (file == NULL)
        {
            errno = EINVAL;
            return -1;
        }
        if (initialize_command_session(&session) < 0) return -1;
        return file_client_upload(&session, file, host, port);
    }
    if (strcmp(command, "download") == 0)
    {
        FileSession session;
        if (file == NULL)
        {
            errno = EINVAL;
            return -1;
        }
        if (initialize_command_session(&session) < 0) return -1;
        return file_client_download(&session, file, output, host, port);
    }
    if (file != NULL || output != NULL)
    {
        errno = EINVAL;
        return -1;
    }
    return legacy_command(command, host, port);
}

/* Divide uma linha de sessão em argumentos, respeitando aspas duplas e escapes. */
static int tokenize_session_line(char *line, char *arguments[], size_t capacity)
{
    char *read_cursor = line;
    char *write_cursor = line;
    size_t count = 0U;
    while (*read_cursor != '\0')
    {
        while (*read_cursor == ' ' || *read_cursor == '\t' || *read_cursor == '\r' || *read_cursor == '\n') ++read_cursor;
        if (*read_cursor == '\0' || *read_cursor == '#') break;
        if (count == capacity) { errno = E2BIG; return -1; }
        arguments[count++] = write_cursor;
        int quoted = 0;
        while (*read_cursor != '\0')
        {
            char current = *read_cursor++;
            if (current == '"') { quoted = !quoted; continue; }
            if (current == '\\' && *read_cursor != '\0' && (quoted || *read_cursor == ' ' || *read_cursor == '\t' || *read_cursor == '"' || *read_cursor == '\\')) current = *read_cursor++;
            else if (!quoted && (current == ' ' || current == '\t' || current == '\r' || current == '\n')) break;
            *write_cursor++ = current;
        }
        if (quoted) { errno = EINVAL; return -1; }
        *write_cursor++ = '\0';
    }
    return (int)count;
}

/* Executa comandos do harness com a identidade e o destino da sessão atuais. */
static int run_session_command(FileSession *session, char *const arguments[], int count, const char *host, uint16_t port)
{
    if (count == 0) return 0;
    const char *command = arguments[0];
    if (strcmp(command, "help") == 0)
    {
        puts("upload <arquivo> | download <nome> <saida> | lookup <nome|object-id>");
        puts("ping|join|leave [host porta] | topology | sleep <segundos> | quit");
        return 0;
    }
    if (strcmp(command, "upload") == 0 && count == 2) return file_client_upload(session, arguments[1], host, port);
    if (strcmp(command, "download") == 0 && (count == 2 || count == 3)) return file_client_download(session, arguments[1], count == 3 ? arguments[2] : NULL, host, port);
    if (strcmp(command, "lookup") == 0 && count == 2)
    {
        if (strlen(arguments[1]) == NODE_ID_SIZE * 2U) return lookup_command(host, port, arguments[1]);
        return print_document_lookup(session, arguments[1], host, port);
    }
    if (strcmp(command, "topology") == 0 && count == 1) return topology_command(host, port);
    if ((strcmp(command, "ping") == 0 || strcmp(command, "join") == 0 || strcmp(command, "leave") == 0) && (count == 1 || count == 3))
    {
        uint16_t target_port = port;
        if (count == 3 && parse_port(arguments[2], &target_port) < 0) return -1;
        return legacy_command(command, count == 3 ? arguments[1] : host, target_port);
    }
    if (strcmp(command, "sleep") == 0 && count == 2)
    {
        char *end = NULL;
        errno = 0;
        unsigned long seconds = strtoul(arguments[1], &end, 10);
        if (errno != 0 || end == arguments[1] || *end != '\0' || seconds > 86400UL) { errno = EINVAL; return -1; }
        while (seconds > 0UL) seconds = sleep((unsigned int)(seconds > UINT_MAX ? UINT_MAX : seconds));
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* Mantém uma identidade única durante a sessão e emite marcadores para o harness. */
static int run_client_session(FILE *input, int interactive, int keep_going)
{
    NodeConfig config;
    Node local_node;
    FileSession session;
    char node_hex[NODE_ID_SIZE * 2U + 1U];
    char *line = NULL;
    size_t line_capacity = 0U;
    unsigned long command_number = 0UL;
    int final_status = 0;
    if (node_config_init(&config, app_config.advertised, app_config.port) < 0 || node_init(&local_node, &config) < 0 || node_id_to_hex(&local_node.id, node_hex, sizeof(node_hex)) < 0) return -1;
    session.source = local_node.id;
    session.output = stdout;
    session.directory = ".";
    printf("NodeID: %s\n[ready]\n", node_hex);
    fflush(stdout);
    while (1)
    {
        char *arguments[64];
        if (interactive && isatty(STDIN_FILENO)) { fputs("client> ", stdout); fflush(stdout); }
        ssize_t length = getline(&line, &line_capacity, input);
        if (length < 0) break;
        int count = tokenize_session_line(line, arguments, sizeof(arguments) / sizeof(arguments[0]));
        if (count == 0) continue;
        int parse_error = count < 0 ? errno : 0;
        ++command_number;
        if (count == 1 && (strcmp(arguments[0], "quit") == 0 || strcmp(arguments[0], "exit") == 0))
        {
            printf("[done %lu rc=0]\n[bye]\n", command_number);
            fflush(stdout);
            free(line);
            return interactive ? 0 : final_status;
        }
        errno = parse_error;
        int command_status = count < 0 ? -1 : run_session_command(&session, arguments, count, app_config.superpeer, app_config.superpeer_port);
        int command_rc = command_status == 0 ? 0 : 1;
        if (command_rc != 0) { int saved_errno = errno == 0 ? EIO : errno; fprintf(stderr, "comando: %s\n", strerror(saved_errno)); final_status = 1; }
        printf("[done %lu rc=%d]\n", command_number, command_rc);
        fflush(stdout);
        if (command_rc != 0 && !keep_going) break;
    }
    if (interactive) puts("[bye]");
    fflush(stdout);
    free(line);
    return interactive ? 0 : final_status;
}

static void usage(const char *program)
{
    fprintf(stderr, "Uso:\n  %s serve <porta-peer> <host-superpeer> <porta-superpeer>\n  %s upload <arquivo.pdf> [<host-peer> <porta-peer>]\n  %s download <nome-ou-objectid> [<destino>] [<host-superpeer> <porta-superpeer>]\n  %s benchmark <arquivo.pdf>\n  %s --cmd <ping|join|leave|upload|download|topology|lookup|state> --host <ip> --port <porta> [--file <arquivo>] [--object-id <sha256>] [--output <destino>] [--data-dir <diretorio>]\n", program, program, program, program, program);
}

int main(int argc, char **argv)
{
    int result = -1;
    int interactive = 0;
    int batch_keep_going = 0;
    const char *batch_path = NULL;

    if (app_config_load(&argc, argv, 0) < 0) { perror("config"); return EXIT_FAILURE; }
    (void)signal(SIGPIPE, SIG_IGN);
    /* Remove a opção transversal antes de interpretar a CLI legada. */
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--local-peer-port") == 0)
        {
            if (i + 1 >= argc || parse_port(argv[i + 1], &local_peer_port) < 0) goto failure;
            for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            argv[argc] = NULL;
            --i;
        }
    }
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--interactive") == 0)
        {
            interactive = 1;
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            argv[--argc] = NULL;
            --i;
        }
        else if (strcmp(argv[i], "--batch") == 0)
        {
            if (i + 1 >= argc || batch_path != NULL) { errno = EINVAL; goto failure; }
            batch_path = argv[i + 1];
            for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            argv[argc] = NULL;
            --i;
        }
        else if (strcmp(argv[i], "--keep-going") == 0)
        {
            batch_keep_going = 1;
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            argv[--argc] = NULL;
            --i;
        }
    }
    if (interactive || batch_path != NULL)
    {
        FILE *input = stdin;
        if (interactive && batch_path != NULL) { errno = EINVAL; goto failure; }
        if (batch_path != NULL && strcmp(batch_path, "-") != 0 && (input = fopen(batch_path, "r")) == NULL) goto failure;
        (void)setvbuf(stdout, NULL, _IOLBF, 0U);
        result = run_client_session(input, interactive, interactive || batch_keep_going);
        if (input != stdin) fclose(input);
        if (result == 0) return EXIT_SUCCESS;
        goto failure;
    }
    if (argc >= 2 && strcmp(argv[1], "--cmd") == 0)
    {
        result = option_command(argc, argv);
    }
    else if (argc == 2 && strcmp(argv[1], "serve") == 0)
    {
        return peer_service_run(app_config.port, app_config.superpeer, app_config.superpeer_port);
    }
    else if (argc == 5 && strcmp(argv[1], "serve") == 0)
    {
        uint16_t local_port;
        uint16_t superpeer_port;

        if (parse_port(argv[2], &local_port) == 0 && parse_port(argv[4], &superpeer_port) == 0)
        {
            return peer_service_run(local_port, argv[3], superpeer_port);
        }
    }
    else if ((argc == 3 || argc == 5) && strcmp(argv[1], "upload") == 0)
    {
        const char *host = argc == 5 ? argv[3] : DEFAULT_PEER_HOST;
        uint16_t port = DEFAULT_PEER_PORT;

        if ((argc == 3 || parse_port(argv[4], &port) == 0))
        {
            result = local_control_command(local_peer_port, 1, argv[2], NULL, host, port);
        }
    }
    else if (argc == 3 && strcmp(argv[1], "benchmark") == 0)
    {
        result = file_client_benchmark_lz4(argv[2]);
    }
    else if ((argc == 3 || argc == 4 || argc == 5 || argc == 6) && strcmp(argv[1], "download") == 0)
    {
        const char *destination = NULL;
        const char *host = DEFAULT_SUPERPEER_HOST;
        uint16_t port = DEFAULT_SUPERPEER_PORT;

        if (argc == 4)
        {
            destination = argv[3];
        }
        else if (argc == 5)
        {
            host = argv[3];
            if (parse_port(argv[4], &port) < 0)
            {
                goto failure;
            }
        }
        else if (argc == 6)
        {
            destination = argv[3];
            host = argv[4];
            if (parse_port(argv[5], &port) < 0)
            {
                goto failure;
            }
        }
        result = local_control_command(local_peer_port, 0, argv[2], destination, host, port);
    }
    if (result == 0)
    {
        return EXIT_SUCCESS;
    }

failure:
    if (errno != 0)
    {
        perror("peer");
    }
    usage(argv[0]);
    return EXIT_FAILURE;
}
