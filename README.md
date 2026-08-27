# Local HTTP -> persistent MPQUIC proxy

Targeted setup:

- picoquic commit: `4685671759703c1ba20d7251c766e055b779341c`
- server SNI: `video.duxng.io.vn`
- server IPv4: `47.129.174.173`
- LAN: `192.168.0.111`, ifindex `2`
- Android USB tethering / 4G: `192.168.69.61`, ifindex `33`
- localhost proxy: `http://127.0.0.1:8080`
- qlog: `/home/a2ilab/mpquic-test/proxy-qlog`

## Architecture

```text
Chrome / dash.js
      |
      | HTTP/1.1 localhost
      v
local HTTP proxy (`src/local_proxy.c`)
      |
      | request queue + picoquic network-thread wakeup
      v
one persistent QUIC/MPQUIC connection
      |\
      | \__ path via LAN
      |
       \___ path via USB tethering / 4G
      |
      v
video.duxng.io.vn:443 (H3)
```

Every local browser GET is mapped to a **new client-initiated H3 bidirectional
stream** using `picoquic_get_next_local_stream_id()`, but all streams reuse the
same QUIC/MPQUIC connection.

The H3 response body is forwarded to the localhost TCP socket **as data arrives**.
It is not buffered until the complete segment has downloaded.

## Install the files

Put these files in:

```bash
~/mpquic-proxy/
```

The directory should contain:

```text
include/mpquic_client.h
src/main.c
src/local_proxy.c
src/local_proxy.h
src/socket_io.c
src/socket_io.h
src/mpquic_client.c
src/mpquic_internal.h
src/mpquic_requests.c
src/mpquic_paths.c
Makefile
```

The source tree is split by responsibility:

```text
include/mpquic_client.h   Public MPQUIC client API
src/main.c                Process lifecycle and signal handling
src/local_proxy.c         Local HTTP/1.1 listener and request handling
src/socket_io.c           Shared reliable socket writes
src/mpquic_client.c       picoquic lifecycle and callbacks
src/mpquic_requests.c     H3 request queue and response streaming
src/mpquic_paths.c        Multipath configuration and probing
src/mpquic_internal.h     Private state shared by MPQUIC modules
```

## Build

```bash
cd ~/mpquic-proxy
make clean
make
```

## Run

```bash
~/mpquic-proxy/proxy
```

Expected early output is similar to:

```text
picoquic version: 1.1.52.0
Persistent MPQUIC client started
  server : video.duxng.io.vn (47.129.174.173):443
  LAN    : 192.168.0.111 ifindex=2
  4G     : 192.168.69.61 ifindex=33
  qlog   : /home/a2ilab/mpquic-test/proxy-qlog
MPQUIC proxy listening on http://127.0.0.1:8080
```

After the QUIC handshake:

```text
HTTP/3 control stream initialized
QUIC ready: multipath=enabled, paths=...
MPQUIC alternative path ... added ...
```

## First test

Use the DASH manifest already verified with picoquicdemo:

```bash
curl -v \
  http://127.0.0.1:8080/video/BigBuckBunny/4sec/BigBuckBunny_4s_simple_2014_05_09.mpd \
  -o /tmp/test.mpd
```

Then:

```bash
head /tmp/test.mpd
```

## Linux regression checklist

Run these checks on the target Linux host after copying the refactored tree:

```bash
make clean && make
./proxy
```

From another terminal, verify the local HTTP behavior and the upstream stream:

```bash
curl -i -X OPTIONS http://127.0.0.1:8080/
curl -i -X POST http://127.0.0.1:8080/
curl -fS \
  http://127.0.0.1:8080/video/BigBuckBunny/4sec/BigBuckBunny_4s_simple_2014_05_09.mpd \
  -o /tmp/test.mpd
curl -fS -H 'Range: bytes=0-1023' \
  http://127.0.0.1:8080/video/BigBuckBunny/4sec/BigBuckBunny_4s_simple_2014_05_09.mpd \
  -o /tmp/test-range.bin
```

Expected results are `204` for `OPTIONS`, `405` for `POST`, a valid MPD for
the GET, and a successful ranged response. Stop the proxy with `Ctrl-C` and
confirm that it prints `MPQUIC client stopped`.

## Browser

Open the site through localhost, not through the remote HTTPS origin:

```text
http://127.0.0.1:8080/
```

Because relative HTML/JS/MPD/segment URLs then resolve to `127.0.0.1:8080`,
all those GETs pass through the MPQUIC proxy.

For a direct DASH test, point dash.js at:

```text
http://127.0.0.1:8080/video/BigBuckBunny/4sec/BigBuckBunny_4s_simple_2014_05_09.mpd
```

## Runtime overrides

The compiled defaults match the current PC, but they can be changed without
editing C code:

```bash
MPQUIC_SERVER_NAME=video.duxng.io.vn \
MPQUIC_SERVER_IP=47.129.174.173 \
MPQUIC_LAN_IP=192.168.0.111 \
MPQUIC_LAN_IF=2 \
MPQUIC_4G_IP=192.168.69.61 \
MPQUIC_4G_IF=33 \
MPQUIC_QLOG_DIR=$HOME/mpquic-test/proxy-qlog \
~/mpquic-proxy/proxy
```

This is useful because the USB-tethering address or interface index can change
after reconnecting the phone.

## Important experimental note

The localhost side is HTTP/1.1 with `Connection: close`; this is deliberate for
the first working proxy because it keeps browser-facing state independent from
the transport experiment. The remote side remains one persistent H3/MPQUIC
connection with many H3 streams.

For publication-grade comparison, use the same localhost-proxy architecture for
the H3 baseline and MPQUIC case so the local HTTP hop is held constant.
