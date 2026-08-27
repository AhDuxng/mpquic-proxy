#define _GNU_SOURCE

#include "mpquic_client.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "h3zero_common.h"
#include "mpquic_internal.h"
#include "picoquic_config.h"
#include "picoquic_internal.h"
#include "picoquic_qlog.h"
#include "picoquic_utils.h"

#define DEFAULT_SERVER_NAME "video.duxng.io.vn"
#define DEFAULT_SERVER_IP "47.129.174.173"
#define DEFAULT_LAN_IP "192.168.0.111"
#define DEFAULT_LAN_IF 2
#define DEFAULT_4G_IP "192.168.69.61"
#define DEFAULT_4G_IF 33
#define DEFAULT_QLOG_DIR "/home/a2ilab/mpquic-test/proxy-qlog"

mpquic_client_state_t mpquic_client_state;

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != NULL && *value != '\0' ? value : fallback;
}

static int env_int_or_default(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    long number;

    if (value == NULL || *value == '\0') {
        return fallback;
    }

    errno = 0;
    number = strtol(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' ||
        number < 0 || number > 0x7fffffffL) {
        return fallback;
    }

    return (int)number;
}

static int mkdir_p(const char *path)
{
    char temporary[1024];
    size_t length;
    char *cursor;

    if (path == NULL || *path == '\0') {
        return -1;
    }

    length = strnlen(path, sizeof(temporary));
    if (length == 0 || length >= sizeof(temporary)) {
        return -1;
    }

    memcpy(temporary, path, length + 1);

    if (temporary[length - 1] == '/') {
        temporary[length - 1] = '\0';
    }

    for (cursor = temporary + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(temporary, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *cursor = '/';
        }
    }

    if (mkdir(temporary, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static int mpquic_connection_callback(
    picoquic_cnx_t *connection,
    uint64_t stream_id,
    uint8_t *bytes,
    size_t length,
    picoquic_call_back_event_t event,
    void *callback_context,
    void *stream_context)
{
    mpquic_client_state_t *client =
        (mpquic_client_state_t *)callback_context;
    proxy_request_t *request = (proxy_request_t *)stream_context;

    switch (event) {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (request != NULL && !request->finished) {
            if (length > 0) {
                uint8_t *cursor = bytes;
                uint8_t *end = bytes + length;

                while (cursor < end && !request->finished) {
                    size_t available_data = 0;
                    uint64_t error_found = 0;
                    uint8_t *next = h3zero_parse_data_stream(
                        cursor,
                        end,
                        &request->stream_state,
                        &available_data,
                        &error_found);

                    if (next == NULL) {
                        fprintf(stderr,
                                "H3 parse error on stream %" PRIu64
                                ": 0x%" PRIx64 "\n",
                                request->stream_id,
                                error_found);
                        (void)picoquic_reset_stream(
                            connection,
                            request->stream_id,
                            H3_MESSAGE_ERROR);
                        mpquic_request_send_502(
                            request, "invalid HTTP/3 response");
                        break;
                    }

                    cursor = next;

                    if (request->stream_state.header_found &&
                        !request->headers_sent &&
                        mpquic_request_send_headers(request) != 0) {
                        (void)picoquic_reset_stream(
                            connection,
                            request->stream_id,
                            H3ZERO_REQUEST_CANCELLED);
                        mpquic_request_finish(request, 1);
                        break;
                    }

                    if (available_data > 0) {
                        if (!request->flow_opened) {
                            uint64_t frame_length =
                                request->stream_state.current_frame_length;

                            if (frame_length < 0x100000) {
                                request->flow_opened = 1;
                            } else if (picoquic_get_cnx_state(connection) ==
                                       picoquic_state_ready) {
                                request->flow_opened = 1;
                                (void)picoquic_open_flow_control(
                                    connection,
                                    request->stream_id,
                                    frame_length);
                            }
                        }

                        if (mpquic_request_send_chunk(
                                request, cursor, available_data) != 0) {
                            (void)picoquic_reset_stream(
                                connection,
                                request->stream_id,
                                H3ZERO_REQUEST_CANCELLED);
                            mpquic_request_finish(request, 1);
                            break;
                        }

                        cursor += available_data;
                    }
                }
            }

            if (event == picoquic_callback_stream_fin &&
                !request->finished) {
                mpquic_request_finish(request, 0);
            }
        }
        break;

    case picoquic_callback_stream_reset:
    case picoquic_callback_stop_sending:
    case picoquic_callback_stream_gap:
        if (request != NULL && !request->finished) {
            mpquic_request_send_502(
                request, "upstream HTTP/3 stream was reset");
        }
        break;

    case picoquic_callback_almost_ready:
    case picoquic_callback_ready:
        client->connected = 1;

        if (!client->h3_initialized) {
            int result = h3zero_protocol_init(connection);

            if (result != 0) {
                fprintf(stderr, "h3zero_protocol_init failed: %d\n", result);
            } else {
                client->h3_initialized = 1;
                fprintf(stdout, "HTTP/3 control stream initialized\n");
                fflush(stdout);
            }
        }

        fprintf(stdout,
                "QUIC ready: multipath=%s, paths=%d\n",
                connection->is_multipath_enabled ? "enabled" : "disabled",
                connection->nb_paths);
        fflush(stdout);

        mpquic_paths_ensure();
        mpquic_requests_process_pending();
        break;

    case picoquic_callback_next_path_allowed:
        (void)mpquic_paths_create();
        break;

    case picoquic_callback_path_available:
    case picoquic_callback_path_suspended:
    case picoquic_callback_path_deleted:
    case picoquic_callback_path_quality_changed:
        fprintf(stdout,
                "MPQUIC path event=%d unique_path_id=%" PRIu64
                " nb_paths=%d\n",
                (int)event,
                stream_id,
                connection->nb_paths);
        fflush(stdout);
        break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
        client->connected = 0;
        fprintf(stderr,
                "QUIC/MPQUIC connection closed:\\n"
                "  callback_event           = %d\\n"
                "  cnx_state                = %d\\n"
                "  local_error              = 0x%" PRIx64 "\\n"
                "  application_error        = 0x%" PRIx64 "\\n"
                "  remote_error             = 0x%" PRIx64 "\\n"
                "  remote_application_error = 0x%" PRIx64 "\\n"
                "  offending_frame_type     = 0x%" PRIx64 "\\n"
                "  local_error_reason       = %s\\n"
                "  remote_error_reason      = %s\\n",
                (int)event,
                (int)connection->cnx_state,
                connection->local_error,
                connection->application_error,
                connection->remote_error,
                connection->remote_application_error,
                connection->offending_frame_type,
                connection->local_error_reason != NULL
                    ? connection->local_error_reason
                    : "(none)",
                connection->remote_error_reason != NULL
                    ? connection->remote_error_reason
                    : "(none)");
        fflush(stderr);
        break;

    case picoquic_callback_prepare_to_send:
    default:
        break;
    }

    return 0;
}

static int mpquic_loop_callback(
    picoquic_quic_t *quic,
    picoquic_packet_loop_cb_enum mode,
    void *callback_context,
    void *callback_argument)
{
    mpquic_client_state_t *client =
        (mpquic_client_state_t *)callback_context;

    (void)quic;

    switch (mode) {
    case picoquic_packet_loop_ready: {
        picoquic_packet_loop_options_t *options =
            (picoquic_packet_loop_options_t *)callback_argument;

        options->provide_alt_port = 1;
        fprintf(stdout, "picoquic network thread ready\n");
        fflush(stdout);
        break;
    }

    case picoquic_packet_loop_wake_up:
        mpquic_paths_ensure();
        mpquic_requests_process_pending();
        break;

    case picoquic_packet_loop_after_receive:
    case picoquic_packet_loop_after_send:
        mpquic_paths_ensure();
        if (client->connected) {
            mpquic_requests_process_pending();
        }
        break;

    case picoquic_packet_loop_alt_port:
    case picoquic_packet_loop_port_update:
    case picoquic_packet_loop_time_check:
    case picoquic_packet_loop_system_call_duration:
    default:
        break;
    }

    return 0;
}

int mpquic_client_init(void)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    picoquic_quic_config_t config;
    uint64_t now;
    int result = 0;
    int is_name = 0;
    const char *server_name =
        env_or_default("MPQUIC_SERVER_NAME", DEFAULT_SERVER_NAME);
    const char *server_ip =
        env_or_default("MPQUIC_SERVER_IP", DEFAULT_SERVER_IP);
    const char *lan_ip =
        env_or_default("MPQUIC_LAN_IP", DEFAULT_LAN_IP);
    const char *g4_ip =
        env_or_default("MPQUIC_4G_IP", DEFAULT_4G_IP);
    const char *qlog_dir =
        env_or_default("MPQUIC_QLOG_DIR", DEFAULT_QLOG_DIR);
    int lan_if = env_int_or_default("MPQUIC_LAN_IF", DEFAULT_LAN_IF);
    int g4_if = env_int_or_default("MPQUIC_4G_IF", DEFAULT_4G_IF);

    memset(client, 0, sizeof(*client));
    pthread_mutex_init(&client->queue_lock, NULL);

    snprintf(client->server_name, sizeof(client->server_name),
             "%s", server_name);
    snprintf(client->server_ip, sizeof(client->server_ip), "%s", server_ip);
    snprintf(client->qlog_dir, sizeof(client->qlog_dir), "%s", qlog_dir);

    if (mkdir_p(client->qlog_dir) != 0) {
        fprintf(stderr, "Cannot create qlog directory: %s\n",
                client->qlog_dir);
        return -1;
    }

    if (picoquic_get_server_address(
            client->server_ip,
            MPQUIC_SERVER_PORT,
            &client->server_address,
            &is_name) != 0) {
        fprintf(stderr, "Cannot resolve MPQUIC server %s:%d\n",
                client->server_ip, MPQUIC_SERVER_PORT);
        return -1;
    }

    if (mpquic_path_configure(0, lan_ip, lan_if, client->server_ip) != 0 ||
        mpquic_path_configure(1, g4_ip, g4_if, client->server_ip) != 0) {
        return -1;
    }
    client->nb_alt_paths = 2;

    picoquic_config_init(&config);

    if (picoquic_config_set_option(
            &config, picoquic_option_SNI, client->server_name) != 0 ||
        picoquic_config_set_option(
            &config, picoquic_option_ALPN, "h3") != 0 ||
        picoquic_config_set_option(
            &config, picoquic_option_MULTIPATH, "1") != 0) {
        fprintf(stderr, "Cannot configure SNI/ALPN/multipath\n");
        picoquic_config_clear(&config);
        return -1;
    }

    now = picoquic_current_time();
    client->quic = picoquic_create_and_configure(
        &config, NULL, NULL, now, NULL);

    if (client->quic == NULL) {
        fprintf(stderr, "picoquic_create_and_configure failed\n");
        picoquic_config_clear(&config);
        return -1;
    }

    picoquic_set_qlog(client->quic, client->qlog_dir);
    picoquic_set_key_log_file_from_env(client->quic);

    client->cnx = picoquic_create_cnx(
        client->quic,
        picoquic_null_connection_id,
        picoquic_null_connection_id,
        (struct sockaddr *)&client->server_address,
        now,
        config.proposed_version,
        client->server_name,
        "h3",
        1);

    if (client->cnx == NULL) {
        fprintf(stderr, "picoquic_create_cnx failed\n");
        picoquic_config_clear(&config);
        picoquic_free(client->quic);
        client->quic = NULL;
        return -1;
    }

    picoquic_set_callback(
        client->cnx, mpquic_connection_callback, client);
    picoquic_cnx_set_pmtud_policy(client->cnx, picoquic_pmtud_delayed);
    picoquic_set_default_pmtud_policy(client->quic, picoquic_pmtud_delayed);
    picoquic_enable_keep_alive(client->cnx, 0);

    result = picoquic_start_client_cnx(client->cnx);
    picoquic_config_clear(&config);

    if (result != 0) {
        fprintf(stderr, "picoquic_start_client_cnx failed: %d\n", result);
        picoquic_free(client->quic);
        client->quic = NULL;
        client->cnx = NULL;
        return -1;
    }

    memset(&client->loop_param, 0, sizeof(client->loop_param));
    client->loop_param.local_af = AF_INET;
    client->loop_param.local_port = 0;
    client->loop_param.extra_socket_required = 1;
    client->loop_param.prefer_extra_socket = 0;
    client->loop_param.do_not_use_gso = 0;

    client->network_thread = picoquic_start_network_thread(
        client->quic,
        &client->loop_param,
        mpquic_loop_callback,
        client,
        &result);

    if (client->network_thread == NULL || result != 0) {
        fprintf(stderr, "Cannot start picoquic network thread: %d\n", result);
        picoquic_free(client->quic);
        client->quic = NULL;
        client->cnx = NULL;
        return -1;
    }

    printf("picoquic version: %s\n", PICOQUIC_VERSION);
    printf("Persistent MPQUIC client started\n");
    printf("  keepalive: enabled (automatic interval = idle_timeout/2)\n");
    printf("  server : %s (%s):%d\n",
           client->server_name, client->server_ip, MPQUIC_SERVER_PORT);
    printf("  LAN    : %s ifindex=%d\n", lan_ip, lan_if);
    printf("  4G     : %s ifindex=%d\n", g4_ip, g4_if);
    printf("  qlog   : %s\n", client->qlog_dir);
    fflush(stdout);

    return 0;
}

void mpquic_client_shutdown(void)
{
    mpquic_client_state_t *client = &mpquic_client_state;

    if (client->shutting_down) {
        return;
    }

    client->shutting_down = 1;

    if (client->network_thread != NULL) {
        picoquic_delete_network_thread(client->network_thread);
        client->network_thread = NULL;
    }

    mpquic_requests_fail_all("MPQUIC proxy is shutting down");

    if (client->quic != NULL) {
        picoquic_free(client->quic);
        client->quic = NULL;
        client->cnx = NULL;
    }

    pthread_mutex_destroy(&client->queue_lock);

    fprintf(stdout, "MPQUIC client stopped\n");
    fflush(stdout);
}
