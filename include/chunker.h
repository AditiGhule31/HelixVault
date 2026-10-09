/**
 * chunker.h — Content-Defined Chunking via Rabin Fingerprinting
 *
 * Implements a sliding-window Rabin fingerprint chunker that splits a byte
 * stream at content-defined boundaries, producing variable-length chunks
 * whose sizes are bounded by configurable min/max limits.
 *
 * Algorithm overview:
 *  1. Maintain a fixed-width sliding window over the input.
 *  2. Compute a rolling polynomial hash (Rabin fingerprint) as the window
 *     slides one byte at a time.
 *  3. Declare a chunk boundary when (hash & CHUNK_MASK) == CHUNK_MAGIC,
 *     tuned so that the expected chunk size equals the target chunk size.
 *  4. Enforce hard minimum and maximum size limits to prevent pathological
 *     chunk distributions.
 *
 * The polynomial is evaluated over GF(2^64) using a pre-computed table
 * for the irreducible polynomial x^64 + x^4 + x^3 + x + 1 (common choice).
 */

#ifndef HELIX_CHUNKER_H
#define HELIX_CHUNKER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Default CDC Parameters
 * ---------------------------------------------------------------------- */

/** Target average chunk size (bytes). */
#define CDC_TARGET_CHUNK_SIZE   (8  * 1024)       /* 8 KiB  */

/** Minimum chunk size — smaller splits are never emitted. */
#define CDC_MIN_CHUNK_SIZE      (2  * 1024)       /* 2 KiB  */

/** Maximum chunk size — larger splits are forced at this boundary. */
#define CDC_MAX_CHUNK_SIZE      (64 * 1024)       /* 64 KiB */

/** Width of the Rabin sliding window in bytes. */
#define RABIN_WINDOW_SIZE       64

/* -------------------------------------------------------------------------
 * Data Structures
 * ---------------------------------------------------------------------- */

/** Configuration block — can be zero-initialised to use all defaults. */
typedef struct CdcConfig {
    size_t   min_chunk_size;   /**< Minimum chunk size (0 → default).  */
    size_t   target_chunk_size;/**< Average/target chunk size (0 → default). */
    size_t   max_chunk_size;   /**< Maximum chunk size (0 → default).  */
} CdcConfig;

/**
 * RabinState — internal rolling-hash state.
 *
 * Callers should treat this as opaque and interact only through the API.
 */
typedef struct RabinState {
    uint64_t fingerprint;           /**< Current rolling hash value. */
    uint8_t  window[RABIN_WINDOW_SIZE]; /**< Sliding window bytes. */
    size_t   window_pos;            /**< Next write position in window. */
} RabinState;

/**
 * ChunkerCtx — top-level chunker context.
 *
 * One context per upload session.  The caller feeds bytes incrementally;
 * the chunker emits callbacks for each complete chunk.
 */
typedef struct ChunkerCtx ChunkerCtx;

/**
 * ChunkInfo — describes a single emitted chunk.
 */
typedef struct ChunkInfo {
    const uint8_t *data;    /**< Pointer into the current input buffer. */
    size_t         size;    /**< Byte length of this chunk. */
    uint64_t       offset;  /**< Byte offset of chunk start in the original
                             *   stream (for manifest ordering). */
} ChunkInfo;

/**
 * ChunkCallback — invoked by the chunker for each complete chunk boundary.
 *
 * The callback MUST NOT modify or free `info->data`; its lifetime is limited
 * to the callback invocation.
 *
 * @param info       Chunk descriptor.
 * @param user_data  Opaque pointer supplied to chunker_create().
 * @return           0 to continue chunking, non-zero to abort.
 */
typedef int (*ChunkCallback)(const ChunkInfo *info, void *user_data);

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * chunker_create — allocate a new chunker context.
 *
 * @param cfg       Chunk size configuration (pass NULL for defaults).
 * @param callback  Function called for each emitted chunk.
 * @param user_data Forwarded to every `callback` invocation.
 * @return          Heap-allocated context, or NULL on OOM.
 */
ChunkerCtx *chunker_create(const CdcConfig *cfg,
                            ChunkCallback    callback,
                            void            *user_data);

/**
 * chunker_feed — push bytes into the chunker.
 *
 * May invoke `callback` zero or more times as chunk boundaries are detected.
 *
 * @param ctx   Active chunker context.
 * @param data  Input bytes (may be a partial buffer from a read() call).
 * @param len   Number of bytes in `data`.
 * @return      0 on success, negative on error or if callback requested abort.
 */
int chunker_feed(ChunkerCtx *ctx, const uint8_t *data, size_t len);

/**
 * chunker_flush — signal end-of-stream and emit any remaining buffered data
 * as a final (possibly undersized) chunk.
 *
 * Must be called once after all data has been fed.
 *
 * @param ctx  Active chunker context.
 * @return     0 on success, negative on error.
 */
int chunker_flush(ChunkerCtx *ctx);

/**
 * chunker_reset — reset the context to process a new stream from scratch.
 *
 * Retains config and callback; only resets rolling state and byte counters.
 *
 * @param ctx  Context to reset.
 */
void chunker_reset(ChunkerCtx *ctx);

/**
 * chunker_destroy — free all resources.
 *
 * @param ctx  Context to destroy (may be NULL — no-op).
 */
void chunker_destroy(ChunkerCtx *ctx);

/**
 * chunker_stats — write a summary of chunk distribution to stderr.
 *
 * @param ctx    Context after processing (stats are cumulative).
 * @param label  Short label for the output line.
 */
void chunker_stats(const ChunkerCtx *ctx, const char *label);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_CHUNKER_H */
