#ifndef MPQUIC_CLIENT_H
#define MPQUIC_CLIENT_H

/*
 * Initialize one persistent H3/MPQUIC connection and start picoquic's
 * background network thread.
 */
int mpquic_client_init(void);

/*
 * Proxy one local HTTP GET over a new HTTP/3 stream on the persistent
 * MPQUIC connection.
 *
 * client_fd: connected localhost TCP socket.
 * path:      origin-form path, e.g. "/video/foo/manifest.mpd".
 * range:     value of the HTTP Range header, e.g. "bytes=0-1023",
 *            or NULL / empty string.
 *
 * The function blocks until the H3 response stream finishes.
 */
int mpquic_client_fetch(int client_fd, const char *path, const char *range);

/* Stop the picoquic network thread and release the QUIC context. */
void mpquic_client_shutdown(void);

#endif
