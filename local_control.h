#ifndef LOCAL_CONTROL_H
#define LOCAL_CONTROL_H
#include "node.h"
#include <stdint.h>
typedef struct LocalControl LocalControl;
int local_control_start(uint16_t port, const NodeID *source, LocalControl **output);
void local_control_stop(LocalControl *control);
int local_control_command(uint16_t local_port, int upload, const char *file, const char *destination, const char *host, uint16_t remote_port);
#endif
