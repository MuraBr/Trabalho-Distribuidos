#define _POSIX_C_SOURCE 200809L
#include "app_config.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

AppConfig app_config;

static int assign(const char *key, const char *value)
{
    char *target = NULL;
    size_t capacity = NODE_ADDRESS_SIZE;
    if (strcmp(key, "ip") == 0 || strcmp(key, "advertise-ip") == 0) target = app_config.advertised;
    else if (strcmp(key, "bind") == 0) target = app_config.bind_ip;
    else if (strcmp(key, "superpeer-host") == 0) target = app_config.superpeer;
    else if (strcmp(key, "data-dir") == 0) { target = app_config.data_dir; capacity = sizeof(app_config.data_dir); }
    else if (strcmp(key, "port") == 0 || strcmp(key, "superpeer-port") == 0)
    {
        char *end;
        errno = 0;
        unsigned long number = strtoul(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' || number == 0UL || number > UINT16_MAX) { errno = EINVAL; return -1; }
        if (strcmp(key, "port") == 0) app_config.port = (uint16_t)number;
        else app_config.superpeer_port = (uint16_t)number;
        return 0;
    }
    else { errno = EINVAL; return -1; }
    if (strlen(value) == 0U || strlen(value) >= capacity) { errno = EINVAL; return -1; }
    if (target != app_config.data_dir) { struct in_addr address; if (inet_pton(AF_INET, value, &address) != 1) { errno = EINVAL; return -1; } }
    strcpy(target, value);
    return 0;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

int app_config_load(int *argc, char **argv, int superpeer)
{
    memset(&app_config, 0, sizeof(app_config));
    strcpy(app_config.advertised, "127.0.0.1");
    strcpy(app_config.bind_ip, "0.0.0.0");
    strcpy(app_config.superpeer, "127.0.0.1");
    app_config.port = superpeer ? 55101U : 55102U;
    app_config.superpeer_port = 55101U;
    for (int i = 1; i + 1 < *argc; ++i)
    {
        if (strcmp(argv[i], "--config") != 0 && strcmp(argv[i], "-f") != 0) continue;
        FILE *file = fopen(argv[i + 1], "r");
        if (file == NULL) return -1;
        char line[1024];
        int status = 0;
        while (fgets(line, sizeof(line), file) != NULL)
        {
            char *key = trim(line);
            if (*key == '#' || *key == '\0') continue;
            char *equal = strchr(key, '=');
            if (equal == NULL) { errno = EINVAL; status = -1; break; }
            *equal++ = '\0';
            if (assign(trim(key), trim(equal)) < 0) { status = -1; break; }
        }
        if (ferror(file)) status = -1;
        fclose(file);
        if (status < 0) return -1;
    }
    for (int i = 1; i < *argc; ++i)
    {
        const char *key = argv[i];
        int config = strcmp(key, "--config") == 0 || strcmp(key, "-f") == 0;
        int custom = strcmp(key, "--bind") == 0 || strcmp(key, "--advertise-ip") == 0 || strcmp(key, "--data-dir") == 0 || strcmp(key, "--superpeer-host") == 0 || strcmp(key, "--superpeer-port") == 0;
        if (!superpeer && *argc > 1 && strcmp(argv[1], "serve") == 0 && strcmp(key, "--port") == 0) custom = 1;
        if (!config && !custom) continue;
        if (i + 1 >= *argc) { errno = EINVAL; return -1; }
        if (custom && assign(key + 2, argv[i + 1]) < 0) return -1;
        for (int j = i; j + 2 < *argc; ++j) argv[j] = argv[j + 2];
        *argc -= 2;
        argv[*argc] = NULL;
        --i;
    }
    return setenv("PEER_BIND_IP", app_config.bind_ip, 1);
}

/* UUID persistente: criação exclusiva, leitura exata e diretórios privados. */
int app_identity(const char *directory, NodeConfig *config, uint16_t port)
{
    char path[600];
    uint8_t uuid[NODE_UUID_SIZE];
    if (strlen(directory) >= sizeof(path) - 12U) { errno = ENAMETOOLONG; return -1; }
    strcpy(path, directory);
    for (char *p = path + 1; *p != '\0'; ++p)
    {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    strcat(path, "/node.uuid");
    int created = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) created = 1;
    else if (errno == EEXIST) fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (created && node_generate_uuid(uuid) < 0) { close(fd); unlink(path); return -1; }
    size_t done = 0U;
    while (done < sizeof(uuid))
    {
        ssize_t count = created ? write(fd, uuid + done, sizeof(uuid) - done) : read(fd, uuid + done, sizeof(uuid) - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); if (created) unlink(path); errno = EBADMSG; return -1; }
        done += (size_t)count;
    }
    uint8_t extra;
    if ((!created && read(fd, &extra, 1U) != 0) || (created && fsync(fd) < 0)) { close(fd); errno = EBADMSG; return -1; }
    if (close(fd) < 0) return -1;
    if (created)
    {
        int directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
        if (directory_fd < 0) return -1;
        int result = fsync(directory_fd);
        close(directory_fd);
        if (result < 0) return -1;
    }
    return node_config_init_with_uuid(config, app_config.advertised, port, uuid);
}
