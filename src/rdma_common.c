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

