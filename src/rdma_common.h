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

#endif // RDMA_COMMON_H

