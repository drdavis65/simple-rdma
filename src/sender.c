#include <stdio.h>
#include <infiniband/verbs.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

int main() {
    
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

    struct ibv_context* ctx = ibv_open_device(dev_list[0]);

    if(!ctx) {
        perror("ibv_open_device");
    }

    struct ibv_device_attr *dev_attr = malloc(sizeof(*dev_attr));
    memset(dev_attr, 0, sizeof(*dev_attr));

    if(ibv_query_device(ctx, dev_attr)) {
        perror("ibv_query_device");
    }

    printf("For device: %s\n    Max mr size: %" PRIu64 
           "\n    Max qp: %" PRIu32 
           "\n    Max qp wr: %d\n",
           dev_name, 
           dev_attr->max_mr_size,
           dev_attr->max_qp,
           dev_attr->max_qp_wr);

    struct ibv_port_attr *port_attr = malloc(sizeof(*port_attr));
    memset(port_attr, 0 sizeof(*port_attr));

    if(ibv_query_port(ctx, 1, port_attr)) {
        perror("ibv_query_port");
    }




    return 0;
}        
        
