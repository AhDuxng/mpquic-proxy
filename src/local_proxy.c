#define _GNU_SOURCE

#include "local_proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mpquic_client.h"
#include "socket_io.h"

#define LISTEN_ADDR "127.0.0.1"
#define LISTEN_PORT 8080
#define LISTEN_BACKLOG 32
#define REQUEST_MAX 32768
#define PATH_MAX_LOCAL 8192
#define RANGE_MAX_LOCAL 512

typedef struct {
    int client_fd;
} worker_arg_t;

static void send_local_error(
    int fd,
    int status,
    const char *reason,
    const char *body)
{
    char header[1024];
    size_t body_len = strlen(body);
    int length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status,
        reason,
        body_len);

    if (length > 0 && (size_t)length < sizeof(header)) {
        (void)socket_send_all(fd, header, (size_t)length);
        (void)socket_send_all(fd, body, body_len);
    }
}

static int extract_range_header(
    const char *request,
    char *output,
    size_t output_size)
{
    const char *line = strstr(request, "\r\n");

    if (line == NULL) {
        return 0;
    }

    line += 2;

    while (*line != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        size_t line_length;

        if (end == NULL) {
            break;
        }

        line_length = (size_t)(end - line);

        if (line_length >= 6 && strncasecmp(line, "Range:", 6) == 0) {
            const char *value = line + 6;
            const char *value_end = end;
            size_t length;

            while (value < value_end &&
                   (*value == ' ' || *value == '\t')) {
                value++;
            }

            while (value_end > value &&
                   (value_end[-1] == ' ' || value_end[-1] == '\t')) {
                value_end--;
            }

            length = (size_t)(value_end - value);
            if (length >= output_size) {
                length = output_size - 1;
            }

            memcpy(output, value, length);
            output[length] = '\0';
            return 1;
        }

        line = end + 2;
    }

    return 0;
}

static int receive_request_headers(int fd, char *request, size_t capacity)
{
    size_t used = 0;

    for (;;) {
        ssize_t received;

        if (used >= capacity - 1) {
            return 1;
        }

        received = recv(fd, request + used, capacity - 1 - used, 0);

        if (received > 0) {
            used += (size_t)received;
            request[used] = '\0';

            if (strstr(request, "\r\n\r\n") != NULL) {
                return 0;
            }

            continue;
        }

        if (received < 0 && errno == EINTR) {
            continue;
        }

        return -1;
    }
}

static void handle_client(int fd)
{
    char request[REQUEST_MAX + 1];
    char method[16];
    char path[PATH_MAX_LOCAL];
    char version[32];
    char range[RANGE_MAX_LOCAL];
    int has_range;
    int receive_result = receive_request_headers(fd, request, sizeof(request));

    if (receive_result > 0) {
        send_local_error(fd, 431, "Request Header Fields Too Large",
                         "request headers too large\n");
        return;
    }

    if (receive_result < 0) {
        return;
    }

    if (sscanf(request, "%15s %8191s %31s", method, path, version) != 3) {
        send_local_error(fd, 400, "Bad Request", "cannot parse request line\n");
        return;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        static const char reply[] =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Range, Content-Type\r\n"
            "Access-Control-Expose-Headers: Content-Range, Accept-Ranges\r\n"
            "Connection: close\r\n"
            "\r\n";

        (void)socket_send_all(fd, reply, sizeof(reply) - 1);
        return;
    }

    if (strcmp(method, "GET") != 0) {
        send_local_error(fd, 405, "Method Not Allowed",
                         "this MPQUIC proxy currently supports GET only\n");
        return;
    }

    if (path[0] != '/') {
        send_local_error(fd, 400, "Bad Request",
                         "only origin-form request targets are supported\n");
        return;
    }

    range[0] = '\0';
    has_range = extract_range_header(request, range, sizeof(range));

    fprintf(stdout, "HTTP -> MPQUIC: GET %s%s%s\n",
            path,
            has_range ? " Range: " : "",
            has_range ? range : "");
    fflush(stdout);

    /* The network thread streams the H3 response directly to this socket. */
    (void)mpquic_client_fetch(fd, path, has_range ? range : NULL);
}

static void *client_worker(void *argument)
{
    worker_arg_t *worker = (worker_arg_t *)argument;
    int fd = worker->client_fd;

    free(worker);
    handle_client(fd);
    close(fd);
    return NULL;
}

static int create_listen_socket(void)
{
    int server_fd;
    int option = 1;
    struct sockaddr_in address;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    (void)setsockopt(
        server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(LISTEN_PORT);

    if (inet_pton(AF_INET, LISTEN_ADDR, &address.sin_addr) != 1) {
        fprintf(stderr, "invalid listen address\n");
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

int local_proxy_run(volatile sig_atomic_t *stop_requested)
{
    int server_fd = create_listen_socket();

    if (server_fd < 0) {
        return -1;
    }

    printf("MPQUIC proxy listening on http://%s:%d\n",
           LISTEN_ADDR, LISTEN_PORT);
    fflush(stdout);

    while (!*stop_requested) {
        int client_fd = accept(server_fd, NULL, NULL);
        worker_arg_t *worker;
        pthread_t thread;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        worker = (worker_arg_t *)calloc(1, sizeof(*worker));
        if (worker == NULL) {
            send_local_error(client_fd, 503, "Service Unavailable",
                             "out of memory\n");
            close(client_fd);
            continue;
        }

        worker->client_fd = client_fd;

        if (pthread_create(&thread, NULL, client_worker, worker) != 0) {
            send_local_error(client_fd, 503, "Service Unavailable",
                             "cannot create worker thread\n");
            close(client_fd);
            free(worker);
            continue;
        }

        pthread_detach(thread);
    }

    close(server_fd);
    return 0;
}
