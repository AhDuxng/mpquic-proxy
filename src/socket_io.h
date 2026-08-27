#ifndef SOCKET_IO_H
#define SOCKET_IO_H

#include <stddef.h>

int socket_send_all(int fd, const void *buffer, size_t length);

#endif
