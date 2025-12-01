/**
 * @file sdr_mr.h
 * @brief Defines SDR memory operations.
 */

#ifndef _SDR_MR_H_
#define _SDR_MR_H_

enum sdr_access_flags { SDR_ACCESS_REMOTE_WRITE = 0 };

/**
 * @brief Opaque structure representing a SDR memory region.
 */
struct sdr_mr;

/**
 * @brief Registers a memory region.
 *
 * @param sdr_ctx SDR context
 * @param addr Starting address of the memory region.
 * @param length Length of the memory region.
 * @param access Access flags for the memory region.
 * @return Pointer to the registered memory region structure on success, NULL on failure.
 */
struct sdr_mr *sdr_mr_reg(struct sdr_context *sdr_ctx, void *addr, size_t length,
                              enum sdr_access_flags access);

/**
 * @brief Deregisters a memory region.
 *
 * @param mr Pointer to the memory region structure to be deregistered.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_mr_dereg(struct sdr_mr *mr);

/**
 * @brief Retrieves the local key (lkey) for a memory region.
 *
 * @param mr Pointer to the memory region structure.
 * @param lkey Pointer to the variable where the lkey will be stored.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_mr_lkey_get(struct sdr_mr *mr, uint32_t *lkey);

/**
 * @brief Retrieves the remote key (rkey) for a memory region.
 *
 * @param mr Pointer to the memory region structure.
 * @param rkey Pointer to the variable where the rkey will be stored.
 * @return SDR_SUCCESS on success, SDR_ERROR on failure.
 */
int sdr_mr_rkey_get(struct sdr_mr *mr, uint32_t *rkey);

#endif // _SDR_MR_H_