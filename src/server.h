#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <stdbool.h>

int
start_server(const char * const directory, uint16_t port, bool is_daemon);

#endif
