#include "concurrent_server.h"

#include "network.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>

typedef struct ConcurrentConnection
{
    struct ConcurrentServer *server;
    int client_fd;
    struct ConcurrentConnection *next;
} ConcurrentConnection;

struct ConcurrentServer
{
    int server_fd;
    ConcurrentServerHandler handler;
    void *context;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    ConcurrentConnection *connections;
    int stopping;
    int listen_closed;
};

static void remove_connection(ConcurrentConnection *connection)
{
    ConcurrentServer *server = connection->server;
    ConcurrentConnection **current;

    (void)pthread_mutex_lock(&server->mutex);
    current = &server->connections;
    while (*current != NULL && *current != connection)
    {
        current = &(*current)->next;
    }
    if (*current == connection)
    {
        *current = connection->next;
    }
    (void)pthread_cond_broadcast(&server->condition);
    (void)pthread_mutex_unlock(&server->mutex);
}

static void *serve_connection(void *argument)
{
    ConcurrentConnection *connection = argument;
    ConcurrentServer *server = connection->server;

    server->handler(server->context, connection->client_fd);
    (void)network_shutdown(connection->client_fd);
    remove_connection(connection);
    free(connection);
    return NULL;
}

int concurrent_server_create(int server_fd, ConcurrentServerHandler handler, void *context, ConcurrentServer **output)
{
    ConcurrentServer *server;
    int error;

    if (server_fd < 0 || handler == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    server = calloc(1U, sizeof(*server));
    if (server == NULL)
    {
        return -1;
    }
    error = pthread_mutex_init(&server->mutex, NULL);
    if (error != 0)
    {
        free(server);
        errno = error;
        return -1;
    }
    error = pthread_cond_init(&server->condition, NULL);
    if (error != 0)
    {
        (void)pthread_mutex_destroy(&server->mutex);
        free(server);
        errno = error;
        return -1;
    }
    server->server_fd = server_fd;
    server->handler = handler;
    server->context = context;
    *output = server;
    return 0;
}

void concurrent_server_stop(ConcurrentServer *server)
{
    ConcurrentConnection *connection;

    if (server == NULL)
    {
        return;
    }
    (void)pthread_mutex_lock(&server->mutex);
    if (!server->stopping)
    {
        server->stopping = 1;
        if (!server->listen_closed)
        {
            server->listen_closed = 1;
            (void)network_shutdown(server->server_fd);
        }
        for (connection = server->connections; connection != NULL; connection = connection->next)
        {
            (void)shutdown(connection->client_fd, SHUT_RDWR);
        }
    }
    (void)pthread_mutex_unlock(&server->mutex);
}

int concurrent_server_run(ConcurrentServer *server)
{
    int status = 0;

    if (server == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    for (;;)
    {
        ConcurrentConnection *connection;
        pthread_t thread;
        int client_fd;
        int error;

        (void)pthread_mutex_lock(&server->mutex);
        if (server->stopping)
        {
            (void)pthread_mutex_unlock(&server->mutex);
            break;
        }
        (void)pthread_mutex_unlock(&server->mutex);
        client_fd = network_accept_client(server->server_fd);
        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        connection = calloc(1U, sizeof(*connection));
        if (connection == NULL)
        {
            (void)network_shutdown(client_fd);
            continue;
        }
        connection->server = server;
        connection->client_fd = client_fd;
        (void)pthread_mutex_lock(&server->mutex);
        connection->next = server->connections;
        server->connections = connection;
        (void)pthread_mutex_unlock(&server->mutex);
        error = pthread_create(&thread, NULL, serve_connection, connection);
        if (error != 0)
        {
            remove_connection(connection);
            (void)network_shutdown(client_fd);
            free(connection);
            errno = error;
            status = -1;
            continue;
        }
        (void)pthread_detach(thread);
    }
    concurrent_server_stop(server);
    (void)pthread_mutex_lock(&server->mutex);
    while (server->connections != NULL)
    {
        (void)pthread_cond_wait(&server->condition, &server->mutex);
    }
    (void)pthread_mutex_unlock(&server->mutex);
    return status;
}

void concurrent_server_destroy(ConcurrentServer *server)
{
    if (server == NULL)
    {
        return;
    }
    concurrent_server_stop(server);
    (void)pthread_cond_destroy(&server->condition);
    (void)pthread_mutex_destroy(&server->mutex);
    free(server);
}
