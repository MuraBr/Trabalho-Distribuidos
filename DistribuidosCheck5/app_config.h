#ifndef APP_CONFIG_H
#define APP_CONFIG_H
#include "node.h"
typedef struct
{
    char advertised[NODE_ADDRESS_SIZE];
    char bind_ip[NODE_ADDRESS_SIZE];
    char superpeer[NODE_ADDRESS_SIZE];
    char chord_host[NODE_ADDRESS_SIZE];
    char data_dir[512];
    char node_name[128];
    uint16_t port, superpeer_port, chord_port;
} AppConfig;
extern AppConfig app_config;
int app_config_load(int *argc, char **argv, int superpeer);
int app_identity(const char *directory, NodeConfig *config, uint16_t port);
#endif
