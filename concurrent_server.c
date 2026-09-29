#include "concurrent_server.h"
#include "network.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_WORKERS 32U
#define SERVER_QUEUE 64U
typedef struct { struct ConcurrentServer *server; size_t index; } Worker;
struct ConcurrentServer
{
    int server_fd;
    ConcurrentServerHandler handler;
    void *context;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    int stopping;
    int queue[SERVER_QUEUE];
    size_t head, count, started;
    int active[SERVER_WORKERS];
    pthread_t threads[SERVER_WORKERS];
    Worker workers[SERVER_WORKERS];
};

static void *worker_run(void *argument)
{
    Worker *worker = argument;
    ConcurrentServer *server = worker->server;
    for (;;)
    {
        int fd;
        pthread_mutex_lock(&server->mutex);
        while (server->count == 0U && !server->stopping) pthread_cond_wait(&server->ready, &server->mutex);
        if (server->stopping) { pthread_mutex_unlock(&server->mutex); break; }
        fd = server->queue[server->head];
        server->head = (server->head + 1U) % SERVER_QUEUE;
        --server->count;
        server->active[worker->index] = fd;
        pthread_mutex_unlock(&server->mutex);
        server->handler(server->context, fd);
        pthread_mutex_lock(&server->mutex);
        server->active[worker->index] = -1;
        (void)network_shutdown(fd);
        pthread_mutex_unlock(&server->mutex);
    }
    return NULL;
}

int concurrent_server_create(int server_fd, ConcurrentServerHandler handler, void *context, ConcurrentServer **output)
{
    ConcurrentServer *server;
    int error;
    if (server_fd < 0 || handler == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = NULL;
    server = calloc(1U, sizeof(*server));
    if (server == NULL) return -1;
    server->server_fd = server_fd;
    server->handler = handler;
    server->context = context;
    for (size_t i = 0U; i < SERVER_WORKERS; ++i) server->active[i] = -1;
    error = pthread_mutex_init(&server->mutex, NULL);
    if (error != 0) { free(server); errno = error; return -1; }
    error = pthread_cond_init(&server->ready, NULL);
    if (error != 0) { pthread_mutex_destroy(&server->mutex); free(server); errno = error; return -1; }
    for (size_t i = 0U; i < SERVER_WORKERS; ++i)
    {
        server->workers[i].server = server;
        server->workers[i].index = i;
        error = pthread_create(&server->threads[i], NULL, worker_run, &server->workers[i]);
        if (error != 0)
        {
            pthread_mutex_lock(&server->mutex);
            server->stopping = 1;
            pthread_cond_broadcast(&server->ready);
            pthread_mutex_unlock(&server->mutex);
            for (size_t j = 0U; j < server->started; ++j) pthread_join(server->threads[j], NULL);
            pthread_cond_destroy(&server->ready);
            pthread_mutex_destroy(&server->mutex);
            free(server);
            errno = error;
            return -1;
        }
        ++server->started;
    }
    *output = server;
    return 0;
}

void concurrent_server_stop(ConcurrentServer *server)
{
    if (server == NULL) return;
    pthread_mutex_lock(&server->mutex);
    server->stopping = 1;
    (void)shutdown(server->server_fd, SHUT_RDWR);
    for (size_t i = 0U; i < SERVER_WORKERS; ++i) if (server->active[i] >= 0) (void)shutdown(server->active[i], SHUT_RDWR);
    while (server->count != 0U)
    {
        close(server->queue[server->head]);
        server->head = (server->head + 1U) % SERVER_QUEUE;
        --server->count;
    }
    pthread_cond_broadcast(&server->ready);
    pthread_mutex_unlock(&server->mutex);
}

int concurrent_server_run(ConcurrentServer *server)
{
    if (server == NULL) { errno = EINVAL; return -1; }
    for (;;)
    {
        int fd = network_accept_client(server->server_fd);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        pthread_mutex_lock(&server->mutex);
        if (server->stopping || server->count == SERVER_QUEUE) close(fd);
        else
        {
            server->queue[(server->head + server->count) % SERVER_QUEUE] = fd;
            ++server->count;
            pthread_cond_signal(&server->ready);
        }
        int stopping = server->stopping;
        pthread_mutex_unlock(&server->mutex);
        if (stopping) break;
    }
    concurrent_server_stop(server);
    return 0;
}

void concurrent_server_destroy(ConcurrentServer *server)
{
    if (server == NULL) return;
    concurrent_server_stop(server);
    for (size_t i = 0U; i < server->started; ++i) pthread_join(server->threads[i], NULL);
    close(server->server_fd);
    pthread_cond_destroy(&server->ready);
    pthread_mutex_destroy(&server->mutex);
    free(server);
}
