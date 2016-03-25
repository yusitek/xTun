#include <string.h>

#include "uv.h"

#include "crypto.h"
#include "logger.h"
#include "packet.h"
#include "util.h"
#include "peer.h"
#include "tun.h"


struct client_context {
    union {
        uv_tcp_t tcp;
        uv_handle_t handle;
        uv_stream_t stream;
    } handle;
    struct packet packet;
    struct peer *peer;
};


static struct client_context *
new_client(int packet_size) {
    struct client_context *client = malloc(sizeof(*client));
    memset(client, 0, sizeof(*client));
    client->packet.buf = malloc(packet_size);
    packet_reset(&client->packet);
    return client;
}

static void
free_client(struct client_context *client) {
    if (client->peer) {
        client->peer->tcp = 0;
        client->peer->data = NULL;
    }
    free(client->packet.buf);
    free(client);
}

static void
client_close_cb(uv_handle_t *handle) {
    struct client_context *client = (struct client_context *) handle->data;
    free_client(client);
}

static void
close_client(struct client_context *client) {
    client->handle.handle.data = client;
    uv_close(&client->handle.handle, client_close_cb);
}

static void
alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    struct client_context *client = container_of(handle, struct client_context,
                                                 handle);
    struct packet *packet = &client->packet;
    if (packet->size) {
        buf->base = (char *) packet->buf + packet->offset;
        buf->len = packet->size - packet->offset;
    } else {
        buf->base = (char *) packet->buf + (packet->read ? 1 : 0);
        buf->len = packet->read ? 1 : HEADER_BYTES;
    }
}

static void
send_cb(uv_write_t *req, int status) {
    struct client_context *client = req->data;
    if (status) {
        if (client->peer) {
            char *a = inet_ntoa(client->peer->tun_addr);
            logger_log(LOG_ERR, "Send to %s failed: %s", a,
              uv_strerror(status));
        } else {
            logger_log(LOG_ERR, "Send to client failed: %s",
              uv_strerror(status));
        }
    }
    uv_buf_t *buf = (uv_buf_t *) (req + 1);
    free(buf->base);
    free(req);
}

static void
recv_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    struct client_context *client = container_of(stream, struct client_context,
                                                 handle.stream);
    if (nread > 0) {
        struct packet *packet = &client->packet;
        int rc = packet_filter(packet, buf->base, nread);
        if (rc == PACKET_UNCOMPLETE) {
            return;
        } else if (rc == PACKET_INVALID) {
            goto error;
        }

        int clen = packet->size;
        int mlen = packet->size - PRIMITIVE_BYTES;
        uint8_t *c = packet->buf, *m = packet->buf;

        int err = crypto_decrypt(m, c, clen);
        if (err) {
            goto error;
        }

        struct iphdr *iphdr = (struct iphdr *) m;
        if (client->peer == NULL) {
            uv_rwlock_rdlock(&rwlock);
            struct peer *peer = lookup_peer(iphdr->saddr, peers);
            uv_rwlock_rdunlock(&rwlock);
            if (peer == NULL) {
                char saddr[24] = {0}, daddr[24] = {0};
                parse_addr(iphdr, saddr, daddr);
                logger_log(LOG_WARNING, "Cache miss: %s -> %s", saddr, daddr);

                struct sockaddr addr;
                int len = sizeof(addr);
                uv_tcp_getpeername(&client->handle.tcp, &addr, &len);

                uv_rwlock_wrlock(&rwlock);
                peer = save_peer(iphdr->saddr, &addr, peers);
                uv_rwlock_wrunlock(&rwlock);
            }

            peer->tcp = 1;
            peer->data = client;
            client->peer = peer;
        }

        struct tundev_context *ctx = stream->data;
        network_to_tun(ctx->tunfd, m, mlen);

        packet_reset(packet);

    } else if (nread < 0) {
        if (nread != UV_EOF) {
            if (client->peer) {
                char *a = inet_ntoa(client->peer->tun_addr);
                logger_log(LOG_ERR, "Receive from %s failed: %s", a,
                           uv_strerror(nread));
            } else {
                logger_log(LOG_ERR, "Receive from client failed: %s",
                           uv_strerror(nread));
            }
        }
        close_client(client);
    }

    return;

error:
    logger_log(LOG_ERR, "Invalid tcp packet");
    close_client(client);
}

static void
accept_cb(uv_stream_t *stream, int status) {
    struct tundev_context *ctx = stream->data;
    struct client_context *client = new_client(ctx->tun->mtu);

    uv_tcp_init(stream->loop, &client->handle.tcp);
    int rc = uv_accept(stream, &client->handle.stream);
    if (rc == 0) {
        client->handle.stream.data = ctx;
        uv_read_start(&client->handle.stream, alloc_cb, recv_cb);
    } else {
        logger_log(LOG_ERR, "accept error: %s", uv_strerror(rc));
        close_client(client);
    }
}

int
tcp_server_start(struct tundev_context *ctx, uv_loop_t *loop) {
    uv_tcp_init(loop, &ctx->inet_tcp.tcp);

    int rc;
    if ((rc = uv_tcp_open(&ctx->inet_tcp.tcp, ctx->inet_tcp_fd))) {
        logger_stderr("tcp open error: %s", uv_strerror(rc));
        exit(1);
    }

    uv_tcp_bind(&ctx->inet_tcp.tcp, &ctx->tun->addr, 0);
    if (rc) {
        logger_stderr("tcp bind error: %s", uv_strerror(rc));
        exit(1);
    }
    ctx->inet_tcp.tcp.data = ctx;
    rc = uv_listen(&ctx->inet_tcp.stream, 128, accept_cb);
    if (rc) {
        logger_stderr("tcp listen error: %s", uv_strerror(rc));
        exit(1);
    }
    return rc;
}

void
tun_to_tcp_client(struct peer *peer, uint8_t *buf, int len) {
    struct client_context *client = peer->data;
    if (client) {
        uv_write_t *req = malloc(sizeof(*req) + sizeof(uv_buf_t));

        uv_buf_t *outbuf = (uv_buf_t *) (req + 1);
        outbuf->base = (char *) buf;
        outbuf->len = len;

        uint8_t hdr[2];
        write_size(hdr, len);

        uv_buf_t bufs[2] = {
            uv_buf_init((char *) hdr, 2),
            *outbuf,
        };

        req->data = client;
        uv_write(req, &client->handle.stream, bufs, 2, send_cb);

    } else {
        free(buf);
    }
}
