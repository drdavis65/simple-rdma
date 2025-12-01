#include <arpa/inet.h>
#include <contrib/lwlog.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "oob_sock.h"

#define OOB_FATAL(FORMAT, ...)                                                                     \
    {                                                                                              \
        lwlog_crit(FORMAT, ##__VA_ARGS__);                                                         \
        exit(EXIT_FAILURE);                                                                        \
        ;                                                                                          \
    }

#define OOB_DEBUG(FORMAT, ...)                                                                     \
    { lwlog_info(FORMAT, ##__VA_ARGS__); }

#define OOB_LOG(FORMAT, ...)                                                                       \
    { lwlog_print(FORMAT, ##__VA_ARGS__); }

#define sock_msg_fn(func, modifier)                                                                \
    static int sock_##func(int fd, modifier void *buf, size_t len) {                               \
        modifier char *ptr = (modifier char *) (buf);                                              \
        size_t processed = 0;                                                                      \
        size_t to_process = len;                                                                   \
        int ret;                                                                                   \
        while (processed < len) {                                                                  \
            ret = func(fd, ptr, to_process, 0);                                                    \
            if (ret < 0)                                                                           \
                return ret;                                                                        \
            ptr += ret;                                                                            \
            processed += ret;                                                                      \
            to_process -= ret;                                                                     \
        }                                                                                          \
        return 0;                                                                                  \
    }

sock_msg_fn(send, const);
sock_msg_fn(recv, );

static void server_accept(struct oob_sock_ctx *ctx) {
    struct sockaddr_in client_addr;
    unsigned int client_size = sizeof(client_addr);
    ctx->data_fd = accept(ctx->server.listen_fd, (struct sockaddr *) &client_addr, &client_size);
    if (ctx->data_fd < 0) {
        OOB_FATAL("Server can't accept: %s", strerror(errno));
    }

    OOB_LOG("Client connected from IP: %s", inet_ntoa(client_addr.sin_addr));
}

static void server_setup(struct oob_sock_ctx *ctx) {
    ctx->server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->server.listen_fd < 0) {
        OOB_FATAL("Server error while creating socket");
    }
    OOB_DEBUG("Server socket created successfully");

    int enable = 1;
    if (setsockopt(ctx->server.listen_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable))) {
        OOB_FATAL("Server error while setting socket options");
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ctx->port);
    server_addr.sin_addr.s_addr = INADDR_ANY; /* listen on any interface */
    if (bind(ctx->server.listen_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        OOB_FATAL("Server couldn't bind to the port %d", ctx->port);
    }
    OOB_DEBUG("Server done with binding");

    if (listen(ctx->server.listen_fd, 1) < 0) {
        OOB_FATAL("Error while listening");
    }
    OOB_LOG("Server is listening for incoming connections on port %d", ctx->port);
    server_accept(ctx);
}

static void client_setup(struct oob_sock_ctx *ctx, const char *server_host) {
    ctx->data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->data_fd < 0) {
        OOB_FATAL("Unable to create socket");
    }
    OOB_DEBUG("Client socket created successfully");

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ctx->port);
    server_addr.sin_addr.s_addr = inet_addr(server_host);
    if (connect(ctx->data_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        OOB_FATAL("Unable to connect to server at %s with socket fd %d", server_host, ctx->data_fd);
    }
    OOB_LOG("Connected with server successfully");
}

void oob_sock_ctx_create(const char *server_addr, int port, struct oob_sock_ctx **ctx) {
    struct oob_sock_ctx *new_ctx = calloc(1, sizeof(*new_ctx));
    if (!new_ctx) {
        OOB_FATAL("Failed to allocate OOB context");
    }
    new_ctx->is_server = server_addr ? 0 : 1;
    new_ctx->port = port;
    if (new_ctx->is_server) {
        server_setup(new_ctx);
    } else {
        if (!strcmp(server_addr, "")) {
            OOB_FATAL("Server address not specified");
        }
        client_setup(new_ctx, server_addr);
    }
    *ctx = new_ctx;
}

void oob_sock_ctx_destroy(struct oob_sock_ctx *ctx) {
    if (!ctx->is_server) {
        close(ctx->data_fd);
    }
    if (ctx->is_server) {
        close(ctx->server.listen_fd);
    }
    free(ctx);
}

void oob_sock_send(struct oob_sock_ctx *ctx, const void *sbuf, size_t size) {
    if (sock_send(ctx->data_fd, sbuf, size)) {
        OOB_FATAL("OOB context failed to send data with errno %s", strerror(errno));
    }
}

void oob_sock_recv(struct oob_sock_ctx *ctx, void *rbuf, size_t size) {
    if (sock_recv(ctx->data_fd, rbuf, size)) {
        OOB_FATAL("OOB context failed to recv data with and errno %s", strerror(errno));
    }
}
