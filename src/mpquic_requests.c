#define _GNU_SOURCE

#include "mpquic_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include "h3zero_common.h"
#include "mpquic_client.h"
#include "socket_io.h"

#define H3_REQUEST_BUFFER 4096

static const char *mime_from_path(const char *path)
{
    const char *query;
    size_t length;
    char clean_path[4096];
    const char *extension;

    if (path == NULL) {
        return "application/octet-stream";
    }

    query = strchr(path, '?');
    length = query == NULL ? strlen(path) : (size_t)(query - path);
    if (length >= sizeof(clean_path)) {
        length = sizeof(clean_path) - 1;
    }

    memcpy(clean_path, path, length);
    clean_path[length] = '\0';
    extension = strrchr(clean_path, '.');

    if (extension == NULL) {
        if (strcmp(clean_path, "/") == 0) {
            return "text/html; charset=utf-8";
        }
        if (strncmp(clean_path, "/api/", 5) == 0) {
            return "application/json";
        }
        return "application/octet-stream";
    }

    if (strcasecmp(extension, ".mpd") == 0) {
        return "application/dash+xml";
    }
    if (strcasecmp(extension, ".m3u8") == 0) {
        return "application/vnd.apple.mpegurl";
    }
    if (strcasecmp(extension, ".m4s") == 0 ||
        strcasecmp(extension, ".mp4") == 0) {
        return strcasestr(clean_path, "audio") != NULL
            ? "audio/mp4"
            : "video/mp4";
    }
    if (strcasecmp(extension, ".m4a") == 0) return "audio/mp4";
    if (strcasecmp(extension, ".webm") == 0) return "video/webm";
    if (strcasecmp(extension, ".html") == 0 ||
        strcasecmp(extension, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(extension, ".js") == 0 ||
        strcasecmp(extension, ".mjs") == 0) {
        return "text/javascript; charset=utf-8";
    }
    if (strcasecmp(extension, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(extension, ".json") == 0) return "application/json";
    if (strcasecmp(extension, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(extension, ".png") == 0) return "image/png";
    if (strcasecmp(extension, ".jpg") == 0 ||
        strcasecmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(extension, ".gif") == 0) return "image/gif";
    if (strcasecmp(extension, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(extension, ".txt") == 0) {
        return "text/plain; charset=utf-8";
    }

    return "application/octet-stream";
}

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Upstream Response";
    }
}

int mpquic_request_send_headers(proxy_request_t *request)
{
    char header[2048];
    int status = request->stream_state.header.status;
    const char *mime = mime_from_path(request->path);
    int length;

    if (request->headers_sent) {
        return 0;
    }

    if (status <= 0) {
        status = 200;
    }

    length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Accept-Ranges: bytes\r\n"
        "Cache-Control: no-store\r\n"
        "X-Proxy-Transport: MPQUIC\r\n"
        "Connection: close\r\n"
        "\r\n",
        status,
        reason_phrase(status),
        mime);

    if (length <= 0 || (size_t)length >= sizeof(header)) {
        return -1;
    }

    if (socket_send_all(request->client_fd, header, (size_t)length) != 0) {
        return -1;
    }

    request->headers_sent = 1;
    return 0;
}

int mpquic_request_send_chunk(
    proxy_request_t *request,
    const uint8_t *data,
    size_t length)
{
    char prefix[64];
    int prefix_length;

    if (!request->headers_sent &&
        mpquic_request_send_headers(request) != 0) {
        return -1;
    }

    prefix_length = snprintf(prefix, sizeof(prefix), "%zx\r\n", length);
    if (prefix_length <= 0 ||
        (size_t)prefix_length >= sizeof(prefix)) {
        return -1;
    }

    if (socket_send_all(
            request->client_fd, prefix, (size_t)prefix_length) != 0 ||
        socket_send_all(request->client_fd, data, length) != 0 ||
        socket_send_all(request->client_fd, "\r\n", 2) != 0) {
        return -1;
    }

    return 0;
}

static void remove_active_request(proxy_request_t *request)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    proxy_request_t **cursor;

    pthread_mutex_lock(&client->queue_lock);

    cursor = &client->active_head;
    while (*cursor != NULL) {
        if (*cursor == request) {
            *cursor = request->next;
            request->next = NULL;
            break;
        }
        cursor = &(*cursor)->next;
    }

    pthread_mutex_unlock(&client->queue_lock);
}

void mpquic_request_finish(proxy_request_t *request, int failed)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    int already_finished;

    pthread_mutex_lock(&request->lock);
    already_finished = request->finished;
    pthread_mutex_unlock(&request->lock);

    if (already_finished) {
        return;
    }

    if (!failed) {
        if (!request->headers_sent &&
            mpquic_request_send_headers(request) != 0) {
            failed = 1;
        }

        if (!failed &&
            socket_send_all(request->client_fd, "0\r\n\r\n", 5) != 0) {
            failed = 1;
        }
    }

    if (client->cnx != NULL && request->stream_id != UINT64_MAX) {
        picoquic_unlink_app_stream_ctx(client->cnx, request->stream_id);
    }

    remove_active_request(request);
    h3zero_delete_data_stream_state(&request->stream_state);

    pthread_mutex_lock(&request->lock);
    if (!request->finished) {
        request->failed = failed;
        request->finished = 1;
        pthread_cond_signal(&request->cond);
    }
    pthread_mutex_unlock(&request->lock);
}

void mpquic_request_send_502(proxy_request_t *request, const char *message)
{
    char body[512];
    char header[1024];
    int body_length;
    int header_length;

    if (request->finished) {
        return;
    }

    body_length = snprintf(body, sizeof(body), "%s\n", message);
    if (body_length < 0) {
        body_length = 0;
    }

    header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 502 Bad Gateway\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_length);

    if (header_length > 0 &&
        (size_t)header_length < sizeof(header)) {
        (void)socket_send_all(
            request->client_fd, header, (size_t)header_length);
        if (body_length > 0) {
            (void)socket_send_all(
                request->client_fd, body, (size_t)body_length);
        }
    }

    request->headers_sent = 1;
    mpquic_request_finish(request, 1);
}

static void add_active_request(proxy_request_t *request)
{
    mpquic_client_state_t *client = &mpquic_client_state;

    pthread_mutex_lock(&client->queue_lock);
    request->next = client->active_head;
    client->active_head = request;
    pthread_mutex_unlock(&client->queue_lock);
}

static proxy_request_t *pop_pending_request(void)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    proxy_request_t *request;

    pthread_mutex_lock(&client->queue_lock);

    request = client->pending_head;
    if (request != NULL) {
        client->pending_head = request->next;
        if (client->pending_head == NULL) {
            client->pending_tail = NULL;
        }
        request->next = NULL;
    }

    pthread_mutex_unlock(&client->queue_lock);
    return request;
}

static int open_h3_request(proxy_request_t *request)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    uint8_t buffer[H3_REQUEST_BUFFER];
    size_t request_length = 0;
    int result;

    request->stream_id = picoquic_get_next_local_stream_id(client->cnx, 0);

    result = h3zero_client_create_stream_request_ex(
        buffer,
        sizeof(buffer),
        (const uint8_t *)request->path,
        strlen(request->path),
        (request->range != NULL && request->range[0] != '\0')
            ? request->range
            : NULL,
        (request->range != NULL && request->range[0] != '\0')
            ? strlen(request->range)
            : 0,
        0,
        client->server_name,
        &request_length);

    if (result != 0) {
        return result;
    }

    memset(&request->stream_state, 0, sizeof(request->stream_state));
    add_active_request(request);

    result = picoquic_add_to_stream_with_ctx(
        client->cnx,
        request->stream_id,
        buffer,
        request_length,
        1,
        request);

    if (result != 0) {
        remove_active_request(request);
        picoquic_unlink_app_stream_ctx(client->cnx, request->stream_id);
        return result;
    }

    fprintf(stdout,
            "H3 stream %" PRIu64 " -> GET %s%s%s\n",
            request->stream_id,
            request->path,
            (request->range != NULL && request->range[0] != '\0')
                ? " Range: "
                : "",
            (request->range != NULL && request->range[0] != '\0')
                ? request->range
                : "");
    fflush(stdout);

    return 0;
}

void mpquic_requests_process_pending(void)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    proxy_request_t *request;

    if (!client->connected || !client->h3_initialized ||
        client->shutting_down || client->cnx == NULL) {
        return;
    }

    while ((request = pop_pending_request()) != NULL) {
        int result = open_h3_request(request);

        if (result != 0) {
            char message[128];
            snprintf(message, sizeof(message),
                     "cannot open HTTP/3 stream, picoquic error %d",
                     result);
            mpquic_request_send_502(request, message);
        }
    }
}

void mpquic_requests_fail_all(const char *message)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    proxy_request_t *pending;
    proxy_request_t *active;

    pthread_mutex_lock(&client->queue_lock);

    pending = client->pending_head;
    active = client->active_head;
    client->pending_head = NULL;
    client->pending_tail = NULL;
    client->active_head = NULL;

    pthread_mutex_unlock(&client->queue_lock);

    while (pending != NULL) {
        proxy_request_t *next = pending->next;
        pending->next = NULL;
        mpquic_request_send_502(pending, message);
        pending = next;
    }

    while (active != NULL) {
        proxy_request_t *next = active->next;
        active->next = NULL;

        /* The network thread is stopped, so callbacks no longer use it. */
        active->stream_id = UINT64_MAX;
        mpquic_request_send_502(active, message);
        active = next;
    }
}

int mpquic_client_fetch(int client_fd, const char *path, const char *range)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    proxy_request_t *request;
    int failed;

    if (client->network_thread == NULL || client->quic == NULL ||
        client->cnx == NULL || client->shutting_down) {
        return -1;
    }

    request = (proxy_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        return -1;
    }

    request->client_fd = client_fd;
    request->stream_id = UINT64_MAX;
    request->path = strdup(path);
    request->range =
        (range != NULL && *range != '\0') ? strdup(range) : NULL;

    if (request->path == NULL ||
        ((range != NULL && *range != '\0') && request->range == NULL)) {
        free(request->path);
        free(request->range);
        free(request);
        return -1;
    }

    pthread_mutex_init(&request->lock, NULL);
    pthread_cond_init(&request->cond, NULL);

    pthread_mutex_lock(&client->queue_lock);
    if (client->pending_tail == NULL) {
        client->pending_head = request;
        client->pending_tail = request;
    } else {
        client->pending_tail->next = request;
        client->pending_tail = request;
    }
    pthread_mutex_unlock(&client->queue_lock);

    if (picoquic_wake_up_network_thread(client->network_thread) != 0) {
        fprintf(stderr,
                "warning: picoquic network-thread wakeup failed; "
                "request remains queued\n");
    }

    pthread_mutex_lock(&request->lock);
    while (!request->finished) {
        pthread_cond_wait(&request->cond, &request->lock);
    }
    failed = request->failed;
    pthread_mutex_unlock(&request->lock);

    pthread_cond_destroy(&request->cond);
    pthread_mutex_destroy(&request->lock);
    free(request->path);
    free(request->range);
    free(request);

    return failed ? -1 : 0;
}
