#include <assert.h>
#include <contrib/lwlog.h>
#include <getopt.h>
#include <math.h>
#include <omp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sdr.h>
#include <sdr_mr.h>

#include "./common/oob_ibv.h"
#include "./common/oob_sock.h"

#define L2V(l) (1UL << (l))
#define V2L(dec) (int) ceil(log2(dec))
#define DEFAULT_CHANNELS 8
#define DEFAULT_WARMUP 10
#define DEFAULT_ITERS 1000
#define DEFAULT_MSG_SIZE 8388608
#define DEFAULT_MAX_IN_FLIGHT 128
#define DEFAULT_MTU_SIZE 4096
#define DEFAULT_BITMAP_CHUNK_SIZE 65536
#define DEFAULT_TX_THREADS_NUM 1
#define DEFAULT_TX_SWR_BATCH_SIZE 8
#define DEFAULT_QP_NUM_GENERATIONS 1
#define DEFAULT_ROOT_MKEY_ENTRIES_FACTOR 1
#define DEFAULT_NUM_ROOT_MKEYS 1
#define UC_OPT_STR "uc"
#define DPA_PROF_OPT_STR "dpa_profiling"
#define TX_THREADS_OPT_STR "tx_threads"
#define TX_OFFLOADING_OPT_STR "tx_offloading"
#define TX_SWR_BATCH_OPT_STR "tx_swr_batch"
#define QP_NUM_GENERATIONS_OPT_STR "qp_num_generations"
#define QP_ROOT_MKEY_ENTRIES_FACTOR_OPT_STR "qp_root_mkey_entries_factor"
#define QP_NUM_ROOT_MKEYS_OPT_STR "qp_num_root_mkeys"
#define MAX_ENVVAR_LEN 128
#define MAX_TX_THREADS_ARG 128
#define MAX_TX_SWR_BATCH_SIZE_ARG 4096

#define APP_ASSERT(EXPR)                                                                           \
    {                                                                                              \
        if (!(EXPR)) {                                                                             \
            lwlog_crit("APP ASSERTION FAILED: %s", #EXPR);                                         \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    }

#define APP_FATAL(FORMAT, ...)                                                                     \
    {                                                                                              \
        lwlog_crit(FORMAT, ##__VA_ARGS__);                                                         \
        exit(EXIT_FAILURE);                                                                        \
    }

#define APP_DEBUG(FORMAT, ...)                                                                     \
    { lwlog_info(FORMAT, ##__VA_ARGS__); }

#define APP_LOG(FORMAT, ...)                                                                       \
    { lwlog_print(FORMAT, ##__VA_ARGS__); }

#define MIN(a, b)                                                                                  \
    ({                                                                                             \
        __typeof__(a) _a = (a);                                                                    \
        __typeof__(b) _b = (b);                                                                    \
        _a < _b ? _a : _b;                                                                         \
    })

#define TIMESTAMP_GET(tp)                                                                          \
    ({                                                                                             \
        if (clock_gettime(CLOCK_MONOTONIC, tp)) {                                                  \
            APP_FATAL("Failed to get clock timestamp");                                            \
        }                                                                                          \
    })

#define TIMESTAMP_DIFF_GET(tp_start, tp_end)                                                       \
    ((((double) tp_end.tv_sec * 1e9 + (double) tp_end.tv_nsec) -                                   \
      ((double) tp_start.tv_sec * 1e9 + (double) tp_start.tv_nsec)) /                              \
     1e9)

struct app_ctx {
    struct opts {
        char *server_addr;
        char *sdr_dev_name;
        size_t warmup;
        size_t iters;
        size_t total_iters;
        size_t msg_size;
        size_t max_in_flight;
        size_t num_channels;
        size_t mtu_size;
        size_t bitmap_chunk_size;
        size_t logging_frequency;
        int validation;
        int uc;
        int dpa_profiling;
        int tx_offloading;
        size_t tx_threads;
        size_t tx_swr_batch_size;
        size_t qp_num_generations;
        size_t qp_root_mkey_entries_factor;
        size_t qp_num_root_mkeys;
    } opts;
    struct sdr_resources {
        struct sdr_context *ctx;
        struct sdr_qp *qp;
        struct sdr_mr *mr;
    } sdr;
    struct oob_flow_control {
        struct oob_ibv_ctx ctx;
        struct oob_ibv_cq cq;
        struct oob_ibv_qp qp;
        size_t sq_capacity;
    } fc;
    struct data {
        struct sdr_recv_handle **recv_handles;
        struct sdr_send_handle **send_handles;
        struct sdr_send_wr *send_wrs;
        struct sdr_recv_wr *recv_wrs;
        size_t nwrs;
        void *local_mem_addr;
        size_t local_mem_length;
        uint32_t imm_data;
    } data;
    struct measurements {
        struct timespec *start_t;
        struct timespec *post_t;
        struct timespec *end_t;
        size_t dropped_chunks;
    } measurements;
    struct oob_sock_ctx *oob_sock_ctx;
    enum sdr_wr_opcode wr_type;
};

void app_env_set(struct app_ctx *app) {
    // RX side always needs one thread, TX side can overwrite this
    omp_set_num_threads(1);

    char size_t_envvar_str[MAX_ENVVAR_LEN];

    if (snprintf(size_t_envvar_str, sizeof(size_t_envvar_str), "%zu",
                 app->opts.qp_num_generations) < 0) {
        APP_FATAL("Failed to convert opts.qp_num_generations to string");
    }
    if (setenv("SDR_QP_NUM_GENERATIONS", size_t_envvar_str, 1)) {
        APP_FATAL("Failed to set SDR_QP_NUM_GENERATIONS envvar");
    }
    APP_LOG("SDR QP num generations: %zu", app->opts.qp_num_generations);
    
    if (app->opts.server_addr) { // TX side runtime configuration
        if (snprintf(size_t_envvar_str, sizeof(size_t_envvar_str), "%zu",
                     app->opts.tx_swr_batch_size) < 0) {
            APP_FATAL("Failed to convert opts.tx_threads to string");
        }
        if (setenv("SDR_SEND_CTX_SWR_BATCH_SIZE", size_t_envvar_str, 1)) {
            APP_FATAL("Failed to set SDR_DPA_TX_PROFILE_STATS_REPORT envvar");
        }
        if (app->opts.tx_offloading) {
            if (setenv("SDR_DPA_TX_OFFLOADING_ENABLE", "1", 1)) {
                APP_FATAL("Failed to set SDR_DPA_TX_OFFLOADING_ENABLE envvar");
            }
            char tx_threads_str[MAX_ENVVAR_LEN];
            if (snprintf(tx_threads_str, sizeof(tx_threads_str), "%zu", app->opts.tx_threads) < 0) {
                APP_FATAL("Failed to convert opts.tx_threads to string");
            }
            if (setenv("SDR_DPA_TX_NUM_WORKERS", tx_threads_str, 1)) {
                APP_FATAL("Failed to set SDR_DPA_TX_NUM_WORKERS envvar");
            }
            APP_LOG("TX offloading enabled");
            if (app->opts.dpa_profiling) {
                if (setenv("SDR_DPA_TX_PROFILE_STATS_REPORT", "1", 1)) {
                    APP_FATAL("Failed to set SDR_DPA_TX_PROFILE_STATS_REPORT envvar");
                }
                APP_LOG("TX DPA profiling enabled");
            }
        } else {
            omp_set_num_threads(app->opts.tx_threads);
            APP_LOG("TX offloading disabled");
        }
        APP_LOG("Number of TX threads: %zu", app->opts.tx_threads);
        APP_LOG("TX send WQe batch size: %zu", app->opts.tx_swr_batch_size);
    } else { // RX side runtime configuration
        if (snprintf(size_t_envvar_str, sizeof(size_t_envvar_str), "%zu",
                     app->opts.qp_root_mkey_entries_factor) < 0) {
            APP_FATAL("Failed to convert opts.qp_root_mkey_entries_factor to string");
        }
        if (setenv("SDR_RECV_CTX_ROOT_MKEY_ENTRIES_FACTOR", size_t_envvar_str, 1)) {
            APP_FATAL("Failed to set SDR_RECV_CTX_ROOT_MKEY_ENTRIES_FACTOR envvar");
        }
        if (snprintf(size_t_envvar_str, sizeof(size_t_envvar_str), "%zu",
                     app->opts.qp_num_root_mkeys) < 0) {
            APP_FATAL("Failed to convert opts.qp_num_root_mkeys to string");
        }
        if (setenv("SDR_RECV_CTX_N_ROOT_MKEYS", size_t_envvar_str, 1)) {
            APP_FATAL("Failed to set SDR_RECV_CTX_N_ROOT_MKEYS envvar");
        }
        APP_LOG("Number of receive QP root memory key entries factor: %zu",
                app->opts.qp_root_mkey_entries_factor);
        APP_LOG("Number of receive QP root memory keys: %zu", app->opts.qp_num_root_mkeys);
        if (app->opts.dpa_profiling) {
            if (setenv("SDR_DPA_RX_PROFILE_STATS_REPORT", "1", 1)) {
                APP_FATAL("Failed to set SDR_DPA_RX_PROFILE_STATS_REPORT envvar");
            }
            APP_LOG("RX DPA profiling enabled");
        }
    }
    if (app->opts.uc) {
        if (setenv("SDR_USE_UC_QP", "1", 1)) {
            APP_FATAL("Failed to set SDR_USE_UC_QP envvar");
        }
        if (setenv("SDR_NO_RC_RNR_RETRY", "1", 1)) {
            APP_FATAL("Failed to set SDR_NO_RC_RNR_RETRY envvar");
        }
        APP_LOG("SDR packet-level transport: UC");
    } else {
        APP_LOG("SDR packet-level transport: RC");
    }
}

int app_opts_parse(struct app_ctx *app, int argc, char **argv) {
    app->wr_type = SDR_WR_WRITE_WITH_IMM;
    app->opts.warmup = DEFAULT_WARMUP;
    app->opts.iters = DEFAULT_ITERS;
    app->opts.msg_size = DEFAULT_MSG_SIZE;
    app->opts.max_in_flight = DEFAULT_MAX_IN_FLIGHT;
    app->opts.num_channels = DEFAULT_CHANNELS;
    app->opts.mtu_size = DEFAULT_MTU_SIZE;
    app->opts.bitmap_chunk_size = DEFAULT_BITMAP_CHUNK_SIZE;
    app->opts.tx_threads = DEFAULT_TX_THREADS_NUM;
    app->opts.tx_swr_batch_size = DEFAULT_TX_SWR_BATCH_SIZE;
    app->opts.qp_num_generations = DEFAULT_QP_NUM_GENERATIONS;
    app->opts.qp_root_mkey_entries_factor = DEFAULT_ROOT_MKEY_ENTRIES_FACTOR;
    app->opts.qp_num_root_mkeys = DEFAULT_NUM_ROOT_MKEYS;
    app->opts.validation = 0;
    app->opts.logging_frequency = 0;

    int c;
    while (1) {
        int option_index = 0;
        // long options are used to set SDR runtime environment variables
        static struct option long_options[] = {
            {UC_OPT_STR, no_argument, 0, 0},
            {DPA_PROF_OPT_STR, no_argument, 0, 0},
            {TX_OFFLOADING_OPT_STR, no_argument, 0, 0},
            {TX_THREADS_OPT_STR, required_argument, 0, 0},
            {TX_SWR_BATCH_OPT_STR, required_argument, 0, 0},
            {QP_NUM_GENERATIONS_OPT_STR, required_argument, 0, 0},
            {QP_ROOT_MKEY_ENTRIES_FACTOR_OPT_STR, required_argument, 0, 0},
            {QP_NUM_ROOT_MKEYS_OPT_STR, required_argument, 0, 0}};

        c = getopt_long(argc, argv, "hs:d:w:i:m:f:c:p:b:vl:", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 0:
            if (!strcmp(long_options[option_index].name, UC_OPT_STR)) {
                app->opts.uc = 1;
            } else if (!strcmp(long_options[option_index].name, DPA_PROF_OPT_STR)) {
                app->opts.dpa_profiling = 1;
            } else if (!strcmp(long_options[option_index].name, TX_THREADS_OPT_STR)) {
                if (sscanf(optarg, "%zu", &app->opts.tx_threads) != 1) {
                    APP_FATAL("Failed to parse iterations");
                }
                if (!app->opts.tx_threads) {
                    APP_FATAL("Number of TX threads should be positive number");
                }
                if (app->opts.tx_threads > MAX_TX_THREADS_ARG) {
                    APP_FATAL("Number of TX threads should be less than %d", MAX_TX_THREADS_ARG);
                }
            } else if (!strcmp(long_options[option_index].name, TX_SWR_BATCH_OPT_STR)) {
                if (sscanf(optarg, "%zu", &app->opts.tx_swr_batch_size) != 1) {
                    APP_FATAL("Failed to parse iterations");
                }
                if (!app->opts.tx_swr_batch_size) {
                    APP_FATAL("Number of TX threads should be positive number");
                }
                if (app->opts.tx_swr_batch_size > MAX_TX_SWR_BATCH_SIZE_ARG) {
                    APP_FATAL("Number of TX threads should be less than %d",
                              MAX_TX_SWR_BATCH_SIZE_ARG);
                }
            } else if (!strcmp(long_options[option_index].name, TX_OFFLOADING_OPT_STR)) {
                app->opts.tx_offloading = 1;
            } else if (!strcmp(long_options[option_index].name, QP_NUM_GENERATIONS_OPT_STR)) {
                if (sscanf(optarg, "%zu", &app->opts.qp_num_generations) != 1) {
                    APP_FATAL("Failed to parse qp num generations");
                }
            } else if (!strcmp(long_options[option_index].name,
                               QP_ROOT_MKEY_ENTRIES_FACTOR_OPT_STR)) {
                if (sscanf(optarg, "%zu", &app->opts.qp_root_mkey_entries_factor) != 1) {
                    APP_FATAL("Failed to parse qp num generations");
                }
            } else if (!strcmp(long_options[option_index].name, QP_NUM_ROOT_MKEYS_OPT_STR)) {
                if (sscanf(optarg, "%zu", &app->opts.qp_num_root_mkeys) != 1) {
                    APP_FATAL("Failed to parse qp num generations");
                }
            } else {
                APP_FATAL("Unknown long option");
            }
            break;
        case 's':
            app->opts.server_addr = optarg;
            break;
        case 'd':
            app->opts.sdr_dev_name = optarg;
            break;
        case 'w':
            if (sscanf(optarg, "%zu", &app->opts.warmup) != 1) {
                APP_FATAL("Failed to parse warmup iterations");
            }
            break;
        case 'i':
            if (sscanf(optarg, "%zu", &app->opts.iters) != 1) {
                APP_FATAL("Failed to parse iterations");
            }
            break;
        case 'm':
            if (sscanf(optarg, "%zu", &app->opts.msg_size) != 1) {
                APP_FATAL("Failed to parse message size");
            }
            break;
        case 'f':
            if (sscanf(optarg, "%zu", &app->opts.max_in_flight) != 1) {
                APP_FATAL("Failed to parse max_in_flight");
            }
            break;
        case 'c':
            if (sscanf(optarg, "%zu", &app->opts.num_channels) != 1) {
                APP_FATAL("Failed to parse num_channels");
            }
            break;
        case 'p':
            if (sscanf(optarg, "%zu", &app->opts.mtu_size) != 1) {
                APP_FATAL("Failed to parse MTU size");
            }
            break;
        case 'b':
            if (sscanf(optarg, "%zu", &app->opts.bitmap_chunk_size) != 1) {
                APP_FATAL("Failed to parse bitmap chunk size");
            }
            break;
        case 'v':
            APP_LOG("Buffer integrity validation was enabled - performance will be affected");
            app->opts.validation = 1;
            break;
        case 'l':
            APP_LOG("Iteration logging was enabled");
            if (sscanf(optarg, "%zu", &app->opts.logging_frequency) != 1) {
                APP_FATAL("Logging frequency value");
            }
            break;
        case 'h':
        default:
            APP_FATAL("Testbench usage:\n"
                      "./sdr_write_bw [options list]\n"
                      "Benchmark options"
                      "-d <RDMA device name>\n"
                      "-s <server ipv4 addr>\n"
                      "-m <write w immediate message size>\n"
                      "-p <packet size>\n"
                      "-c <bitmap chunk size>\n"
                      "-i <number of iterations>\n"
                      "-w <number of warmup iterations>\n"
                      "-f <maximum number of messages in flight>\n"
                      "-c <number of transport channels>\n"
                      "-v - enable receive buffer content validation\n"
                      "-l <N> - log benchmark progress every N iterations\n"
                      "Other options (set libsdr environment variables):\n"
                      "--dpa_profiling - turns on DPA profiling\n"
                      "--tx_offloading - sets SDR_DPA_TX_OFFLOADING_ENABLE to enable DPA-based libsdr send offloading\n"
                      "--tx_threads=<N> - sets number of the OpenMP/DPA threads for the libsdr send progress engine\n"
                      "--tx_swr_batch=<N> - sets SDR_SEND_CTX_SWR_BATCH_SIZE value\n"
                      "--qp_num_generations=<N> - SDR_QP_NUM_GENERATIONS\n"
                      "--qp_root_mkey_entries_factor=<N> - sets SDR_RECV_CTX_ROOT_MKEY_ENTRIES_FACTOR\n"
                      "--qp_num_root_mkeys=<N> - sets SDR_RECV_CTX_N_ROOT_MKEYS\n");
        }
    }

    if (!app->opts.sdr_dev_name) {
        APP_FATAL("SDR device name doesn't provided");
    }
    if (app->opts.msg_size % sizeof(uint64_t)) {
        APP_FATAL("Message size should be divisible by sizeof(uint64_t)");
    }
    if (app->opts.msg_size % app->opts.bitmap_chunk_size) {
        APP_FATAL("Message size is not divisible by bitmap chunk size");
    }
    if (app->opts.msg_size % app->opts.bitmap_chunk_size % 8) {
        APP_FATAL("Number of chunks in the bitmap should be divisible by 8");
    }
    app->opts.total_iters = app->opts.warmup + app->opts.iters;
    if (!app->opts.total_iters) {
        APP_FATAL("Number of iterations should be positive");
    }

    app->data.nwrs = MIN(app->opts.max_in_flight, app->opts.total_iters);

    return 0;
}

int app_get_sdr_mtu(size_t mtu_size) {
    switch (mtu_size) {
    case (64):
        return SDR_MTU_64;
    case (128):
        return SDR_MTU_128;
    case (256):
        return SDR_MTU_256;
    case (512):
        return SDR_MTU_512;
    case (1024):
        return SDR_MTU_1024;
    case (2048):
        return SDR_MTU_2048;
    case (4096):
        return SDR_MTU_4096;
    default:
        APP_FATAL("Unknown MTU size");
    }
}

void app_fc_resources_init(struct app_ctx *app) {
    if (oob_ibv_ctx_create(app->opts.sdr_dev_name, &app->fc.ctx)) {
        APP_FATAL("Error while creating flow control IBV context");
    }
    struct oob_ibv_cq_attr cq_attr = {0};
    cq_attr.cq_depth = app->opts.max_in_flight;
    if (oob_ibv_cq_create(&app->fc.ctx, &cq_attr, &app->fc.cq)) {
        APP_FATAL("Error while creating flow control CQ");
    }
    struct oob_ibv_qp_attr qp_attr = {0};
    qp_attr.send_cq = &app->fc.cq;
    qp_attr.recv_cq = &app->fc.cq;
    qp_attr.max_send_wr = app->opts.max_in_flight;
    qp_attr.max_recv_wr = app->opts.max_in_flight;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.rnr_retry = 0;
    if (oob_ibv_qp_create(&app->fc.ctx, &qp_attr, &app->fc.qp)) {
        APP_FATAL("Error while creating flow control QP");
    }
    struct oob_qp_remote_info local_info = {0};
    struct oob_qp_remote_info remote_info = {0};
    size_t info_size = sizeof(struct oob_qp_remote_info);
    if (oob_ibv_qp_remote_info_get(&app->fc.ctx, &app->fc.qp, &local_info)) {
        APP_FATAL("Error while getting local flow control QP info");
    }
    if (app->opts.server_addr) {
        oob_sock_send(app->oob_sock_ctx, &local_info, info_size);
        oob_sock_recv(app->oob_sock_ctx, &remote_info, info_size);
    } else {
        oob_sock_recv(app->oob_sock_ctx, &remote_info, info_size);
        oob_sock_send(app->oob_sock_ctx, &local_info, info_size);
    }
    if (oob_ibv_qp_connect(&app->fc.qp, &remote_info)) {
        APP_FATAL("Error while connecting flow control QPs");
    }
    app->fc.sq_capacity = app->opts.max_in_flight;
    APP_DEBUG("Successfully allocated flow control resources");
}

void app_fc_resources_destroy(struct app_ctx *app) {
    oob_ibv_qp_destroy(&app->fc.qp);
    oob_ibv_cq_destroy(&app->fc.cq);
    oob_ibv_ctx_destroy(&app->fc.ctx);
}

void app_sdr_resources_init(struct app_ctx *app) {
    // Open SDR context
    struct sdr_dev_attr dev_attr;
    memset(&dev_attr, 0, sizeof(dev_attr));
    dev_attr.num_channels = app->opts.num_channels;
    app->sdr.ctx = sdr_context_create(app->opts.sdr_dev_name, &dev_attr);
    if (!app->sdr.ctx) {
        APP_FATAL("Error while creating context");
    }
    // Create SDR QP
    struct sdr_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.bitmap_chunk_size_log_bytes = V2L(app->opts.bitmap_chunk_size);
    ;
    qp_attr.max_in_flight_log_bytes = V2L(app->opts.max_in_flight * app->opts.msg_size);
    qp_attr.max_log_num_msgs = V2L(app->opts.max_in_flight);
    qp_attr.mtu = app_get_sdr_mtu(app->opts.mtu_size);
    qp_attr.send_enable = app->opts.server_addr ? 1 : 0;
    qp_attr.recv_enable = !qp_attr.send_enable;
    app->sdr.qp = sdr_qp_create(app->sdr.ctx, &qp_attr);
    if (!app->sdr.qp) {
        APP_FATAL("Error while creating QP");
    }
    // SDR QP connection management
    void *local_qp_info, *remote_qp_info;
    size_t local_qp_info_size, remote_qp_info_size;
    if (sdr_qp_info_size_get(app->sdr.qp, &local_qp_info_size)) {
        APP_FATAL("Error while getting QP size");
    }
    APP_DEBUG("Local QP address size: %zu", local_qp_info_size);

    local_qp_info = malloc(local_qp_info_size);
    if (!local_qp_info) {
        APP_FATAL("Error while allocating memory for local QP info");
    }
    if (sdr_qp_info_get(app->sdr.qp, local_qp_info)) {
        APP_FATAL("Error while getting local QP info");
    }

    if (app->opts.server_addr) {
        oob_sock_send(app->oob_sock_ctx, &local_qp_info_size, sizeof(local_qp_info_size));
        oob_sock_recv(app->oob_sock_ctx, &remote_qp_info_size, sizeof(remote_qp_info_size));
    } else {
        oob_sock_recv(app->oob_sock_ctx, &remote_qp_info_size, sizeof(remote_qp_info_size));
        oob_sock_send(app->oob_sock_ctx, &local_qp_info_size, sizeof(local_qp_info_size));
    }

    APP_DEBUG("Remote QP info size: %zu bytes", remote_qp_info_size);

    remote_qp_info = malloc(remote_qp_info_size);
    if (!remote_qp_info) {
        APP_FATAL("Error while allocating memory for remote QP info");
    }

    if (app->opts.server_addr) {
        oob_sock_send(app->oob_sock_ctx, local_qp_info, local_qp_info_size);
        oob_sock_recv(app->oob_sock_ctx, remote_qp_info, remote_qp_info_size);
    } else {
        oob_sock_recv(app->oob_sock_ctx, remote_qp_info, remote_qp_info_size);
        oob_sock_send(app->oob_sock_ctx, local_qp_info, local_qp_info_size);
    }

    if (sdr_qp_connect(app->sdr.qp, remote_qp_info)) {
        APP_FATAL("Error while connecting QPs");
    }

    APP_LOG("QP connection establishment completed");

    free(local_qp_info);
    free(remote_qp_info);
    APP_DEBUG("Successfully allocated SDR resources");
}

void app_sdr_resources_finalize(struct app_ctx *app) {
    sdr_mr_dereg(app->sdr.mr);
    sdr_qp_destroy(app->sdr.qp);
    sdr_context_destroy(app->sdr.ctx);
}

void app_buffers_alloc(struct app_ctx *app) {
    if (app->opts.server_addr) {
        if (posix_memalign((void **) &app->data.send_handles, sysconf(_SC_PAGESIZE),
                           app->data.nwrs * sizeof(struct sdr_send_handle *))) {
            APP_FATAL("Client failed to allocate send handle pointers array");
        }
        if (posix_memalign((void **) &app->data.send_wrs, sysconf(_SC_PAGESIZE),
                           app->data.nwrs * sizeof(struct sdr_send_wr))) {
            APP_FATAL("Client failed to allocate send WRs array");
        }
    } else {
        if (posix_memalign((void **) &app->data.recv_handles, sysconf(_SC_PAGESIZE),
                           app->data.nwrs * sizeof(struct sdr_recv_handle *))) {
            APP_FATAL("Server failed to allocate receive handle pointers array");
        }
        if (posix_memalign((void **) &app->data.recv_wrs, sysconf(_SC_PAGESIZE),
                           app->data.nwrs * sizeof(struct sdr_recv_wr))) {
            APP_FATAL("Server failed to allocate receive WRs array");
        }
    }
    if (posix_memalign((void **) &app->measurements.start_t, sysconf(_SC_PAGESIZE),
                       app->opts.total_iters * sizeof(struct timespec))) {
        APP_FATAL("Server failed to allocate receive handle pointers array");
    }
    if (posix_memalign((void **) &app->measurements.post_t, sysconf(_SC_PAGESIZE),
                       app->opts.total_iters * sizeof(struct timespec))) {
        APP_FATAL("Server failed to allocate receive handle pointers array");
    }
    if (posix_memalign((void **) &app->measurements.end_t, sysconf(_SC_PAGESIZE),
                       app->opts.total_iters * sizeof(struct timespec))) {
        APP_FATAL("Server failed to allocate receive WRs array");
    }
    app->data.local_mem_length = app->opts.max_in_flight * app->opts.msg_size;
    if (posix_memalign(&app->data.local_mem_addr, sysconf(_SC_PAGESIZE),
                       app->data.local_mem_length)) {
        APP_FATAL("Error while allocating buffer");
    }
    app->sdr.mr = sdr_mr_reg(app->sdr.ctx, app->data.local_mem_addr,
                                 app->data.local_mem_length, SDR_ACCESS_REMOTE_WRITE);
    if (!app->sdr.mr) {
        APP_FATAL("Error while registering memory");
    }
}

void app_buffers_dealloc(struct app_ctx *app) {
    free(app->measurements.start_t);
    free(app->measurements.post_t);
    free(app->measurements.end_t);
    if (app->opts.server_addr) {
        free(app->data.send_handles);
        free(app->data.send_wrs);
    } else {
        free(app->data.recv_handles);
        free(app->data.recv_wrs);
    }
    free(app->data.local_mem_addr);
}

inline uint64_t app_validation_buf_value(size_t iter, size_t idx) {
    return 0xDEADBEAF + iter + idx;
}

void app_validation_send_buf_set(struct app_ctx *app, size_t iter) {
    size_t buf_id = iter % app->data.nwrs;
    size_t buf_elems = app->opts.msg_size / sizeof(uint64_t);
    uint64_t *buf = (uint64_t *) app->data.local_mem_addr + buf_elems * buf_id;
    for (size_t i = 0; i < buf_elems; i++) {
        buf[i] = app_validation_buf_value(iter, i);
    }
}

int app_validation_recv_buf_check(struct app_ctx *app, size_t iter) {
    size_t buf_id = iter % app->data.nwrs;
    size_t buf_elems = app->opts.msg_size / sizeof(uint64_t);
    uint64_t *buf = (uint64_t *) app->data.local_mem_addr + buf_elems * buf_id;
    for (size_t i = 0; i < buf_elems; i++) {
        uint64_t expected = app_validation_buf_value(iter, i);
        if (expected != buf[i]) {
            APP_LOG("Incorrect buffer: iter=%zu buf_id=%zu buf_elems=%zu i=%zu expected=%lu "
                    "received=%lu",
                    iter, buf_id, buf_elems, i, expected, buf[i]);
            return 1;
        }
    }
    return 0;
}

void app_server_cts_send(struct app_ctx *app) {
    uint32_t cts = 0xDEADBEAF;
    oob_sock_send(app->oob_sock_ctx, &cts, sizeof(cts));
}

void app_server_perf_report(struct app_ctx *app) {
    size_t chunks_per_msg = app->opts.msg_size / app->opts.bitmap_chunk_size;
    double avg_drop_rate = (double) app->measurements.dropped_chunks /
                           (((double) app->opts.iters * chunks_per_msg) / 100.);
    APP_LOG("Avg drop rate: %.2f%%", avg_drop_rate);
}

int app_server_recv_is_completed(struct app_ctx *app, uint8_t *bitmap, size_t bitmap_size,
                                 size_t cur_iter) {
    // NOTE: app and cur_iter can be used to implement application-specific behaviour
    (void) app;
    (void) cur_iter;
    for (size_t chunk_id = 0; chunk_id < bitmap_size; chunk_id++) {
        if (!bitmap[chunk_id]) {
            return 0;
        }
    }
    return 1;
}

void app_server(struct app_ctx *app) {
    // Pre-post receives
    size_t num_started = 0;
    size_t in_flight = 0;
    uint32_t buf_lkey;
    APP_ASSERT(!sdr_mr_lkey_get(app->sdr.mr, &buf_lkey));
    for (num_started = 0; num_started < app->data.nwrs; num_started++) {
        app->data.recv_wrs[num_started].max_length = app->opts.msg_size;
        app->data.recv_wrs[num_started].lkey = buf_lkey;
        app->data.recv_wrs[num_started].address =
            (uint64_t) app->data.local_mem_addr + num_started * app->opts.msg_size;
        // We assume that pre-posting always succeeds, e.g., no SDR_RETRY returned
        if (sdr_recv_post(app->sdr.qp, &app->data.recv_wrs[num_started],
                            &app->data.recv_handles[num_started])) {
            APP_FATAL("Server error while pre-posting receive");
        }
        in_flight++;
        APP_ASSERT(app->data.recv_handles[num_started]);
        APP_DEBUG("Server pre-posted receive: idx=%zu, num_started=%zu/%zu, in_flight=%zu",
                  num_started, num_started, app->opts.total_iters, in_flight);
    }

    app_server_cts_send(app);

    // Start poll & re-post in round-robin fashion
    size_t num_completed = 0;
    while (num_completed < app->opts.total_iters) {
        size_t rwr_id = num_completed % app->data.nwrs;
        if (app->wr_type == SDR_WR_WRITE_WITH_IMM) {
            int ret;
            if ((ret = sdr_recv_imm_get(app->data.recv_handles[rwr_id], &app->data.imm_data)) !=
                SDR_RETRY) {
                if (ret == SDR_ERROR) {
                    APP_FATAL("Server error while receiving immediate data");
                }
            } else {
                continue;
            }
            assert(app->data.imm_data == 0xDEADBEAF + (uint64_t) num_completed);
        }
        uint8_t *bitmap;
        size_t bitmap_size = 0;
        if (sdr_recv_bitmap_get(app->data.recv_handles[rwr_id], &bitmap, &bitmap_size)) {
            APP_FATAL("Server error while obtaining receive bitmap");
        }
        if (!app_server_recv_is_completed(app, bitmap, bitmap_size, num_completed)) {
            continue;
        }
        if (sdr_recv_complete(app->data.recv_handles[rwr_id])) {
            APP_FATAL("Server error while completing receive");
        }

        if (app->opts.validation) {
            if (app_validation_recv_buf_check(app, num_completed)) {
                APP_FATAL("Server receive buffer validation failed!");
            }
        }

        num_completed++;
        in_flight--;
        APP_DEBUG("Server completed receive: in_flight=%zu, num_completed=%zu/%zu, rwr_id=%zu",
                  in_flight, num_completed, app->opts.total_iters, rwr_id);

        // keep re-posting
        if (num_started < app->opts.total_iters) {
            assert(in_flight < app->data.nwrs);
        retry:
            int ret = sdr_recv_post(app->sdr.qp, &app->data.recv_wrs[rwr_id],
                                      &app->data.recv_handles[rwr_id]);
            if (ret == SDR_RETRY)
                goto retry;
            else if (ret)
                APP_FATAL("Server error while re-posting receive");

            num_started++;
            in_flight++;
            APP_DEBUG("Server re-posted receive: num_started=%zu/%zu, in_flight=%zu", num_started,
                      app->opts.total_iters, in_flight);
        }

        // send flow control ack
        if (!app->fc.sq_capacity) {
            if (oob_ibv_cq_cqe_batch_wait(&app->fc.cq, app->opts.max_in_flight)) {
                APP_FATAL("Failed to wait flow control ack batch");
            }
            app->fc.sq_capacity += app->opts.max_in_flight;
        } else if (app->fc.sq_capacity != app->opts.max_in_flight) {
            size_t n_poll;
            if (oob_ibv_cq_cqe_batch_poll(&app->fc.cq, app->opts.max_in_flight, &n_poll)) {
                APP_FATAL("Failed to wait flow control ack batch");
            }
            app->fc.sq_capacity += n_poll;
        }
        uint32_t ack = 0xDEADBEAF + num_completed;
        if (oob_ibv_qp_signal_send_post(&app->fc.qp, ack)) {
            APP_FATAL("Failed to post flow control ack send");
        }
        app->fc.sq_capacity--;
    }
    if (app->opts.validation) {
        APP_LOG("Receive buffer validation passed");
    }
}

void app_client_perf_report(struct app_ctx *app) {
    size_t measurements_start_id = app->opts.warmup ? app->opts.warmup : 0;
    // injection bandwidth observed by the sender from posting the first send and until polling last
    // completion
    double total_inj_time_s =
        TIMESTAMP_DIFF_GET(app->measurements.start_t[measurements_start_id],
                           app->measurements.end_t[app->opts.total_iters - 1]);
    double avg_inj_time_s = total_inj_time_s / app->opts.iters;
    double avg_inj_Bps = ((double) app->opts.msg_size * app->opts.iters) / total_inj_time_s;
    double avg_pps =
        ((double) (app->opts.msg_size / app->opts.mtu_size) * app->opts.iters) / total_inj_time_s;
    double avg_post_time_s = 0.0;
    for (size_t iter = measurements_start_id; iter < app->opts.total_iters; iter++) {
        avg_post_time_s +=
            TIMESTAMP_DIFF_GET(app->measurements.start_t[iter], app->measurements.post_t[iter]);
    }
    avg_post_time_s /= app->opts.total_iters;
    APP_LOG("Avg sdr_send_post completion time: %f ms", avg_post_time_s * 1000.0);
    APP_LOG("Avg message completion time: %f ms", avg_inj_time_s * 1000.0);
    APP_LOG("Average bitrate: %.3f Gbit/s", avg_inj_Bps / 1e9 * 8.0);
    APP_LOG("Average packet rate: %.3f Mpps", avg_pps / 1e6);
}

void app_client(struct app_ctx *app) {
    // Pre-post credit flow control receives
    for (size_t wr_id = 0; wr_id < app->data.nwrs; wr_id++) {
        if (oob_ibv_qp_signal_recv_post(&app->fc.qp)) {
            APP_FATAL("Failed to post flow control receive");
        }
    }

    // Receive CTS from server
    uint32_t cts;
    oob_sock_recv(app->oob_sock_ctx, &cts, sizeof(cts));
    APP_ASSERT(cts == 0xDEADBEAF);

    // Prepare sdr wrs
    for (size_t wr_id = 0; wr_id < app->data.nwrs; wr_id++) {
        app->data.send_wrs[wr_id].local_addr =
            (uint64_t) app->data.local_mem_addr + wr_id * app->opts.msg_size;
        app->data.send_wrs[wr_id].remote_offset = 0;
        app->data.send_wrs[wr_id].length = app->opts.msg_size;
        if (sdr_mr_lkey_get(app->sdr.mr, &app->data.send_wrs[wr_id].lkey)) {
            APP_FATAL("Client error while obtaining lkey");
        }
        app->data.send_wrs[wr_id].opcode = app->wr_type;
    }

    // Post sdr sends
    size_t num_started = 0;
    size_t in_flight = 0;
    int ret;
    for (num_started = 0; num_started < app->data.nwrs; num_started++) {
        if (app->opts.validation) {
            app_validation_send_buf_set(app, num_started);
        }
        TIMESTAMP_GET(&app->measurements.start_t[num_started]);
        if (app->data.send_wrs[num_started].opcode == SDR_WR_WRITE_WITH_IMM) {
            app->data.send_wrs[num_started].imm_value = (uint32_t) num_started + 0xDEADBEAF;
        }
        while ((ret = sdr_send_post(app->sdr.qp, &app->data.send_wrs[num_started],
                                      &app->data.send_handles[num_started]))) {
            if (ret == SDR_ERROR) {
                APP_FATAL("Client error while posting send: cur_swr=%zu num_started=%zu",
                          num_started, num_started);
            }
        }
        TIMESTAMP_GET(&app->measurements.post_t[num_started]);
        in_flight++;
    }

    size_t num_completed = 0;
    while (num_completed < app->opts.total_iters) {
        size_t swr_id = num_completed % app->data.nwrs;
        int completed = 0;
        int ret = sdr_send_poll(app->data.send_handles[swr_id], &completed, NULL);
        if (ret == SDR_ERROR) {
            APP_FATAL("Client error while polling send");
        }
        if (!completed)
            continue;

        uint32_t ack;
        if (oob_ibv_cq_signal_wait(&app->fc.cq, &ack)) {
            APP_FATAL("Failed to wait flow control ack receive");
        }
        if (ack == 0xDEADBEAF + num_completed + 1) {
            TIMESTAMP_GET(&app->measurements.end_t[num_completed]);
        } else {
            APP_FATAL("Client got wrong flow control ack");
        }
        if (oob_ibv_qp_signal_recv_post(&app->fc.qp)) {
            APP_FATAL("Failed to post flow control receive");
        }
        if (app->opts.logging_frequency) {
            if (!(num_completed % app->opts.logging_frequency)) {
                APP_LOG("Iteration: %zu/%zu", num_completed, app->opts.total_iters);
            }
        }
        num_completed++;
        in_flight--;
        APP_DEBUG("Client completes send: in_flight=%zu, num_completed=%zu/%zu, swr_id=%zu",
                  in_flight, num_completed, app->opts.total_iters, swr_id);

        // Keep posting
        if (num_started < app->opts.total_iters) {
            assert(in_flight < app->data.nwrs);
            if (app->opts.validation) {
                app_validation_send_buf_set(app, num_started);
            }
            TIMESTAMP_GET(&app->measurements.start_t[num_started]);
            if (app->data.send_wrs[swr_id].opcode == SDR_WR_WRITE_WITH_IMM) {
                app->data.send_wrs[swr_id].imm_value = (uint32_t) num_started + 0xDEADBEAF;
            }
            while ((ret = sdr_send_post(app->sdr.qp, &app->data.send_wrs[swr_id],
                                          &app->data.send_handles[swr_id]))) {
                if (ret == SDR_ERROR) {
                    APP_FATAL(
                        "Client error while re-posting send: num_started=%zu num_completed=%zu",
                        num_started, num_completed);
                }
            }
            TIMESTAMP_GET(&app->measurements.post_t[num_started]);
            num_started++;
            in_flight++;
        }
    }
}

int main(int argc, char **argv) {
    struct app_ctx app;
    memset(&app, 0, sizeof(app));
    if (app_opts_parse(&app, argc, argv) != 0) {
        APP_FATAL("failed to parse command line options");
        exit(1);
    }
    if (app.opts.server_addr) {
        APP_LOG("Server IPv4 address: %s", app.opts.server_addr);
    }
    oob_sock_ctx_create(app.opts.server_addr, OOB_DEFAULT_PORT, &app.oob_sock_ctx);
    app_env_set(&app);
    APP_LOG("SDR device name: %s", app.opts.sdr_dev_name);
    app_fc_resources_init(&app);
    app_sdr_resources_init(&app);
    APP_LOG("Benchmark warmup: %zu", app.opts.warmup);
    APP_LOG("Benchmark iters: %zu", app.opts.iters);
    APP_LOG("Benchmark message size: %zu", app.opts.msg_size);
    APP_LOG("Benchmark MTU size: %zu", app.opts.mtu_size);
    APP_LOG("Benchmark bitmap chunk size: %zu", app.opts.bitmap_chunk_size);
    if (app.opts.server_addr) {
        APP_LOG("Benchmark maximum writes in flight: %zu", app.opts.max_in_flight);
    } else {
        APP_LOG("Benchmark number of prepost receives: %zu", app.data.nwrs);
    }
    APP_LOG("Benchmark QP channels: %zu", app.opts.num_channels);
    app_buffers_alloc(&app);
    if (app.opts.server_addr) {
        APP_LOG("Client started");
        app_client(&app);
        app_client_perf_report(&app);
    } else {
        APP_LOG("Server started");
        app_server(&app);
        app_server_perf_report(&app);
    }
    app_buffers_dealloc(&app);
    app_sdr_resources_finalize(&app);
    app_fc_resources_destroy(&app);
    oob_sock_ctx_destroy(app.oob_sock_ctx);
    return 0;
}
