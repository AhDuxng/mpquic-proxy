#ifndef MPQUIC_INTERNAL_H
#define MPQUIC_INTERNAL_H

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>

#include "h3zero.h"
#include "picoquic.h"
#include "picoquic_packet_loop.h"

#define MPQUIC_SERVER_PORT 443
#define MPQUIC_MAX_ALT_PATHS 2

typedef struct proxy_request_s proxy_request_t;

struct proxy_request_s {
    int client_fd;
    char *path;
    char *range;

    uint64_t stream_id;
    h3zero_data_stream_state_t stream_state;

    int headers_sent;
    int flow_opened;
    int finished;
    int failed;

    pthread_mutex_t lock;
    pthread_cond_t cond;

    proxy_request_t *next;
};

typedef struct {
    picoquic_quic_t *quic;
    picoquic_cnx_t *cnx;
    picoquic_network_thread_ctx_t *network_thread;
    picoquic_packet_loop_param_t loop_param;

    struct sockaddr_storage server_address;

    struct sockaddr_storage alt_client[MPQUIC_MAX_ALT_PATHS];
    struct sockaddr_storage alt_server[MPQUIC_MAX_ALT_PATHS];
    int alt_if[MPQUIC_MAX_ALT_PATHS];
    int alt_state[MPQUIC_MAX_ALT_PATHS];
    int nb_alt_paths;

    int connected;
    int h3_initialized;
    int multipath_initiated;
    int multipath_probe_done;
    int shutting_down;

    pthread_mutex_t queue_lock;
    proxy_request_t *pending_head;
    proxy_request_t *pending_tail;
    proxy_request_t *active_head;

    char server_name[256];
    char server_ip[128];
    char qlog_dir[1024];
} mpquic_client_state_t;

extern mpquic_client_state_t mpquic_client_state;

int mpquic_request_send_headers(proxy_request_t *request);
int mpquic_request_send_chunk(
    proxy_request_t *request,
    const uint8_t *data,
    size_t length);
void mpquic_request_finish(proxy_request_t *request, int failed);
void mpquic_request_send_502(proxy_request_t *request, const char *message);
void mpquic_requests_process_pending(void);
void mpquic_requests_fail_all(const char *message);

int mpquic_path_configure(
    int index,
    const char *local_ip,
    int interface_index,
    const char *server_ip);
int mpquic_paths_create(void);
void mpquic_paths_ensure(void);

#endif
