#define _POSIX_C_SOURCE 200809L
#include "network.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int timeout_ms(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end;
    long seconds;
    if (value == NULL) return fallback;
    errno = 0;
    seconds = strtol(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' && seconds > 0 && seconds <= 3600 ? (int)seconds * 1000 : fallback;
}

static int64_t now_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static int wait_ready(int fd, short events, int64_t deadline)
{
    struct pollfd descriptor = {.fd = fd, .events = events};
    for (;;)
    {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        int result = poll(&descriptor, 1U, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (result > 0) return 0;
        if (result == 0) { errno = ETIMEDOUT; return -1; }
        if (errno != EINTR) return -1;
    }
}

int network_create_server(uint16_t porta, int backlog)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(porta)};
    const char *bind_ip = getenv("PEER_BIND_IP");
    int opt = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind_ip != NULL && inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) { close(fd); errno = EINVAL; return -1; }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, backlog) < 0)
    {
        int error = errno; close(fd); errno = error; return -1;
    }
    return fd;
}

int network_accept_client(int server_fd)
{
    return accept(server_fd, NULL, NULL);
}

int network_connect(const char *ip, uint16_t porta)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(porta)};
    int fd, flags, error = 0;
    socklen_t size = sizeof(error);
    if (ip == NULL || porta == 0U || inet_pton(AF_INET, ip, &address.sin_addr) != 1) { errno = EINVAL; return -1; }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto failure;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        if (errno != EINPROGRESS || wait_ready(fd, POLLOUT, now_ms() + timeout_ms("PEER_CONNECT_TIMEOUT", 5000)) < 0) goto failure;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) goto failure;
        if (error != 0) { errno = error; goto failure; }
    }
    if (fcntl(fd, F_SETFL, flags) < 0) goto failure;
    return fd;
failure:
    error = errno; close(fd); errno = error; return -1;
}

ssize_t network_send_all(int sock, const void *buffer, size_t size)
{
    const unsigned char *data = buffer;
    size_t done = 0U;
    int timeout = timeout_ms("PEER_IO_TIMEOUT", 30000);
    int64_t deadline = now_ms() + timeout;
    if (size > (size_t)SSIZE_MAX || (buffer == NULL && size != 0U)) { errno = EINVAL; return -1; }
    while (done < size)
    {
        ssize_t count = send(sock, data + done, size - done, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count > 0) { done += (size_t)count; deadline = now_ms() + timeout; continue; }
        if (count == 0) { errno = EPIPE; return -1; }
        if (errno == EINTR) continue;
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || wait_ready(sock, POLLOUT, deadline) < 0) return -1;
    }
    return (ssize_t)done;
}

ssize_t network_recv_exact(int sock, void *buffer, size_t size)
{
    unsigned char *data = buffer;
    size_t done = 0U;
    int timeout = timeout_ms("PEER_IO_TIMEOUT", 30000);
    int64_t deadline = now_ms() + timeout;
    if (size > (size_t)SSIZE_MAX || (buffer == NULL && size != 0U)) { errno = EINVAL; return -1; }
    while (done < size)
    {
        ssize_t count = recv(sock, data + done, size - done, MSG_DONTWAIT);
        if (count > 0) { done += (size_t)count; deadline = now_ms() + timeout; continue; }
        if (count == 0) return (ssize_t)done;
        if (errno == EINTR) continue;
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || wait_ready(sock, POLLIN, deadline) < 0) return -1;
    }
    return (ssize_t)done;
}

int network_shutdown(int sock)
{
    (void)shutdown(sock, SHUT_RDWR);
    return close(sock);
}
