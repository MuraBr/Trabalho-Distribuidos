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
    const char *control = NULL;
    if (strcmp(key, "heartbeat-ms") == 0) control = "C3_HEARTBEAT_MS";
    else if (strcmp(key, "suspect-ms") == 0) control = "C3_SUSPECT_MS";
    else if (strcmp(key, "failed-ms") == 0) control = "C3_FAILED_MS";
    else if (strcmp(key, "removed-ms") == 0) control = "C3_REMOVED_MS";
    else if (strcmp(key, "control-timeout-ms") == 0) control = "C3_RPC_MS";
    if (control != NULL) return setenv(control, value, 1);
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

static int config_port(const char *text, uint16_t *port)
{
    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || number == 0UL || number > UINT16_MAX) { errno = EINVAL; return -1; }
    *port = (uint16_t)number;
    return 0;
}

static int requested_port(int argc, char **argv, uint16_t *port)
{
    for (int i = 1; i + 1 < argc; ++i) if (strcmp(argv[i], "--port") == 0) return config_port(argv[i + 1], port);
    errno = EINVAL;
    return -1;
}

static int load_superpeer_roster(FILE *file, uint16_t port)
{
    char line[1024];
    char bootstrap_host[NODE_ADDRESS_SIZE] = {0};
    uint16_t bootstrap_port = 0U;
    int matches = 0;
    rewind(file);
    while (fgets(line, sizeof(line), file) != NULL)
    {
        char *fields[6];
        char *cursor = trim(line);
        if (*cursor == '\0' || *cursor == '#') continue;
        for (size_t i = 0U; i < 5U; ++i)
        {
            fields[i] = cursor;
            char *comma = strchr(cursor, ',');
            if (comma == NULL) { errno = EINVAL; return -1; }
            *comma = '\0';
            cursor = comma + 1;
        }
        fields[5] = cursor;
        if (strchr(fields[5], ',') != NULL) { errno = EINVAL; return -1; }
        for (size_t i = 0U; i < 6U; ++i) fields[i] = trim(fields[i]);
        uint16_t entry_port;
        struct in_addr address;
        if (*fields[0] == '\0' || strcmp(fields[1], "superpeer") != 0 || inet_pton(AF_INET, fields[2], &address) != 1 || config_port(fields[3], &entry_port) < 0) { errno = EINVAL; return -1; }
        /* O primeiro registro é o ponto de entrada dos demais Super Peers. */
        if (bootstrap_port == 0U)
        {
            if (strlen(fields[2]) >= sizeof(bootstrap_host)) { errno = EINVAL; return -1; }
            strcpy(bootstrap_host, fields[2]);
            bootstrap_port = entry_port;
        }
        if (entry_port != port) continue;
        if (++matches > 1 || strlen(fields[0]) >= sizeof(app_config.node_name)) { errno = EINVAL; return -1; }
        strcpy(app_config.node_name, fields[0]);
        if (assign("ip", fields[2]) < 0 || assign("bind", fields[2]) < 0) return -1;
        app_config.port = entry_port;
    }
    if (ferror(file)) return -1;
    if (matches != 1) { errno = ENOENT; return -1; }
    if (port != bootstrap_port)
    {
        strcpy(app_config.chord_host, bootstrap_host);
        app_config.chord_port = bootstrap_port;
    }
    return 0;
}

int app_config_load(int *argc, char **argv, int superpeer)
{
    memset(&app_config, 0, sizeof(app_config));
    strcpy(app_config.advertised, "127.0.0.1");
    strcpy(app_config.bind_ip, "0.0.0.0");
    strcpy(app_config.superpeer, "127.0.0.1");
    app_config.chord_host[0] = '\0';
    app_config.port = superpeer ? 55101U : 55102U;
    app_config.superpeer_port = 55101U;
    app_config.chord_port = 0U;
    app_config.node_name[0] = '\0';
    for (int i = 1; i + 1 < *argc; ++i)
    {
        if (strcmp(argv[i], "--config") != 0 && strcmp(argv[i], "-f") != 0) continue;
        FILE *file = fopen(argv[i + 1], "r");
        if (file == NULL) return -1;
        char line[1024];
        int status = 0;
        int roster = 0;
        while (fgets(line, sizeof(line), file) != NULL)
        {
            char *content = trim(line);
            if (*content == '\0' || *content == '#') continue;
            roster = strchr(content, ',') != NULL && strchr(content, '=') == NULL;
            break;
        }
        if (roster)
        {
            uint16_t port;
            if (!superpeer || requested_port(*argc, argv, &port) < 0 || load_superpeer_roster(file, port) < 0) status = -1;
        }
        else
        {
            rewind(file);
        while (fgets(line, sizeof(line), file) != NULL)
        {
            char *key = trim(line);
            if (*key == '#' || *key == '\0') continue;
            char *equal = strchr(key, '=');
            if (equal == NULL) { errno = EINVAL; status = -1; break; }
            *equal++ = '\0';
            if (assign(trim(key), trim(equal)) < 0) { status = -1; break; }
        }
        }
        if (ferror(file)) status = -1;
        fclose(file);
        if (status < 0) return -1;
    }
    for (int i = 1; i < *argc; ++i)
    {
        const char *key = argv[i];
        int config = strcmp(key, "--config") == 0 || strcmp(key, "-f") == 0;
        int custom = strcmp(key, "--bind") == 0 || strcmp(key, "--advertise-ip") == 0 || strcmp(key, "--data-dir") == 0 || strcmp(key, "--superpeer-host") == 0 || strcmp(key, "--superpeer-port") == 0 || strcmp(key, "--heartbeat-ms") == 0 || strcmp(key, "--suspect-ms") == 0 || strcmp(key, "--failed-ms") == 0 || strcmp(key, "--removed-ms") == 0 || strcmp(key, "--control-timeout-ms") == 0;
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
