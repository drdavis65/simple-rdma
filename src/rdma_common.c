#include "rdma_common.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

// Setup TCP server socket (for receiver)
int setup_tcp_server(int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sockfd);
        return -1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if(bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    if(listen(sockfd, 1) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
    
    printf("TCP server listening on port %d\n", port);
    return sockfd;
}

// Setup TCP client socket and connect to server (for sender)
int setup_tcp_client(const char *server_ip, int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    struct hostent *server;
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    server = gethostbyname(server_ip);
    if(server == NULL) {
        fprintf(stderr, "Error: No such host %s\n", server_ip);
        close(sockfd);
        return -1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);
    
    if(connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    
    printf("Connected to receiver at %s:%d\n", server_ip, port);
    return sockfd;
}

// Exchange RDMA connection information via TCP
// Receiver version: sends first, then receives
int exchange_conn_info_as_receiver(int sockfd, struct rdma_conn_info *local_info, 
                                    struct rdma_conn_info *remote_info) {
    // Send local info first
    if(send(sockfd, local_info, sizeof(*local_info), 0) != sizeof(*local_info)) {
        perror("send");
        return -1;
    }
    
    // Receive remote info
    if(recv(sockfd, remote_info, sizeof(*remote_info), MSG_WAITALL) != sizeof(*remote_info)) {
        perror("recv");
        return -1;
    }
    
    return 0;
}

// Exchange RDMA connection information via TCP
// Sender version: receives first, then sends
int exchange_conn_info_as_sender(int sockfd, struct rdma_conn_info *local_info, 
                                  struct rdma_conn_info *remote_info) {
    // Receive remote info first (receiver sends first)
    if(recv(sockfd, remote_info, sizeof(*remote_info), MSG_WAITALL) != sizeof(*remote_info)) {
        perror("recv");
        return -1;
    }
    
    // Send local info
    if(send(sockfd, local_info, sizeof(*local_info), 0) != sizeof(*local_info)) {
        perror("send");
        return -1;
    }
    
    return 0;
}

int modify_qp_to_rtr(struct SDR_context *ctx, struct ibv_ah_attr *ah_attr) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = ctx->portinfo.active_mtu,
        .dest_qp_num = ctx->remote_qpn,
        .rq_psn = ctx->rq_psn,
        .ah_attr = *ah_attr
    };
    
    // For UC QP type, RTR requires: STATE, AV, PATH_MTU, DEST_QPN, RQ_PSN
    // (max_dest_rd_atomic and min_rnr_timer are for RC QP type)
    int rtr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | 
                   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
    
    if(ibv_modify_qp(ctx->qp, &attr, rtr_mask)) {
        perror("Failed to modify QP to RTR");
        return -1;
    }
    printf("QP transitioned to RTR\n");
    return 0;
}

// Transition QP to RTS (Ready to Send) state
// For UC QP type, only IBV_QP_STATE and IBV_QP_SQ_PSN are required
// (timeout, retry_cnt, rnr_retry, max_rd_atomic are for RC QP type)
int modify_qp_to_rts(struct SDR_context *ctx) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = ctx->sq_psn
    };
    
    // For UC QP type, RTS only requires STATE and SQ_PSN
    int rts_mask = IBV_QP_STATE | IBV_QP_SQ_PSN;
    
    if(ibv_modify_qp(ctx->qp, &attr, rts_mask)) {
        perror("Failed to modify QP to RTS");
        return -1;
    }
    printf("QP transitioned to RTS\n");
    return 0;
}

struct SDR_context* context_create(char* req_dev_name) {
    int num_devices = 0;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    
    if(!dev_list) {
        perror("ibv_get_device_list");
        return 1;
    }
    
    printf("Found %d ibv devices\n", num_devices);
    
    if(num_devices == 0) {
        fprintf(stderr, "No InfiniBand devices found\n");
        return 1;
    }
    
    struct SDR_context *ctx_ = malloc(sizeof(*ctx_));
    memset(ctx_, 0, sizeof(*ctx_));
    for(int i = 0; i < num_devices; i++) {
        struct ibv_device* dev = dev_list[i];
        printf("    %d name: %s\n", i, ibv_get_device_name(dev)); 
        if(strcmp(req_dev_name, ibv_get_device_name(dev)) == 0) {
            ctx_->ctx = ibv_open_device(dev_list[i]);
            if(!ctx_->ctx) {
                perror("ibv_open_device");
                return 1;
            }
            break;
        }
    }
    
    struct ibv_device_attr *dev_attr = malloc(sizeof(*dev_attr));
    memset(dev_attr, 0, sizeof(*dev_attr));
    
    if(ibv_query_device(ctx_->ctx, dev_attr)) {
        perror("ibv_query_device");
        return 1;
    }
    
    printf("For device: %s\n    Max mr size: %" PRIu64 
           "\n    Max qp: %" PRIu32 
           "\n    Max qp wr: %d\n",
           req_dev_name, 
           dev_attr->max_mr_size,
           dev_attr->max_qp,
           dev_attr->max_qp_wr);
    
    memset(&ctx_->portinfo, 0, sizeof(ctx_->portinfo));
    
    if(ibv_query_port(ctx_->ctx, 1, &ctx_->portinfo)) {
        perror("ibv_query_port");
        return 1;
    }
    
    printf("Device state: %s\nActive mtu: %s\nSpeed: %s\n",
           port_state_str(ctx_->portinfo.state),
           mtu_str(ctx_->portinfo.active_mtu),
           speed_str(ctx_->portinfo.active_speed));
    
    union ibv_gid* gid = malloc(sizeof(*gid));
    
    ctx_->gid = gid;

    if(ibv_query_gid(ctx_->ctx, 1, 3, gid)) {
        perror("ibv_query_gid");
        return 1;
    }
    
    struct ibv_gid_entry* entry = malloc(sizeof(*entry));
    if(ibv_query_gid_ex(ctx_->ctx, 1, 3, entry, 0)) {
        perror("ibv_query_gid_ex");
        return 1;
    }
    
    printf("GID type: %s\n", gid_type_str(entry->gid_type)); 

    return ctx_;
}
