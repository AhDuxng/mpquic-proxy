#include "socket_io.h"

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>

int socket_send_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)buffer;

    while (length > 0) {
        ssize_t sent = send(fd, cursor, length, MSG_NOSIGNAL);

        if (sent > 0) {
            cursor += (size_t)sent;
            length -= (size_t)sent;
            continue;
        }

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        return -1;
    }

    return 0;
}
