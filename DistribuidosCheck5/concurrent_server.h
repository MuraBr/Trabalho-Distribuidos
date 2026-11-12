#ifndef CONCURRENT_SERVER_H
#define CONCURRENT_SERVER_H

typedef struct ConcurrentServer ConcurrentServer;
typedef void (*ConcurrentServerHandler)(void *context, int client_fd);

int concurrent_server_create(int server_fd, ConcurrentServerHandler handler, void *context, ConcurrentServer **output);
int concurrent_server_run(ConcurrentServer *server);
void concurrent_server_stop(ConcurrentServer *server);
void concurrent_server_destroy(ConcurrentServer *server);

#endif
