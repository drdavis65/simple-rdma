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

struct receiver_context {
	struct ibv_context *ctx;
	struct ibv_comp_channel *channel;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	char *buf;
	int size;
	int num_packets;
	struct ibv_port_attr portinfo;

	// Connection info for QP modification
	uint32_t remote_qpn; // Remote QP number
	uint32_t sq_psn; // Send queue PSN
	uint32_t rq_psn; // Receive queue PSN
	struct ibv_ah *ah; // Address Handle (for routing)
	uint32_t remote_rkey; // Remote memory region key (for RDMA ops)
};

// Transition QP to RTR (Ready to Receive) state
static int modify_qp_to_rtr(struct receiver_context *ctx,
			    struct ibv_ah_attr *ah_attr)
{
	struct ibv_qp_attr attr = { .qp_state = IBV_QPS_RTR,
				    .path_mtu = ctx->portinfo.active_mtu,
				    .dest_qp_num = ctx->remote_qpn,
				    .rq_psn = ctx->rq_psn,
				    .max_dest_rd_atomic = 16,
				    .min_rnr_timer = 0x12,
				    .ah_attr = *ah_attr };

	int rtr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		       IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	if (ibv_modify_qp(ctx->qp, &attr, rtr_mask)) {
		perror("Failed to modify QP to RTR");
		return -1;
	}
	printf("Receiver QP transitioned to RTR\n");
	return 0;
}

// Transition QP to RTS (Ready to Send) state
static int modify_qp_to_rts(struct receiver_context *ctx)
{
	struct ibv_qp_attr attr = { .qp_state = IBV_QPS_RTS,
				    .timeout = 0x12,
				    .retry_cnt = 6,
				    .rnr_retry = 7,
				    .sq_psn = ctx->sq_psn };

	int rts_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
		       IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN;

	if (ibv_modify_qp(ctx->qp, &attr, rts_mask)) {
		perror("Failed to modify QP to RTS");
		return -1;
	}
	printf("Receiver QP transitioned to RTS - ready to receive!\n");
	return 0;
}

int main(int argc, char *argv[])
{
	int tcp_port = RDMA_TCP_PORT;

	// Parse command-line arguments
	if (argc > 1) {
		tcp_port = atoi(argv[1]);
		if (tcp_port <= 0 || tcp_port > 65535) {
			fprintf(stderr, "Invalid port number: %s\n", argv[1]);
			return 1;
		}
	}

	printf("Receiver starting on port %d\n", tcp_port);

	int num_devices = 0;
	struct ibv_device **dev_list = ibv_get_device_list(&num_devices);

	if (!dev_list) {
		perror("ibv_get_device_list");
		return 1;
	}

	printf("Found %d ibv devices\n", num_devices);

	if (num_devices == 0) {
		fprintf(stderr, "No InfiniBand devices found\n");
		return 1;
	}

	for (int i = 0; i < num_devices; i++) {
		struct ibv_device *dev = dev_list[i];
		printf("    %d name: %s\n", i, ibv_get_device_name(dev));
	}

	const char *dev_name = ibv_get_device_name(dev_list[0]);
	struct receiver_context *recv_ctx = malloc(sizeof(*recv_ctx));
	memset(recv_ctx, 0, sizeof(*recv_ctx));

	recv_ctx->ctx = ibv_open_device(dev_list[0]);
	if (!recv_ctx->ctx) {
		perror("ibv_open_device");
		return 1;
	}

	struct ibv_device_attr *dev_attr = malloc(sizeof(*dev_attr));
	memset(dev_attr, 0, sizeof(*dev_attr));

	if (ibv_query_device(recv_ctx->ctx, dev_attr)) {
		perror("ibv_query_device");
		return 1;
	}

	printf("For device: %s\n    Max mr size: %" PRIu64
	       "\n    Max qp: %" PRIu32 "\n    Max qp wr: %d\n",
	       dev_name, dev_attr->max_mr_size, dev_attr->max_qp,
	       dev_attr->max_qp_wr);

	memset(&recv_ctx->portinfo, 0, sizeof(recv_ctx->portinfo));

	if (ibv_query_port(recv_ctx->ctx, 1, &recv_ctx->portinfo)) {
		perror("ibv_query_port");
		return 1;
	}

	printf("Device state: %s\nActive mtu: %s\nSpeed: %s\n",
	       port_state_str(recv_ctx->portinfo.state),
	       mtu_str(recv_ctx->portinfo.active_mtu),
	       speed_str(recv_ctx->portinfo.active_speed));

	union ibv_gid *gid = malloc(sizeof(*gid));
	if (ibv_query_gid(recv_ctx->ctx, 1, 3, gid)) {
		perror("ibv_query_gid");
		return 1;
	}

	struct ibv_gid_entry *entry = malloc(sizeof(*entry));
	if (ibv_query_gid_ex(recv_ctx->ctx, 1, 3, entry, 0)) {
		perror("ibv_query_gid_ex");
		return 1;
	}

	printf("GID type: %s\n", gid_type_str(entry->gid_type));

	recv_ctx->num_packets = 3;
	recv_ctx->size = 3 * 1024 * sizeof(char);

	// Use posix_memalign for page-aligned memory (required for RDMA)
	if (posix_memalign((void **)&recv_ctx->buf, sysconf(_SC_PAGESIZE),
			   recv_ctx->size)) {
		perror("posix_memalign");
		return 1;
	}
	memset(recv_ctx->buf, 0, recv_ctx->size);

	recv_ctx->channel = ibv_create_comp_channel(recv_ctx->ctx);
	if (!recv_ctx->channel) {
		perror("ibv_create_comp_channel");
		return 1;
	}

	recv_ctx->pd = ibv_alloc_pd(recv_ctx->ctx);
	if (!recv_ctx->pd) {
		perror("ibv_alloc_pd");
		return 1;
	}

	recv_ctx->mr =
		ibv_reg_mr(recv_ctx->pd, recv_ctx->buf, recv_ctx->size,
			   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!recv_ctx->mr) {
		perror("ibv_reg_mr");
		return 1;
	}

	recv_ctx->cq = ibv_create_cq(recv_ctx->ctx, recv_ctx->num_packets, NULL,
				     recv_ctx->channel, 0);
	if (!recv_ctx->cq) {
		perror("ibv_create_cq");
		return 1;
	}

	// Create QP
	struct ibv_qp_init_attr init_attr = {
        .send_cq = recv_ctx->cq,
        .recv_cq = recv_ctx->cq,
        .cap     = {
            .max_send_wr = 1,
            .max_recv_wr = 3,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC
    };

	recv_ctx->qp = ibv_create_qp(recv_ctx->pd, &init_attr);
	if (!recv_ctx->qp) {
		perror("ibv_create_qp");
		return 1;
	}

	// Transition QP to INIT
	struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT,
				    .pkey_index = 0,
				    .port_num = 1,
				    .qp_access_flags =
					    IBV_ACCESS_LOCAL_WRITE |
					    IBV_ACCESS_REMOTE_WRITE };

	if (ibv_modify_qp(recv_ctx->qp, &attr,
			  IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
				  IBV_QP_ACCESS_FLAGS)) {
		perror("Failed to modify QP to INIT");
		return 1;
	}

	// Get local QP number and generate PSN
	uint32_t local_qpn = recv_ctx->qp->qp_num;
	printf("Receiver local QP number: %u\n", local_qpn);

	// Generate PSNs (Packet Sequence Numbers)
	// PSN is a 24-bit value used for reliable delivery and ordering.
	// It doesn't need to be random - any value works as long as both sides agree.
	// Using random helps avoid collisions in multi-connection scenarios.
	srand(time(NULL));
	recv_ctx->sq_psn =
		rand() &
		0xFFFFFF; // 24-bit PSN (could also use 0 or any fixed value)
	recv_ctx->rq_psn = 0; // Will be set from remote side

	// Setup TCP server for connection establishment
	int server_sock = setup_tcp_server(tcp_port);
	if (server_sock < 0) {
		return 1;
	}

	printf("Waiting for sender to connect...\n");
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	int client_sock = accept(server_sock, (struct sockaddr *)&client_addr,
				 &client_len);
	if (client_sock < 0) {
		perror("accept");
		close(server_sock);
		return 1;
	}
	printf("Sender connected from %s:%d\n", inet_ntoa(client_addr.sin_addr),
	       ntohs(client_addr.sin_port));

	// Prepare local connection info
	struct rdma_conn_info local_info = {
		.qpn = local_qpn,
		.psn = recv_ctx->sq_psn,
		.gid = *gid,
		.lid = recv_ctx->portinfo.lid,
		.rkey = recv_ctx->mr->rkey,
		.remote_addr = (uint64_t)(uintptr_t)recv_ctx
				       ->buf // Address of receive buffer
	};

	struct rdma_conn_info remote_info;

	// Exchange connection information
	if (exchange_conn_info_as_receiver(client_sock, &local_info,
					   &remote_info)) {
		close(client_sock);
		close(server_sock);
		return 1;
	}

	printf("Received remote info: QPN=%u, PSN=%u\n", remote_info.qpn,
	       remote_info.psn);

	// Store remote connection info
	recv_ctx->remote_qpn = remote_info.qpn;
	recv_ctx->rq_psn = remote_info.psn;

	// Create Address Handle
	struct ibv_ah_attr ah_attr = { .is_global = 1,
				       .port_num = 1,
				       .grh = { .dgid = remote_info.gid,
						.flow_label = 0,
						.sgid_index = 3,
						.hop_limit = 255,
						.traffic_class = 0 } };

	recv_ctx->ah = ibv_create_ah(recv_ctx->pd, &ah_attr);
	if (!recv_ctx->ah) {
		perror("ibv_create_ah");
		close(client_sock);
		close(server_sock);
		return 1;
	}

	// Transition QP to RTR (Ready to Receive) - sufficient for receiving data
	// Receiver only needs RTR since it's only receiving, not sending
	if (modify_qp_to_rtr(recv_ctx, &ah_attr)) {
		close(client_sock);
		close(server_sock);
		return 1;
	}

	close(client_sock);
	close(server_sock);
	printf("Receiver ready! Waiting for data...\n");

	// Post receive buffer
	struct ibv_sge sge = { .addr = (uintptr_t)recv_ctx->buf,
			       .length = recv_ctx->size,
			       .lkey = recv_ctx->mr->lkey };

	struct ibv_recv_wr wr = { .wr_id = 1, .sg_list = &sge, .num_sge = 1 };

	struct ibv_recv_wr *bad_wr;
	if (ibv_post_recv(recv_ctx->qp, &wr, &bad_wr)) {
		perror("ibv_post_recv");
		return 1;
	}
	printf("Posted receive work request\n");

	// Poll for completion
	struct ibv_wc wc;
	int num_completions = 0;
	do {
		num_completions = ibv_poll_cq(recv_ctx->cq, 1, &wc);
	} while (num_completions == 0);

	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Work completion error: %s\n",
			ibv_wc_status_str(wc.status));
		return 1;
	}
	if (wc.wc_flags & IBV_WC_WITH_IMM) {
		printf("Received immediate data: %x\n", ntohl(wc.imm_data));
	}

	printf("Received data: %s\n", recv_ctx->buf);
	printf("Receive completed successfully! (wr_id: %lu, byte_len: %u)\n",
	       wc.wr_id, wc.byte_len);

	return 0;
}