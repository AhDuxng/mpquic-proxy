#include "mpquic_internal.h"

#include <stdio.h>

#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picosocks.h"

static int path_error_is_transient(int error)
{
    return error == PICOQUIC_ERROR_PATH_ID_BLOCKED ||
           error == PICOQUIC_ERROR_PATH_CID_BLOCKED ||
           error == PICOQUIC_ERROR_PATH_NOT_READY;
}

int mpquic_path_configure(
    int index,
    const char *local_ip,
    int interface_index,
    const char *server_ip)
{
    mpquic_client_state_t *client = &mpquic_client_state;

    if (index < 0 || index >= MPQUIC_MAX_ALT_PATHS) {
        return -1;
    }

    if (picoquic_store_text_addr(
            &client->alt_client[index], local_ip, 0) != 0) {
        fprintf(stderr, "Cannot parse local path IP: %s\n", local_ip);
        return -1;
    }

    if (picoquic_store_text_addr(
            &client->alt_server[index],
            server_ip,
            MPQUIC_SERVER_PORT) != 0) {
        fprintf(stderr, "Cannot parse server IP: %s\n", server_ip);
        return -1;
    }

    client->alt_if[index] = interface_index;
    client->alt_state[index] = 0;
    return 0;
}

int mpquic_paths_create(void)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    int need_retry = 0;
    int index;

    if (client->cnx == NULL ||
        picoquic_get_cnx_state(client->cnx) != picoquic_state_ready ||
        !client->cnx->is_multipath_enabled) {
        return 0;
    }

    for (index = 0; index < client->nb_alt_paths; index++) {
        int result;

        if (client->alt_state[index] != 0) {
            continue;
        }

        result = picoquic_probe_new_path_ex(
            client->cnx,
            (struct sockaddr *)&client->alt_server[index],
            (struct sockaddr *)&client->alt_client[index],
            client->alt_if[index],
            picoquic_get_quic_time(client->quic),
            0);

        if (result == 0) {
            client->alt_state[index] = 1;
            fprintf(stdout,
                    "MPQUIC alternative path %d added (ifindex=%d), "
                    "nb_paths=%d\n",
                    index + 1,
                    client->alt_if[index],
                    client->cnx->nb_paths);
            fflush(stdout);
        } else if (path_error_is_transient(result)) {
            need_retry = 1;
        } else {
            client->alt_state[index] = -1;
            fprintf(stderr,
                    "MPQUIC path %d probe failed: %d\n",
                    index + 1,
                    result);
        }
    }

    client->multipath_probe_done = !need_retry;
    return 0;
}

void mpquic_paths_ensure(void)
{
    mpquic_client_state_t *client = &mpquic_client_state;
    int is_already_allowed = 0;

    if (client->cnx == NULL ||
        picoquic_get_cnx_state(client->cnx) != picoquic_state_ready ||
        !client->cnx->is_multipath_enabled) {
        return;
    }

    if (!client->multipath_initiated) {
        client->multipath_initiated = 1;

        if (picoquic_subscribe_new_path_allowed(
                client->cnx, &is_already_allowed) == 0 &&
            is_already_allowed) {
            (void)mpquic_paths_create();
        }
    }

    if (!client->multipath_probe_done &&
        client->cnx->is_notified_that_path_is_allowed) {
        (void)mpquic_paths_create();
    }
}
