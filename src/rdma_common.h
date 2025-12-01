#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <stdint.h>
#include <infiniband/verbs.h>

// Default TCP port for RDMA connection establishment
#define RDMA_TCP_PORT 18515

// Structure for exchanging RDMA connection information
struct rdma_conn_info {
    uint32_t qpn;           // Queue Pair Number
    uint32_t psn;           // Packet Sequence Number
    union ibv_gid gid;      // Global ID
    uint16_t lid;           // Local ID (for InfiniBand)
    uint32_t rkey;          // Remote memory region key (for RDMA ops)
    uint64_t remote_addr;   // Remote memory address (for RDMA ops)
};

// TCP connection establishment functions
// Setup TCP server socket (for receiver)
int setup_tcp_server(int port);

// Setup TCP client socket and connect to server (for sender)
int setup_tcp_client(const char *server_ip, int port);

// Exchange RDMA connection information via TCP
// Receiver version: sends first, then receives
int exchange_conn_info_as_receiver(int sockfd, struct rdma_conn_info *local_info, 
                                    struct rdma_conn_info *remote_info);

// Exchange RDMA connection information via TCP
// Sender version: receives first, then sends
int exchange_conn_info_as_sender(int sockfd, struct rdma_conn_info *local_info, 
                                 struct rdma_conn_info *remote_info);

struct SDR_context {
    struct ibv_context *ctx;
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
//    struct ibv_dm       *dm;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_qp_ex *qpx;      // Extended QP for advanced operations
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
    uint64_t remote_addr;       // Remote memory address (for RDMA ops)
};

int modify_qp_to_rtr(struct SDR_context *ctx, struct ibv_ah_attr *ah_attr);

int modify_qp_to_rts(struct SDR_context *ctx); 



#endif // RDMA_COMMON_H

