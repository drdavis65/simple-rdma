#ifndef _OOB_IBV_H_
#define _OOB_IBV_H_

#include <contrib/lwlog.h>
#include <infiniband/verbs.h>
#include <stdlib.h>

#define OOB_IBV_ASSERT(EXPR)                                                                       \
    {                                                                                              \
        if (!(EXPR)) {                                                                             \
            lwlog_crit("OOB_IBV ASSERTION FAILED: %s", #EXPR);                                     \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    }

#define OOB_IBV_CRITICAL(FORMAT, ...)                                                              \
    { lwlog_crit(FORMAT, ##__VA_ARGS__); }

#define OOB_IBV_DEBUG(FORMAT, ...)                                                                 \
    { lwlog_debug(FORMAT, ##__VA_ARGS__); }

#define MAC_ADDR_LEN 6
#define MAX_SQ_CAPACITY 4096
#define MAX_SEND_SGE 1
#define MAX_RECV_SGE 1
#define MAX_INLINE_DATA 64

struct oob_ibv_ctx {
    const char *dev_name;
    struct ibv_context *ibv_ctx;
    struct ibv_pd *ibv_pd;
};

struct oob_ibv_mr {
    struct ibv_mr *ibv_mr;
};

struct oob_ibv_cq_attr {
    uint32_t cq_depth;
};

struct oob_ibv_cq {
    struct ibv_cq *ibv_cq;
};

struct oob_qp_remote_info {
    uint8_t mac_addr[MAC_ADDR_LEN];
    union ibv_gid gid;
    uint8_t gid_table_index;
    uint16_t lid;
    uint32_t qp_num;
};

struct oob_qp_info {
    uint8_t mac_addr[MAC_ADDR_LEN];
    union ibv_gid gid;
    uint8_t gid_table_index;
    uint16_t lid;
    uint32_t num_qps;
    uint8_t send_enable;
    uint8_t recv_enable;
};

struct oob_ibv_qp_attr {
    enum ibv_qp_type qp_type;
    struct oob_ibv_cq *send_cq;
    struct oob_ibv_cq *recv_cq;
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_rd_atomic;
    uint32_t max_dest_rd_atomic;
    uint32_t rnr_retry;
};

struct oob_ibv_qp {
    struct oob_ibv_ctx *ctx;
    struct ibv_qp *ibv_qp;
    uint32_t qp_num;
    struct oob_ibv_qp_attr attr;
    struct ibv_send_wr swrs[MAX_SQ_CAPACITY];
    struct ibv_sge sges[MAX_SQ_CAPACITY];
};

int oob_ibv_ctx_create(const char *dev_name, struct oob_ibv_ctx *ctx);
int oob_ibv_ctx_destroy(struct oob_ibv_ctx *ctx);
int oob_ibv_mr_reg(struct oob_ibv_ctx *ctx, void *addr, size_t length, enum ibv_access_flags access,
                   struct oob_ibv_mr *mr);
int oob_ibv_mr_dereg(struct oob_ibv_mr *mr);
int oob_ibv_cq_create(struct oob_ibv_ctx *ctx, struct oob_ibv_cq_attr *attr, struct oob_ibv_cq *cq);
int oob_ibv_cq_destroy(struct oob_ibv_cq *cq);
int oob_ibv_qp_create(struct oob_ibv_ctx *ctx, struct oob_ibv_qp_attr *attr, struct oob_ibv_qp *qp);
int oob_ibv_qp_destroy(struct oob_ibv_qp *qp);
int oob_ibv_qp_remote_info_get(struct oob_ibv_ctx *ctx, struct oob_ibv_qp *qp,
                               struct oob_qp_remote_info *info);
int oob_ibv_qp_connect(struct oob_ibv_qp *qp, struct oob_qp_remote_info *remote_dev_info);
int oob_ibv_qp_signal_recv_post(struct oob_ibv_qp *qp);
int oob_ibv_qp_signal_send_post(struct oob_ibv_qp *qp, uint32_t signal);
int oob_ibv_qp_write_post(struct oob_ibv_qp *qp, void *buf, uint32_t lkey, size_t length,
                          uint64_t raddr, uint32_t rkey, uint32_t imm_data);
int oob_ibv_cq_signal_wait(struct oob_ibv_cq *cq, uint32_t *signal);
int oob_ibv_cq_cqe_batch_wait(struct oob_ibv_cq *cq, size_t batch_size);
int oob_ibv_cq_cqe_batch_poll(struct oob_ibv_cq *cq, size_t batch_size, size_t *compls);

#endif /* _OOB_IBV_H_ */