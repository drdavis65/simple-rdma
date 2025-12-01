/**
 * @file oob.h
 * @brief Defines out-of-band communication primitives
 */

#ifndef _OOB_SOCK_H_
#define _OOB_SOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>

#define OOB_DEFAULT_PORT 13382

struct oob_sock_ctx {
    int data_fd;
    int is_server;
    struct server {
        int listen_fd;
        int reuse;
    } server;
    int port;
};

/**
 * @brief Create and initialize out-of-band communication channel.
 *
 * @param server_addr server address (should be set on client side).
 * @param port server port (should be set on client side).
 * @param ctx OOB context handler.
 */
void oob_sock_ctx_create(const char *server_addr, int port, struct oob_sock_ctx **ctx);

/**
 * @brief Destroy out-of-band communication channel.
 *
 * @param ctx OOB context handler.
 */
void oob_sock_ctx_destroy(struct oob_sock_ctx *ctx);

/**
 * @brief Send buffer through out-of-band communication channel.
 *
 * @param ctx OOB context handler.
 * @param buf pointer to the buffer.
 * @param size size of the buffer
 */
void oob_sock_send(struct oob_sock_ctx *ctx, const void *buf, size_t size);

/**
 * @brief Receive buffer through out-of-band communication channel.
 *
 * @param ctx OOB context handler.
 * @param buf pointer to the receive buffer.
 * @param size size of the buffer
 */
void oob_sock_recv(struct oob_sock_ctx *ctx, void *buf, size_t size);

#endif // _OOB_SOCK_H_
