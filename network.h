#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

int network_create_server(uint16_t, int);
int network_accept_client(int);
int network_connect(const char *, uint16_t);
ssize_t network_send_all(int, const char *, size_t);
ssize_t network_recv_exact(int, void *, size_t);
int network_shutdown(int);
