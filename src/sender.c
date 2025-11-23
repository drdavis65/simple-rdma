#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdio.h>
#include <infiniband/verbs.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include "devinfo.h"
#include "rdma_common.h"

struct sender_context {
    struct ibv_context *ctx;
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
//    struct ibv_dm       *dm;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    char *buf;
    int size;
    int num_packets;
    struct ibv_port_attr portinfo;
    
    // Connection info for QP modification
    uint32_t remote_qpn;        // Remote QP number
    uint32_t sq_psn;            // Send queue PSN
    uint32_t rq_psn;            // Receive queue PSN
    struct ibv_ah *ah;          // Address Handle (for routing)
    uint32_t remote_rkey;       // Remote memory region key (for RDMA ops)
};

// Transition QP to RTR (Ready to Receive) state
static int modify_qp_to_rtr(struct sender_context *ctx, struct ibv_ah_attr *ah_attr) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = ctx->portinfo.active_mtu,
        .dest_qp_num = ctx->remote_qpn,
        .rq_psn = ctx->rq_psn,
        .max_dest_rd_atomic = 16,
        .min_rnr_timer = 0x12,
        .ah_attr = *ah_attr
    };
    
    int rtr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | 
                   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | 
                   IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    
    if(ibv_modify_qp(ctx->qp, &attr, rtr_mask)) {
        perror("Failed to modify QP to RTR");
        return -1;
    }
    printf("QP transitioned to RTR\n");
    return 0;
}

// Transition QP to RTS (Ready to Send) state
static int modify_qp_to_rts(struct sender_context *ctx) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 0x12,
        .retry_cnt = 6,
        .rnr_retry = 7,
        .sq_psn = ctx->sq_psn
    };
    
    int rts_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                   IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN;
    
    if(ibv_modify_qp(ctx->qp, &attr, rts_mask)) {
        perror("Failed to modify QP to RTS");
        return -1;
    }
    printf("QP transitioned to RTS - ready to send!\n");
    return 0;
}


int main(int argc, char *argv[]) {
    const char *receiver_ip = "127.0.0.1";  // Default to localhost
    int tcp_port = RDMA_TCP_PORT;
    
    // Parse command-line arguments: ./sender [receiver_ip] [port]
    if(argc > 1) {
        receiver_ip = argv[1];
    }
    if(argc > 2) {
        tcp_port = atoi(argv[2]);
        if(tcp_port <= 0 || tcp_port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[2]);
            return 1;
        }
    }
    
    printf("Sender connecting to receiver at %s:%d\n", receiver_ip, tcp_port);
    
    int num_devices = 0;

    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);

    if(!dev_list) {
        perror("ibv_get_device_list");
    }

    printf("Found %d ibv devices\n", num_devices);;

    for(int i = 0; i < num_devices; i++) {
        struct ibv_device* dev = dev_list[i];
        printf("    %d name: %s\n", i, ibv_get_device_name(dev)); 
    }

    const char* dev_name = ibv_get_device_name(dev_list[0]);

    struct sender_context *send_ctx = malloc(sizeof(*send_ctx));

    send_ctx->ctx = ibv_open_device(dev_list[0]);

    if(!send_ctx->ctx) {
        perror("ibv_open_device");
    }

    struct ibv_device_attr *dev_attr = malloc(sizeof(*dev_attr));
    memset(dev_attr, 0, sizeof(*dev_attr));

    if(ibv_query_device(send_ctx->ctx, dev_attr)) {
        perror("ibv_query_device");
    }

    printf("For device: %s\n    Max mr size: %" PRIu64 
           "\n    Max qp: %" PRIu32 
           "\n    Max qp wr: %d\n",
           dev_name, 
           dev_attr->max_mr_size,
           dev_attr->max_qp,
           dev_attr->max_qp_wr);

    memset(&send_ctx->portinfo, 0, sizeof(send_ctx->portinfo));

    if(ibv_query_port(send_ctx->ctx, 1, &send_ctx->portinfo)) {
        perror("ibv_query_port");
    }

    printf("Device state: %s\nActive mtu: %s\nSpeed: %s\n",
           port_state_str(send_ctx->portinfo.state),
           mtu_str(send_ctx->portinfo.active_mtu),
           speed_str(send_ctx->portinfo.active_speed));

    union ibv_gid* gid = malloc(sizeof(*gid));

    if(ibv_query_gid(send_ctx->ctx, 1, 3, gid)) {
        perror("ibv_query_gid");
    }

    struct ibv_gid_entry* entry = malloc(sizeof(*entry));
    
    if(ibv_query_gid_ex(send_ctx->ctx, 1, 3, entry, 0)) {
        perror("ibv_query_gid_ex");
    }

    printf("GID type: %s\n", gid_type_str(entry->gid_type)); 

    send_ctx->num_packets = 3;

    send_ctx->size = 3 * 1024 * sizeof(char);

    // Use posix_memalign for page-aligned memory (required for RDMA)
    if(posix_memalign((void**)&send_ctx->buf, sysconf(_SC_PAGESIZE), send_ctx->size)) {
        perror("posix_memalign");
        return 1;
    }
    memset(send_ctx->buf, 0, send_ctx->size);

    send_ctx->channel = ibv_create_comp_channel(send_ctx->ctx);
    if(!send_ctx->channel) {
        perror("ibv_create_comp_channel");
        return 1;
    }

    send_ctx->pd = ibv_alloc_pd(send_ctx->ctx);
    if(!send_ctx->pd) {
        perror("ibv_alloc_pd");
        return 1;
    }

    send_ctx->mr = ibv_reg_mr(send_ctx->pd, send_ctx->buf, send_ctx->size, IBV_ACCESS_LOCAL_WRITE);
    if(!send_ctx->mr) {
        perror("ibv_reg_mr");
        return 1;
    }

    send_ctx->cq = ibv_create_cq(send_ctx->ctx, send_ctx->num_packets, NULL, send_ctx->channel, 0);
    if(!send_ctx->cq) {
        perror("ibv_create_cq");
        return 1;
    }
    
    {
        struct ibv_qp_attr attr;

        struct ibv_qp_init_attr init_attr = {
            .send_cq = send_ctx->cq,
            .recv_cq = send_ctx->cq,
            .cap     = {
                .max_send_wr = 3,
                .max_recv_wr = 1,
                .max_send_sge = 1,
                .max_recv_sge = 1,
            },
            .qp_type = IBV_QPT_RC
        };

        send_ctx->qp = ibv_create_qp(send_ctx->pd, &init_attr);

        if(!send_ctx->qp) {
            perror("ibv_create_qp");
        }

        ibv_query_qp(send_ctx->qp, &attr, IBV_QP_CAP, &init_attr);
    }
    {
        struct ibv_qp_attr attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = 1,
            .qp_access_flags = 0
        };

        if(ibv_modify_qp(send_ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            perror("Failed to modify QP to INIT");
            return 1;
        }
    }

    // Step 1: Get local QP number and generate PSNs
    struct ibv_qp_attr qp_attr;
    struct ibv_qp_init_attr qp_init_attr;
    if(ibv_query_qp(send_ctx->qp, &qp_attr, IBV_QP_QP_NUM, &qp_init_attr)) {
        perror("ibv_query_qp");
        return 1;
    }
    uint32_t local_qpn = send_ctx->qp->qp_num;
    printf("Local QP number: %u\n", local_qpn);
    
    // Generate PSNs (Packet Sequence Numbers)
    // PSN is a 24-bit value used for reliable delivery and ordering.
    // It doesn't need to be random - any value works as long as both sides agree.
    // Using random helps avoid collisions in multi-connection scenarios.
    srand(time(NULL));  // Initialize random seed
    send_ctx->sq_psn = rand() & 0xFFFFFF;  // 24-bit PSN (could also use 0 or any fixed value)
    send_ctx->rq_psn = 0;  // Will be set from remote side
    
    // Step 2: Setup TCP client and exchange connection information
    int tcp_sock = setup_tcp_client(receiver_ip, tcp_port);
    if(tcp_sock < 0) {
        return 1;
    }
    
    // Prepare local connection info
    struct rdma_conn_info local_info = {
        .qpn = local_qpn,
        .psn = send_ctx->sq_psn,
        .gid = *gid,
        .lid = send_ctx->portinfo.lid,
        .rkey = 0  // Not needed for send operations
    };
    
    struct rdma_conn_info remote_info;
    
    // Exchange connection information
    if(exchange_conn_info_as_sender(tcp_sock, &local_info, &remote_info)) {
        close(tcp_sock);
        return 1;
    }
    
    printf("Received remote info: QPN=%u, PSN=%u\n", remote_info.qpn, remote_info.psn);
    
    // Store remote connection info
    send_ctx->remote_qpn = remote_info.qpn;
    send_ctx->rq_psn = remote_info.psn;
    
    close(tcp_sock);
    
    // Step 3: Create Address Handle (AH) for routing to remote
    struct ibv_ah_attr ah_attr = {
        .is_global = 1,
        .port_num = 1,
        .grh = {
            .dgid = remote_info.gid,  // Use remote GID from connection exchange
            .flow_label = 0,
            .sgid_index = 3,  // GID index you queried earlier
            .hop_limit = 255,
            .traffic_class = 0
        }
    };
    
    send_ctx->ah = ibv_create_ah(send_ctx->pd, &ah_attr);
    if(!send_ctx->ah) {
        perror("ibv_create_ah");
        return 1;
    }
    
    // Step 4: Transition QP to RTR (Ready to Receive)
    if(modify_qp_to_rtr(send_ctx, &ah_attr)) {
        return 1;
    }
    
    // Step 5: Transition QP to RTS (Ready to Send)
    if(modify_qp_to_rts(send_ctx)) {
        return 1;
    }
    
    // Step 6: Prepare data to send
    strcpy(send_ctx->buf, "Hello, RDMA!");
    size_t send_len = strlen(send_ctx->buf) + 1;
    
    // Step 7: Post a send work request
    struct ibv_sge sge = {
        .addr = (uintptr_t)send_ctx->buf,
        .length = send_len,
        .lkey = send_ctx->mr->lkey
    };
    
    struct ibv_send_wr wr = {
        .wr_id = 1,  // User-defined ID for this WR
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED  // Request completion notification
    };
    
    struct ibv_send_wr *bad_wr;
    if(ibv_post_send(send_ctx->qp, &wr, &bad_wr)) {
        perror("ibv_post_send");
        return 1;
    }
    printf("Posted send work request\n");
    
    // Step 8: Poll for completion
    struct ibv_wc wc;
    int num_completions = 0;
    do {
        num_completions = ibv_poll_cq(send_ctx->cq, 1, &wc);
    } while(num_completions == 0);
    
    if(wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Work completion error: %s\n", ibv_wc_status_str(wc.status));
        return 1;
    }
    
    printf("Send completed successfully! (wr_id: %lu)\n", wc.wr_id);

    return 0;
}        
        
