/**
 * @file sdr.h
 * @brief Defines SDR context, qp, and operations.
 */

#ifndef _SDR_H_
#define _SDR_H_

#include <stddef.h>
#include <stdint.h>

struct sdr_context;
struct sdr_qp;
struct sdr_send_handle;
struct sdr_recv_handle;

/**
 * @enum sdr_errcode
 * @brief Enumeration for SDR function error codes.
 */
enum sdr_errcode
{
    SDR_SUCCESS = 0, /**< function completed successfully */
    SDR_ERROR = 1,   /**< function completed with error */
    SDR_RETRY = 2    /**< function couldn't be completed successfully due to lack of the resources at
                          the moment */
};

/**
 * @enum sdr_mtu
 * @brief Enumeration for SDR MTU sizes.
 */
enum sdr_mtu
{
    SDR_MTU_64,   /**< MTU size 64 bytes */
    SDR_MTU_128,  /**< MTU size 128 bytes */
    SDR_MTU_256,  /**< MTU size 256 bytes */
    SDR_MTU_512,  /**< MTU size 512 bytes */
    SDR_MTU_1024, /**< MTU size 1024 bytes */
    SDR_MTU_2048, /**< MTU size 2048 bytes */
    SDR_MTU_4096  /**< MTU size 4096 bytes */
};

/**
 * @struct sdr_dev_attr
 * @brief SDR device attributes.
 */
struct sdr_dev_attr
{
    uint32_t num_channels; /**< Number of channels (1 channel per DPA thread) */
    uint32_t log_max_qps;  /**< Max number of QPs for this context (log2) */
};

/**
 * @enum sdr_wr_opcode
 * @brief Enumeration for SDR work request opcodes.
 */
enum sdr_wr_opcode
{
    SDR_WR_WRITE,         /**< Write operation */
    SDR_WR_WRITE_WITH_IMM /**< Write with immediate operation */
};

/**
 * @struct sdr_qp_attr
 * @brief SDR queue pair attributes.
 */
struct sdr_qp_attr
{
    uint32_t bitmap_chunk_size_log_bytes; /**< Bitmap chunk log size in bytes */
    size_t max_in_flight_log_bytes;       /**< Max number of bytes in flight (log2) */
    uint32_t max_log_num_msgs;            /**< Maximum number of messages in flight (log2) */
    enum sdr_mtu mtu;                     /**< MTU size */
    uint8_t send_enable;                  /**< Send enable flag */
    uint8_t recv_enable;                  /**< Receive enable flag */
    uint32_t rate_limit;                  /**< Rate limit */
};

/**
 * @struct sdr_send_wr
 * @brief SDR send work request.
 */
struct sdr_send_wr
{
    enum sdr_wr_opcode opcode; /**< Work request opcode */
    uint32_t imm_value;        /**< Immediate value (valid only if opcode=SDR_WR_WRITE_WITH_IMM) */
    uint32_t length;           /**< Length of data */
    uint32_t lkey;             /**< Local key */
    uint64_t local_addr;       /**< Local address */
    uint64_t remote_offset;    /**< Remote offset in bitmap chunks in the receive buffer */
};

/**
 * @struct sdr_send_start_wr
 * @brief SDR send_start work request.
 */
struct sdr_send_start_wr
{
    enum sdr_wr_opcode opcode; /**< Work request opcode */
    uint32_t imm_value;        /**< Immediate value (valid only if opcode=SDR_WR_WRITE_WITH_IMM) */
};

/**
 * @struct sdr_send_continue_wr
 * @brief SDR send_continue work request.
 */
struct sdr_send_continue_wr
{
    uint32_t length; /**< Length of data */

    uint64_t local_addr; /**< Local address */
    uint32_t lkey;       /**< Local key */
    uint64_t remote_offset; /**< Remote offset in bitmap chunks in the receive buffer */
};

/**
 * @struct sdr_recv_wr
 * @brief SDR recv work request.
 */
struct sdr_recv_wr
{
    uint32_t max_length; /**< Maximum length of data to receive */
    uint32_t lkey;       /**< Local key associated with this receive's buffer */
    uint64_t address;    /**< Receive start address */
};

/**
 * @struct sdr_send_status
 * @brief Send completion status information. Currently not used.
 */
struct sdr_send_status
{
    uint32_t flags;
};

/**
 * @brief Open an SDR device.
 * This function creates DPA resources that are shared between QPs on the same device.
 * @param dev_name Device name
 * @param dev_attr Pointer to the device attributes.
 * @return Pointer to a valid SDR context on success, NULL on failure.
 */
struct sdr_context *sdr_context_create(const char *dev_name, struct sdr_dev_attr *dev_attr);

/**
 * @brief Close an SDR device.
 * @param context Pointer to the SDR context.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_context_destroy(struct sdr_context *context);

/**
 * @brief Create an SDR QP.
 * @param ctx Pointer to the SDR context.
 * @param qp_attr Pointer to the QP attributes.
 * @return Pointer to a valid SDR QP, NULL on failure.
 */
struct sdr_qp *sdr_qp_create(struct sdr_context *ctx, struct sdr_qp_attr *qp_attr);

/**
 * @brief Destroy an SDR QP.
 * @param qp Pointer to the SDR QP.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_qp_destroy(struct sdr_qp *qp);

/**
 * @brief Get information about QP info size.
 * @param info_size Pointer to the size of the QP information buffer.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_qp_info_size_get(struct sdr_qp *qp, size_t *info_size);

/**
 * @brief Get information about QP.
 * This information must be exchanged out-of-band and used for the sdr_qp_connect call.
 * @param qp Pointer to the SDR QP.
 * @param info The pointer to store the QP information buffer.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_qp_info_get(struct sdr_qp *qp, void *info);

/**
 * @brief Connect the QPs.
 * @param qp Pointer to the SDR QP.
 * @param remote_qp_info Pointer to the remote QP information.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_qp_connect(struct sdr_qp *qp, void *remote_qp_info);

/**
 * @brief Post a receive descriptor.
 * @param qp Pointer to the SDR QP.
 * @param wr Pointer to the receive work request.
 * @param recv_handle Pointer to the receive request handle.
 * @return SDR_SUCCESS on success, SDR_RETRY in case the runtime lacks enough resources to post
 * receive at the moment, SDR_ERROR on failure.
 */
int sdr_recv_post(struct sdr_qp *qp, struct sdr_recv_wr *wr,
                  struct sdr_recv_handle **handle);

/**
 * @brief Get immediate value.
 * Can be called only if the expected operation is a WRITE_WITH_IMM.
 * @param recv_req Pointer to the receive request handle.
 * @param immediate Pointer to the immediate value.
 * @return SDR_SUCCESS on success; SDR_RETRY if the metadata are not yet received; SDR_ERROR
 * on failure.
 */
int sdr_recv_imm_get(struct sdr_recv_handle *handle, uint32_t *immediate);

/**
 * @brief Get receive data bitmap.
 * The bitmap can be used to check completion of single chunks. The chunk size is defined at
 * QP creation time (chunk_log_bsize).
 * @param recv_req Pointer to the receive request handle.
 * @param bitmap_bytes Pointer to the bitmap bytes.
 * @param bitmap_num_bytes Pointer to the number of bitmap bytes.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_recv_bitmap_get(struct sdr_recv_handle *handle, uint8_t **bitmap_bytes,
                        size_t *bitmap_num_bytes);

/**
 * @brief Retrieves chunk completion status at MTU-granularity. Note that this operation queries state on the device,
 * hence it can be expected to be costly (at least one PCIe roundtrip).
 * @param recv_req Pointer to the receive request handle.
 * @param bitmap Bitmap array where the result should be stored. The application must size the bitmap array
 * according with the chunk and MTU sizes (i.e., bits(bitmap) = chunk_size/MTU)
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_recv_bitmap_chunk_expand(struct sdr_recv_handle *handle, uint32_t chunk_offset, uint64_t *bitmap);

/**
 * @brief Complete a receive operation.
 * It must be called for every posted receive. After this call, the bitmap associated with this recv
 * is reset.
 * @param recv_req Pointer to the receive request handle.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_recv_complete(struct sdr_recv_handle *handle);

/**
 * @brief Post a send descriptor.
 * @param qp Pointer to the SDR QP.
 * @param wr Pointer to the send work request.
 * @return SDR_SUCCESS on success, SDR_RETRY in case the runtime lacks enough resources to post
 * send at the moment, SDR_ERROR on failure.
 */
int sdr_send_post(struct sdr_qp *qp, struct sdr_send_wr *wr,
                  struct sdr_send_handle **handle);

/**
 * @brief Starts a streaming send operation. A streaming send can be used to overlap the computation of parts of a message
 * with the transmission of already computed parts.
 * @param qp Pointer to the SDR QP.
 * @param wr Pointer to the send_start work request.
 * @return SDR_SUCCESS on success, SDR_RETRY in case the runtime lacks enough resources to post send at the moment,
 * SDR_ERROR on failure.
 */
int sdr_streaming_send_start(struct sdr_qp *qp, struct sdr_send_start_wr *wr, struct sdr_send_handle **handle);

/**
 * @brief Continues a streaming send operation.
 * @param qp Pointer to the SDR QP.
 * @param wr Pointer to the send_continue work request.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_streaming_send_continue(struct sdr_send_handle *handle, struct sdr_send_continue_wr *wr);

/**
 * @brief Completes a streaming send operation.
 * @param qp Pointer to the SDR QP.
 * @param wr Pointer to the send_continue work request.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_streaming_send_end(struct sdr_send_handle *handle);

/**
 * @brief Polls for send completion.
 * @param handle Pointer to the send request.
 * @return SDR_SUCCESS on success; SDR_RETRY if the send is not yet complete; SDR_ERROR on
 * failure.
 */
int sdr_send_poll(struct sdr_send_handle *handle, int *completed_flag, struct sdr_send_status *status);

#endif // _SDR_H_