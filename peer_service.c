#define _POSIX_C_SOURCE 200809L
#include "peer_service.h"

#include "concurrent_server.h"
#include "network.h"
#include "node.h"
#include "protocol.h"
#include "rpc.h"
#include "storage.h"
#include "transfer_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
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
    Storage *storage;
    ConcurrentServer *runtime;
    char superpeer_host[NODE_ADDRESS_SIZE];
    uint16_t superpeer_port;
} PeerService;

static int service_server_fd = -1;

static void stop_service(int signal_number)
{
    (void)signal_number;
    if (service_server_fd >= 0)
    {
        (void)shutdown(service_server_fd, SHUT_RDWR);
    }
}

static int load_or_create_uuid(const char *storage_path, uint8_t uuid[NODE_UUID_SIZE])
{
    char path[PEER_STORAGE_PATH_SIZE + 16U];
    size_t received = 0U;
    int fd;
    int length;

    if ((mkdir(".peer_storage", 0700) < 0 && errno != EEXIST) || (mkdir(storage_path, 0700) < 0 && errno != EEXIST))
    {
        return -1;
    }
    length = snprintf(path, sizeof(path), "%s/node.uuid", storage_path);
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0)
    {
        if (node_generate_uuid(uuid) < 0)
        {
            (void)close(fd);
            (void)unlink(path);
            return -1;
        }
        while (received < NODE_UUID_SIZE)
        {
            ssize_t count = write(fd, uuid + received, NODE_UUID_SIZE - received);

            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
                (void)close(fd);
                (void)unlink(path);
                errno = EIO;
                return -1;
            }
            received += (size_t)count;
        }
        if (fsync(fd) < 0)
        {
            (void)close(fd);
            (void)unlink(path);
            return -1;
        }
        if (close(fd) < 0)
        {
            (void)unlink(path);
            return -1;
        }
        return 0;
    }
    if (errno != EEXIST)
    {
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }
    while (received < NODE_UUID_SIZE)
    {
        ssize_t count = read(fd, uuid + received, NODE_UUID_SIZE - received);

        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            (void)close(fd);
            errno = EBADMSG;
            return -1;
        }
        received += (size_t)count;
    }
    {
        uint8_t extra;
        ssize_t count = read(fd, &extra, 1U);

        if (count != 0)
        {
            (void)close(fd);
            errno = EBADMSG;
            return -1;
        }
    }
    return close(fd);
}

static int send_response(PeerService *service, int socket_fd, const Message *request, Message_Type type, const uint8_t *payload, uint32_t payload_size)
{
    Message response;
    int status;

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
    uint8_t payload[JOIN_PAYLOAD_WIRE_SIZE];
    Message response;
    int status;

    if (rpc_encode_join_payload(&service->node.config, payload) < 0 || rpc_call(service->superpeer_host, service->superpeer_port, &service->node.id, NULL, M_JOIN, payload, JOIN_PAYLOAD_WIRE_SIZE, &response) < 0)
    {
        return -1;
    }
    status = response.header.message_type == (uint8_t)M_ACK ? 0 : -1;
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

    if (transfer_encode_document(document, &payload, &payload_size, TRANSFER_STORE_ANNOUNCE) < 0 || rpc_call(service->superpeer_host, service->superpeer_port, &service->node.id, NULL, M_STORE, payload, payload_size, &response) < 0)
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
        errno = EREMOTEIO;
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
    int status;

    if (transfer_decode_chunk_request(message->payload, message->header.payload_size, &id, &index) < 0 || storage_read_chunk(service->storage, &id, index, &chunk, &owned_data) < 0 || transfer_encode_chunk(&chunk, TRANSFER_DOWNLOAD_CHUNK, &payload, &payload_size) < 0)
    {
        free(owned_data);
        free(payload);
        return send_response(service, socket_fd, message, M_ERROR, NULL, 0U);
    }
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
        if (message.header.message_type == (uint8_t)M_STORE)
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
            (void)send_response(service, socket_fd, &message, M_ERROR, NULL, 0U);
        }
    }
    message_free(&message);
}

static int initialize_service(PeerService *service, uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port)
{
    NodeConfig config;
    uint8_t uuid[NODE_UUID_SIZE];
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
    if (load_or_create_uuid(storage_path, uuid) < 0 || node_config_init_with_uuid(&config, "127.0.0.1", local_port, uuid) < 0 || node_init(&service->node, &config) < 0)
    {
        return -1;
    }
    strcpy(service->superpeer_host, superpeer_host);
    service->superpeer_port = superpeer_port;
    if (storage_create(storage_path, &service->node.id, &service->storage) < 0)
    {
        return -1;
    }
    return 0;
}

int peer_service_run(uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port)
{
    PeerService service;
    int status = EXIT_FAILURE;

    if (initialize_service(&service, local_port, superpeer_host, superpeer_port) < 0)
    {
        perror("peer initialization");
        return EXIT_FAILURE;
    }
    if (join_superpeer(&service) < 0 || announce_catalog(&service) < 0)
    {
        perror("superpeer registration");
        goto cleanup;
    }
    service.server_fd = network_create_server(local_port, PEER_BACKLOG);
    if (service.server_fd < 0 || concurrent_server_create(service.server_fd, serve_connection, &service, &service.runtime) < 0)
    {
        goto cleanup;
    }
    service_server_fd = service.server_fd;
    (void)signal(SIGINT, stop_service);
    (void)signal(SIGTERM, stop_service);
    printf("Peer storage started\nNodeID: ");
    for (size_t index = 0U; index < NODE_ID_SIZE; ++index)
    {
        printf("%02x", (unsigned)service.node.id.bytes[index]);
    }
    printf("\nStorage: .peer_storage/%" PRIu16 "\n", local_port);
    fflush(stdout);
    status = concurrent_server_run(service.runtime) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    service_server_fd = -1;
    if (service.runtime != NULL)
    {
        concurrent_server_destroy(service.runtime);
    }
    else if (service.server_fd >= 0)
    {
        (void)network_shutdown(service.server_fd);
    }
    storage_destroy(service.storage);
    return status;
}
