#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "uv.h"

#include "crypto.h"
#include "logger.h"
#include "packet.h"
#include "util.h"
#include "peer.h"
#include "tun.h"
#include "http.h"



struct client_context {
    union {
        uv_tcp_t tcp;
        uv_handle_t handle;
        uv_stream_t stream;
    } handle;
    struct sockaddr addr;
    struct packet packet;
    struct peer *peer;
};


static struct client_context *
new_client(int mtu) {
    struct client_context *client = malloc(sizeof(*client));
    memset(client, 0, sizeof(*client));
    client->packet.buf = malloc(PRIMITIVE_BYTES + mtu);
    client->packet.max = PRIMITIVE_BYTES + mtu;
    packet_reset(&client->packet);
    return client;
}

static void
free_client(struct client_context *client) {
    free(client->packet.buf);
    free(client);
}

static void
client_close_cb(uv_handle_t *handle) {
    struct client_context *client = container_of(handle, struct client_context,
                                                 handle);
    free_client(client);
}

static void
close_client(struct client_context *client) {
    if (client->peer) {
        client->peer->data = NULL;
        client->peer = NULL;
    }
    uv_close(&client->handle.handle, client_close_cb);
}

static void
handle_invalid_packet(struct client_context *client) {
    int port = 0;
    char remote[INET_ADDRSTRLEN + 1];
    port = ip_name(&client->addr, remote, sizeof(remote));
    logger_log(LOG_ERR, "Invalid tcp packet from %s:%d", remote, port);
    packet_reset(&client->packet);
    close_client(client);
}

static void
alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    struct client_context *client = container_of(handle, struct client_context,
                                                 handle);
    packet_alloc(&client->packet, buf);
}

static void
recv_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    struct tundev_context *ctx;
    struct client_context *client;

    ctx = stream->data;
    client = container_of(stream, struct client_context, handle.stream);
    struct packet *packet = &client->packet;

    if (nread > 0) {

        if ((ctx->connect == AUTHING) && (0 == memcmp(packet->buf, "GET ", 4))) {
            http_auth(stream,  packet->buf);
            packet_reset(packet);
            ctx->connect = CONNECTED;
            return;
        }

        int rc = packet_filter(packet, buf->base, nread);
        if (rc == PACKET_UNCOMPLETE) {
            return;
        } else if (rc == PACKET_INVALID) {
            logger_log(LOG_ERR, "Filter Invalid: %d", nread);
            goto error;
        }

        int clen = packet->size;
        int mlen = packet->size - PRIMITIVE_BYTES;
        uint8_t *c = packet->buf, *m = packet->buf;

        assert(mlen > 0 && mlen <= ctx->tun->mtu);

        int err = crypto_decrypt(m, c, clen);
        if (err) {
            logger_log(LOG_ERR, "Fail Decrypt: %d", clen);
            goto error;
        }

        struct iphdr *iphdr = (struct iphdr *) m;

        in_addr_t client_network = iphdr->saddr & htonl(ctx->tun->netmask);
        if (client_network != ctx->tun->network) {
            char *a = inet_ntoa(*(struct in_addr *) &iphdr->saddr);
            logger_log(LOG_ERR, "Invalid client: %s", a);
            close_client(client);
            return;
        }

        if (client->peer == NULL) {
            uv_rwlock_rdlock(&rwlock);
            struct peer *peer = lookup_peer(iphdr->saddr, peers);
            uv_rwlock_rdunlock(&rwlock);
            if (peer == NULL) {
                char saddr[24] = {0}, daddr[24] = {0};
                parse_addr(iphdr, saddr, daddr);
                logger_log(LOG_WARNING, "[TCP] Cache miss: %s -> %s",
                           saddr, daddr);

                uv_rwlock_wrlock(&rwlock);
                peer = save_peer(iphdr->saddr, &client->addr, peers);
                uv_rwlock_wrunlock(&rwlock);

            } else {
                if (peer->data) {
                    struct client_context *old = peer->data;
                    close_client(old);
                }
            }

            peer->protocol= xTUN_TCP;
            peer->data = client;
            client->peer = peer;
        }

        network_to_tun(ctx->tunfd, m, mlen);

        packet_reset(packet);

    } else if (nread < 0) {
        if (nread != UV_EOF) {
            logger_log(LOG_ERR, "Receive from client failed: %s",
                       uv_strerror(nread));
        }
        close_client(client);
    }

    return;

error:
    if (verbose) {
        dump_hex(buf->base, nread, "Invalid tcp Packet");
    }
    handle_invalid_packet(client);
}

static void
accept_cb(uv_stream_t *stream, int status) {
    struct tundev_context *ctx = stream->data;
    struct client_context *client = new_client(ctx->tun->mtu);

    uv_tcp_init(stream->loop, &client->handle.tcp);
    int rc = uv_accept(stream, &client->handle.stream);
    if (rc == 0) {
        int len = sizeof(struct sockaddr);
        uv_tcp_getpeername(&client->handle.tcp, &client->addr, &len);
        client->handle.stream.data = ctx;
        uv_tcp_nodelay(&client->handle.tcp, 1);
        uv_tcp_keepalive(&client->handle.tcp, 1, 60);

        client->packet.size = 512;
        ctx->connect = AUTHING;

        uv_read_start(&client->handle.stream, alloc_cb, recv_cb);

    } else {
        logger_log(LOG_ERR, "accept error: %s", uv_strerror(rc));
        close_client(client);
    }
}

int
tcp_server_start(struct tundev_context *ctx, uv_loop_t *loop) {
    int rc;

    uv_tcp_init(loop, &ctx->inet_tcp.tcp);

    ctx->inet_tcp_fd = create_socket(SOCK_STREAM, 1);
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
        tun_to_tcp(buf, len, &client->handle.stream);
    } else {
        free(buf);
    }
}
