#define _GNU_SOURCE
#include "local_control.h"
#include "file_client.h"
#include "network.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_LIMIT (5U * 1024U * 1024U)
struct LocalControl
{
    int listener;
    int lock;
    int client;
    int stopping;
    pthread_mutex_t mutex;
    pthread_t thread;
    NodeID source;
    char path[108];
};

/* Diretório privado impede que outro usuário injete comandos ou substitua o socket. */
static int control_path(uint16_t port, char path[108])
{
    char directory[80];
    struct stat info;
    (void)snprintf(directory, sizeof(directory), "/tmp/pd-c2-%lu", (unsigned long)getuid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    if (lstat(directory, &info) < 0) return -1;
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077U) != 0U) { errno = EACCES; return -1; }
    (void)snprintf(path, 108U, "%s/peer-%u.sock", directory, (unsigned)port);
    return 0;
}

static int send_string(int fd, const char *text)
{
    size_t size = strlen(text);
    uint32_t length;
    if (size > CONTROL_LIMIT) { errno = EOVERFLOW; return -1; }
    length = htonl((uint32_t)size);
    return network_send_all(fd, &length, sizeof(length)) == (ssize_t)sizeof(length) && network_send_all(fd, text, size) == (ssize_t)size ? 0 : -1;
}

static int receive_string(int fd, char **output, uint32_t limit)
{
    uint32_t length;
    char *text;
    *output = NULL;
    if (network_recv_exact(fd, &length, sizeof(length)) != (ssize_t)sizeof(length)) return -1;
    length = ntohl(length);
    if (length > limit) { errno = EMSGSIZE; return -1; }
    text = calloc((size_t)length + 1U, 1U);
    if (text == NULL) return -1;
    if (network_recv_exact(fd, text, length) != (ssize_t)length || memchr(text, 0, length) != NULL) { free(text); errno = EBADMSG; return -1; }
    *output = text;
    return 0;
}

/* Cada escrita gera um frame de progresso; o último frame contém o resultado. */
static ssize_t progress_write(void *cookie, const char *buffer, size_t size)
{
    int fd = *(int *)cookie;
    if (size > CONTROL_LIMIT) { errno = EMSGSIZE; return -1; }
    uint32_t header[2] = {htonl(UINT32_MAX), htonl((uint32_t)size)};
    if (network_send_all(fd, header, sizeof(header)) != (ssize_t)sizeof(header) || network_send_all(fd, buffer, size) != (ssize_t)size) return -1;
    return (ssize_t)size;
}

static void handle_command(LocalControl *control, int fd)
{
    uint32_t header[2];
    char *fields[5] = {NULL};
    FILE *stream = NULL;
    int result = -1;
    int error = EBADMSG;
    if (network_recv_exact(fd, header, sizeof(header)) != (ssize_t)sizeof(header)) goto cleanup;
    for (size_t i = 0U; i < 5U; ++i) if (receive_string(fd, &fields[i], 4095U) < 0) goto cleanup;
    if (ntohl(header[0]) > 1U || ntohl(header[1]) == 0U || ntohl(header[1]) > UINT16_MAX || fields[4][0] != '/' || fields[0][0] == '\0') goto cleanup;
    cookie_io_functions_t io = {.write = progress_write};
    stream = fopencookie(&fd, "w", io);
    if (stream != NULL) setvbuf(stream, NULL, _IOLBF, 0U);
    if (stream == NULL) { error = errno; goto cleanup; }
    FileSession session = {.source = control->source, .output = stream, .directory = fields[4]};
    if (ntohl(header[0]) == 1U)
        result = file_client_upload(&session, fields[0], fields[2], (uint16_t)ntohl(header[1]));
    else
        result = file_client_download(&session, fields[0], fields[1][0] == '\0' ? NULL : fields[1], fields[2], (uint16_t)ntohl(header[1]));
    error = result == 0 ? 0 : (errno == 0 ? EIO : errno);
    if (error != 0) (void)fprintf(stream, "Operação rejeitada: %s\n", strerror(error));
    if (fclose(stream) != 0 && error == 0) error = EIO;
    stream = NULL;
cleanup:
    header[0] = htonl((uint32_t)error);
    (void)network_send_all(fd, header, sizeof(header[0]));
    (void)send_string(fd, error == EBADMSG ? "Comando local inválido\n" : "");
    if (stream != NULL) fclose(stream);
    for (size_t i = 0U; i < 5U; ++i) free(fields[i]);
}

static void *control_loop(void *argument)
{
    LocalControl *control = argument;
    for (;;)
    {
        int fd = accept(control->listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        pthread_mutex_lock(&control->mutex);
        if (control->stopping) { pthread_mutex_unlock(&control->mutex); close(fd); break; }
        control->client = fd;
        pthread_mutex_unlock(&control->mutex);
        handle_command(control, fd);
        pthread_mutex_lock(&control->mutex);
        control->client = -1;
        close(fd);
        pthread_mutex_unlock(&control->mutex);
    }
    return NULL;
}

int local_control_start(uint16_t port, const NodeID *source, LocalControl **output)
{
    LocalControl *control = calloc(1U, sizeof(*control));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    char lock_path[120];
    int error;
    if (control == NULL) return -1;
    *output = NULL;
    control->listener = -1;
    control->lock = -1;
    control->client = -1;
    control->source = *source;
    if (control_path(port, control->path) < 0) goto fail;
    (void)snprintf(lock_path, sizeof(lock_path), "%s.lock", control->path);
    control->lock = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (control->lock < 0 || flock(control->lock, LOCK_EX | LOCK_NB) < 0) goto fail;
    control->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (control->listener < 0) goto fail;
    strcpy(address.sun_path, control->path);
    (void)unlink(control->path); /* Lock exclusivo permite remover somente um socket antigo deste Peer. */
    if (bind(control->listener, (struct sockaddr *)&address, sizeof(address)) < 0 || chmod(control->path, 0600) < 0 || listen(control->listener, 64) < 0) goto fail;
    error = pthread_mutex_init(&control->mutex, NULL);
    if (error != 0) { errno = error; goto fail; }
    error = pthread_create(&control->thread, NULL, control_loop, control);
    if (error != 0) { pthread_mutex_destroy(&control->mutex); errno = error; goto fail; }
    *output = control;
    return 0;
fail:
    error = errno;
    if (control->listener >= 0) close(control->listener);
    if (control->lock >= 0) close(control->lock);
    free(control);
    errno = error;
    return -1;
}

void local_control_stop(LocalControl *control)
{
    if (control == NULL) return;
    pthread_mutex_lock(&control->mutex);
    control->stopping = 1;
    if (control->client >= 0) shutdown(control->client, SHUT_RDWR);
    pthread_mutex_unlock(&control->mutex);
    shutdown(control->listener, SHUT_RDWR);
    pthread_join(control->thread, NULL);
    close(control->listener);
    unlink(control->path);
    close(control->lock);
    pthread_mutex_destroy(&control->mutex);
    free(control);
}

static int absolute_path(const char *path, const char *directory, char output[PATH_MAX])
{
    int length = snprintf(output, PATH_MAX, "%s%s%s", path[0] == '/' ? "" : directory, path[0] == '/' ? "" : "/", path);
    if (length < 0 || length >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int local_control_command(uint16_t local_port, int upload, const char *file, const char *destination, const char *host, uint16_t remote_port)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    char cwd[PATH_MAX], source[PATH_MAX], target[PATH_MAX] = "";
    uint32_t header[2] = {htonl(upload ? 1U : 0U), htonl(remote_port)};
    char *report = NULL;
    int result = -1;
    int fd;
    if (getcwd(cwd, sizeof(cwd)) == NULL || control_path(local_port, address.sun_path) < 0) return -1;
    if (upload && absolute_path(file, cwd, source) < 0) return -1;
    if (destination != NULL && absolute_path(destination, cwd, target) < 0) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        fprintf(stderr, "Peer local %u indisponível; inicie peer serve ou selecione --local-peer-port.\n", (unsigned)local_port);
        goto cleanup;
    }
    if (network_send_all(fd, header, sizeof(header)) != (ssize_t)sizeof(header) || send_string(fd, upload ? source : file) < 0 || send_string(fd, target) < 0 || send_string(fd, host) < 0 || send_string(fd, "") < 0 || send_string(fd, cwd) < 0) goto cleanup;
    for (;;)
    {
        /* A operação pode levar minutos; EOF detecta encerramento do serviço. */
        struct pollfd waiting = {.fd = fd, .events = POLLIN};
        int ready;
        do { ready = poll(&waiting, 1U, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0 || network_recv_exact(fd, header, sizeof(header[0])) != (ssize_t)sizeof(header[0]) || receive_string(fd, &report, CONTROL_LIMIT) < 0) goto cleanup;
        fputs(report, stdout);
        fflush(stdout);
        free(report);
        report = NULL;
        if (ntohl(header[0]) != UINT32_MAX) break;
    }
    errno = (int)ntohl(header[0]);
    result = errno == 0 ? 0 : -1;
cleanup:
    { int error = errno; free(report); close(fd); errno = error; }
    return result;
}
