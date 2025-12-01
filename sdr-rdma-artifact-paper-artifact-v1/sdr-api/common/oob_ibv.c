#include <net/if.h>

#include "oob_ibv.h"

int oob_ibv_mr_reg(struct oob_ibv_ctx *ctx, void *addr, size_t length, enum ibv_access_flags access,
                   struct oob_ibv_mr *mr) {
    mr->ibv_mr = ibv_reg_mr(ctx->ibv_pd, addr, length, access);
    OOB_IBV_ASSERT(mr->ibv_mr);
    return 0;
}

int oob_ibv_mr_dereg(struct oob_ibv_mr *mr) { return ibv_dereg_mr(mr->ibv_mr); }

int oob_ibv_ctx_create(const char *dev_name, struct oob_ibv_ctx *ctx) {
    struct ibv_device **dev_list;
    struct ibv_device *dev;
    int i;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        OOB_IBV_CRITICAL("IBV: Failed to get IB devices list");
        return 1;
    }

    for (i = 0; dev_list[i]; i++) {
        if (!strcmp(ibv_get_device_name(dev_list[i]), dev_name)) {
            break;
        }
    }

    dev = dev_list[i];
    if (!dev) {
        OOB_IBV_CRITICAL("IBV: No IB devices found");
        ibv_free_device_list(dev_list);
        return 1;
    }

    ctx->ibv_ctx = ibv_open_device(dev);
    if (ctx->ibv_ctx == NULL) {
        OOB_IBV_CRITICAL("IBV: Couldn't get ibv context for %s", ibv_get_device_name(dev));
        ibv_free_device_list(dev_list);
        return 1;
    }

    ibv_free_device_list(dev_list);

    ctx->ibv_pd = ibv_alloc_pd(ctx->ibv_ctx);

    return 0;
}

int oob_ibv_ctx_destroy(struct oob_ibv_ctx *ctx) {
    ibv_dealloc_pd(ctx->ibv_pd);
    return ibv_close_device(ctx->ibv_ctx);
}

int oob_ibv_qp_create(struct oob_ibv_ctx *ctx, struct oob_ibv_qp_attr *attr,
                      struct oob_ibv_qp *qp) {
    struct ibv_qp_init_attr ibv_qp_fattr = {0};
    qp->attr = *attr;
    qp->ctx = ctx;

    ibv_qp_fattr.send_cq = attr->send_cq->ibv_cq;
    ibv_qp_fattr.recv_cq = attr->recv_cq->ibv_cq;
    ibv_qp_fattr.qp_type = attr->qp_type;
    ibv_qp_fattr.cap.max_send_wr = attr->max_send_wr;
    ibv_qp_fattr.cap.max_recv_wr = attr->max_recv_wr;
    ibv_qp_fattr.cap.max_send_sge = MAX_SEND_SGE;
    ibv_qp_fattr.cap.max_recv_sge = MAX_RECV_SGE;
    ibv_qp_fattr.cap.max_inline_data = MAX_INLINE_DATA;

    qp->ibv_qp = ibv_create_qp(ctx->ibv_pd, &ibv_qp_fattr);
    OOB_IBV_ASSERT(qp->ibv_qp != NULL);
    qp->qp_num = qp->ibv_qp->qp_num;

    return 0;
}

int oob_ibv_qp_destroy(struct oob_ibv_qp *qp) { return ibv_destroy_qp(qp->ibv_qp); }

int oob_ibv_qp_connect(struct oob_ibv_qp *qp, struct oob_qp_remote_info *remote_dev_info) {
    struct ibv_qp_attr attr = {0};
    int ret = 0;

    // resolve address
    struct ibv_ah_attr ah_attr = {0};
    ah_attr.is_global = 1;
    ah_attr.port_num = 1;
    ah_attr.grh.sgid_index = remote_dev_info->gid_table_index;
    ah_attr.grh.hop_limit = 64;
    ah_attr.grh.traffic_class = 106;
    ah_attr.grh.flow_label = 12381;

    memcpy(ah_attr.grh.dgid.raw, remote_dev_info->gid.raw, sizeof(remote_dev_info->gid.raw));
    ibv_resolve_eth_l2_from_gid(qp->ctx->ibv_ctx, &ah_attr, remote_dev_info->mac_addr, NULL);

    // init
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 0x1;
    attr.qp_access_flags =
        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

    ret = ibv_modify_qp(qp->ibv_qp, &attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    OOB_IBV_ASSERT(ret == 0);

    // to RTR
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = remote_dev_info->qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = qp->attr.max_dest_rd_atomic;
    attr.min_rnr_timer = 30;
    attr.ah_attr.dlid = remote_dev_info->lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 0x1;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = remote_dev_info->gid;
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = remote_dev_info->gid_table_index;
    attr.ah_attr.grh.traffic_class = 100;

    int attr_mask = 0;
    if (qp->attr.qp_type == IBV_QPT_RC) {
        attr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                    IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    } else if (qp->attr.qp_type == IBV_QPT_UC) {
        attr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
    } else {
        OOB_IBV_CRITICAL("IBV QP not supported!");
    }

    ret = ibv_modify_qp(qp->ibv_qp, &attr, attr_mask);
    OOB_IBV_ASSERT(ret == 0);

    // to RTS
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.timeout = 0;
    attr.retry_cnt = qp->attr.rnr_retry ? 6 : 0;
    attr.rnr_retry = qp->attr.rnr_retry ? 7 : 0;
    attr.max_rd_atomic = qp->attr.max_rd_atomic;

    if (qp->attr.qp_type == IBV_QPT_RC) {
        attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                    IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    } else if (qp->attr.qp_type == IBV_QPT_UC) {
        attr_mask = IBV_QP_STATE | IBV_QP_SQ_PSN;
    } else {
        OOB_IBV_CRITICAL("IBV QP not supported!");
    }

    ret = ibv_modify_qp(qp->ibv_qp, &attr, attr_mask);
    OOB_IBV_ASSERT(ret == 0);

    return 0;
}

static int oob_qp_dev_info(struct oob_ibv_ctx *ctx, struct oob_qp_remote_info *info) {
    int default_port_num = 1;
    int max_gid_table_entries = 32;
    int if_namesize = 32;
    int max_path_len = 8192;
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx->ibv_ctx, default_port_num, &port_attr)) {
        OOB_IBV_CRITICAL("Couldn't query ibv port%d\n", default_port_num);
        return 0;
    }

    info->lid = port_attr.lid;
    struct ibv_gid_entry *gid_tbl_entries =
        calloc(max_gid_table_entries, sizeof(struct ibv_gid_entry));
    if (!gid_tbl_entries) {
        OOB_IBV_CRITICAL("Couldn't allocate memory for gid table\n");
        return 0;
    }

    int num_entries = ibv_query_gid_table(ctx->ibv_ctx, gid_tbl_entries, max_gid_table_entries, 0);
    int dev_found = 0;
    for (int i = 3; i < num_entries && !dev_found; i++) {
        if (gid_tbl_entries[i].gid_type == IBV_GID_TYPE_IB) {
            info->gid_table_index = gid_tbl_entries[i].gid_index;
            info->gid = gid_tbl_entries[i].gid;
            info->mac_addr[0] = 0;
            info->mac_addr[1] = 0;
            info->mac_addr[2] = 0;
            info->mac_addr[3] = 0;
            info->mac_addr[4] = 0;
            info->mac_addr[5] = 0;
            dev_found = 1;
        }

        if (gid_tbl_entries[i].gid_type == IBV_GID_TYPE_ROCE_V2) {
            char ifname[if_namesize];
            char sys_path[max_path_len];
            if_indextoname(gid_tbl_entries[i].ndev_ifindex, ifname);
            snprintf(sys_path, max_path_len, "/sys/class/net/%s/address", ifname);
            FILE *maddr_file = fopen(sys_path, "r");
            if (!maddr_file) {
                OOB_IBV_CRITICAL("Couldn't open %s\n", sys_path);
                return 0;
            }
            int ret = fscanf(maddr_file, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx%*c", &info->mac_addr[0],
                             &info->mac_addr[1], &info->mac_addr[2], &info->mac_addr[3],
                             &info->mac_addr[4], &info->mac_addr[5]);
            if (ret != 6) {
                OOB_IBV_CRITICAL("Failed to parse maddr file\n");
                return 0;
            }
            fclose(maddr_file);
            info->gid_table_index = gid_tbl_entries[i].gid_index;
            info->gid = gid_tbl_entries[i].gid;
            dev_found = 1;
        }
    }
    free(gid_tbl_entries);
    return dev_found;
}

int oob_ibv_qp_remote_info_get(struct oob_ibv_ctx *ctx, struct oob_ibv_qp *qp,
                               struct oob_qp_remote_info *info) {
    if (!oob_qp_dev_info(ctx, info)) {
        OOB_IBV_CRITICAL("Failed to find device info");
        return 1;
    }
    info->qp_num = qp->qp_num;
    return 0;
}

int oob_ibv_qp_signal_recv_post(struct oob_ibv_qp *qp) {
    struct ibv_recv_wr wr = {0}, *bad_wr = NULL;

    if (ibv_post_recv(qp->ibv_qp, &wr, &bad_wr)) {
        OOB_IBV_CRITICAL("ibv_post_recv failed");
        return 1;
    }

    return 0;
}

int oob_ibv_qp_signal_send_post(struct oob_ibv_qp *qp, uint32_t signal) {
    struct ibv_send_wr wr = {0}, *bad_wr = NULL;

    wr.opcode = IBV_WR_SEND_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = signal;

    if (ibv_post_send(qp->ibv_qp, &wr, &bad_wr)) {
        OOB_IBV_CRITICAL("ibv_post_send failed");
        return 1;
    }

    return 0;
}

int oob_ibv_qp_write_post(struct oob_ibv_qp *qp, void *buf, uint32_t lkey, size_t length,
                          uint64_t raddr, uint32_t rkey, uint32_t imm_data) {
    struct ibv_send_wr wr = {0}, *bad_wr = NULL;

    struct ibv_sge sge;
    sge.addr = (uintptr_t) buf;
    sge.length = length;
    sge.lkey = lkey;

    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = raddr;
    wr.wr.rdma.rkey = rkey;
    wr.imm_data = imm_data;

    if (ibv_post_send(qp->ibv_qp, &wr, &bad_wr)) {
        OOB_IBV_CRITICAL("ibv_post_send failed");
        return 1;
    }

    return 0;
}

int oob_ibv_cq_signal_wait(struct oob_ibv_cq *cq, uint32_t *signal) {
    struct ibv_wc wc;
    while (1) {
        int num_completions = ibv_poll_cq(cq->ibv_cq, 1, &wc);
        if (num_completions < 0) {
            OOB_IBV_CRITICAL("ibv_poll_cq failed, wc.status=%d", wc.status);
            return 1;
        } else if (num_completions > 0) {
            OOB_IBV_ASSERT(num_completions == 1);
            OOB_IBV_ASSERT(wc.status == IBV_WC_SUCCESS);
            *signal = wc.imm_data;
            return 0;
        }
    }
    return 1;
}

int oob_ibv_cq_cqe_batch_wait(struct oob_ibv_cq *cq, size_t batch_size) {
    struct ibv_wc wc[batch_size];
    size_t total_compls = 0;
    while (total_compls != batch_size) {
        int num_completions = ibv_poll_cq(cq->ibv_cq, batch_size, wc);
        if (num_completions < 0) {
            OOB_IBV_CRITICAL("ibv_poll_cq failed");
            return 1;
        } else if (num_completions > 0) {
            for (size_t i = 0; i < num_completions; i++) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    OOB_IBV_CRITICAL("WC %zu status: %s", total_compls + i,
                                     ibv_wc_status_str(wc[i].status));
                }
            }
            total_compls += num_completions;
        }
    }
    return 0;
}

int oob_ibv_cq_cqe_batch_poll(struct oob_ibv_cq *cq, size_t batch_size, size_t *compls) {
    struct ibv_wc wc[batch_size];
    while (1) {
        int num_completions = ibv_poll_cq(cq->ibv_cq, batch_size, wc);
        if (num_completions < 0) {
            OOB_IBV_CRITICAL("ibv_poll_cq failed");
            return 1;
        } else {
            for (size_t i = 0; i < num_completions; i++) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    OOB_IBV_CRITICAL("WC %zu status: %s", i, ibv_wc_status_str(wc[i].status));
                }
            }
            *compls = num_completions;
            return 0;
        }
    }
    return 1;
}

int oob_ibv_cq_create(struct oob_ibv_ctx *ctx, struct oob_ibv_cq_attr *attr,
                      struct oob_ibv_cq *cq) {
    cq->ibv_cq = ibv_create_cq(ctx->ibv_ctx, attr->cq_depth, NULL, NULL, 0);
    OOB_IBV_ASSERT(cq->ibv_cq);
    return 0;
}

int oob_ibv_cq_destroy(struct oob_ibv_cq *cq) { return ibv_destroy_cq(cq->ibv_cq); }