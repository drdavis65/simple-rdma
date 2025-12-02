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
#include "rdma_common.h"


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
    
    struct SDR_context* send_ctx = context_create("mlx5_0");

    send_ctx->size = 3 * 1024 * sizeof(char);
    send_ctx->num_packets = send_ctx->size / send_ctx->portinfo.active_mtu;;
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
        // Create QP as extended to support advanced operations (e.g., RDMA write with immediate)
        struct ibv_qp_init_attr_ex init_attr_ex = {
            .send_cq = send_ctx->cq,
            .recv_cq = send_ctx->cq,
            .cap     = {
                .max_send_wr = 3,
                .max_recv_wr = 1, //extend to rx_depth here
                .max_send_sge = 1,
                .max_recv_sge = 1,
            },
            .qp_type = IBV_QPT_UC,
            .comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
            .pd = send_ctx->pd,
            .send_ops_flags = IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_WRITE
        };

        send_ctx->qp = ibv_create_qp_ex(send_ctx->ctx, &init_attr_ex);

        if(!send_ctx->qp) {
            perror("ibv_create_qp_ex");
            return 1;
        }
        
        // Get extended QP pointer - since we created with ibv_create_qp_ex,
        // the QP has extended features and can be cast to ibv_qp_ex
        // (internally it's a verbs_qp with a union containing both ibv_qp and ibv_qp_ex)
        send_ctx->qpx = (struct ibv_qp_ex *)send_ctx->qp;
        if(!send_ctx->qpx) {
            fprintf(stderr, "Failed to get extended QP\n");
            return 1;
        }
        printf("Created extended QP\n");
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
    // QP number is available directly from the QP structure, no query needed
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
        .gid = *(send_ctx->gid),
        .lid = send_ctx->portinfo.lid,
        .rkey = 0,  // Not needed for send operations
        .remote_addr = 0  // Not needed for sender
    };
    
    struct rdma_conn_info remote_info;
    
    // Exchange connection information
    if(exchange_conn_info_as_sender(tcp_sock, &local_info, &remote_info)) {
        close(tcp_sock);
        return 1;
    }
    
    printf("Received remote info: QPN=%u, PSN=%u, rkey=0x%x, remote_addr=0x%llx\n", 
           remote_info.qpn, remote_info.psn, remote_info.rkey, 
           (unsigned long long)remote_info.remote_addr);
    
    // Store remote connection info
    send_ctx->remote_qpn = remote_info.qpn;
    send_ctx->rq_psn = remote_info.psn;
    send_ctx->remote_rkey = remote_info.rkey;
    send_ctx->remote_addr = remote_info.remote_addr;
    
    // Validate remote info
    if(remote_info.rkey == 0 || remote_info.remote_addr == 0) {
        fprintf(stderr, "ERROR: Invalid remote info - rkey=0x%x, remote_addr=0x%llx\n",
                remote_info.rkey, (unsigned long long)remote_info.remote_addr);
        close(tcp_sock);
        return 1;
    }
    
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
        close(tcp_sock);
        return 1;
    }
    
    // Step 4: Transition QP to RTR (Ready to Receive) - required for connection establishment
    if(modify_qp_to_rtr(send_ctx, &ah_attr)) {
        close(tcp_sock);
        return 1;
    }
    
    // Step 5: Transition QP to RTS (Ready to Send) - required for sending data
    if(modify_qp_to_rts(send_ctx)) {
        close(tcp_sock);
        return 1;
    }
    
    // Wait for receiver to signal it's ready (receive posted)
    // This ensures receiver has posted receive before we send
    char ready_msg;
    if(recv(tcp_sock, &ready_msg, 1, MSG_WAITALL) != 1) {
        perror("recv ready signal");
        close(tcp_sock);
        return 1;
    }
    if(ready_msg != 'R') {
        fprintf(stderr, "ERROR: Invalid ready signal: %c\n", ready_msg);
        close(tcp_sock);
        return 1;
    }
    printf("Received ready signal from receiver\n");
    
    close(tcp_sock);
    
    // Extended QP is already available (created as extended from start)
    
    // Step 6: Prepare data to send
    strcpy(send_ctx->buf, "Hello, RDMA!");
    size_t send_len = strlen(send_ctx->buf) + 1;
    
    // Example using RDMA write with immediate (requires qpx)
    if(send_ctx->qpx) {
        ibv_wr_start(send_ctx->qpx);
        
        send_ctx->qpx->wr_id = 1;
        send_ctx->qpx->wr_flags = IBV_SEND_SIGNALED;
        
        // Set RDMA write with immediate operation
        // Parameters: qp, rkey, remote_addr, imm_data (all in network byte order)
        ibv_wr_rdma_write_imm(send_ctx->qpx, send_ctx->remote_rkey, 
                              send_ctx->remote_addr,  // Remote address from connection exchange
                              htonl(0x12345678));      // Immediate data (32-bit, network byte order)
        
        // Set scatter-gather entry (local buffer to write)
        ibv_wr_set_sge(send_ctx->qpx, send_ctx->mr->lkey, 
                      (uintptr_t)send_ctx->buf, send_len);
        
        if(ibv_wr_complete(send_ctx->qpx)) {
            perror("ibv_wr_complete");
            return 1;
        }
        printf("Posted RDMA write with immediate work request (remote_addr=0x%llx, rkey=0x%x, len=%zu)\n",
               (unsigned long long)send_ctx->remote_addr, send_ctx->remote_rkey, send_len);
    } else {
        fprintf(stderr, "Extended QP not available\n");
        return 1;
    }
    
    // Step 8: Poll for send completion
    struct ibv_wc wc;
    int num_completions = 0;
    int poll_count = 0;
    printf("Polling for send completion...\n");
    do {
        num_completions = ibv_poll_cq(send_ctx->cq, 1, &wc);
        poll_count++;
        if(poll_count % 1000000 == 0) {
            printf("Still polling for send completion... (count: %d)\n", poll_count);
        }
    } while(num_completions == 0);
    
    if(wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Work completion error: %s\n", ibv_wc_status_str(wc.status));
        return 1;
    }
    
    printf("Send completed successfully! (wr_id: %lu)\n", wc.wr_id);

    return 0;
}        
        
