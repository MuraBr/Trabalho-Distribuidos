#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int network_create_server(uint16_t porta, int backlog);
int network_accept_client(int server_fd);
int network_connect(const char *ip, uint16_t porta);
ssize_t network_send_all(int sock, const void *buffer, size_t tam);
ssize_t network_recv_exact(int sock, void *buffer, size_t tam);
int network_shutdown(int sock);

#endif /* NETWORK_H */
