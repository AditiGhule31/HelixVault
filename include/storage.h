/**
 * storage.h — Physical Chunk Storage Pool
 *
 * The storage pool manages the on-disk directory of deduplicated content-
 * addressed chunks.  Each chunk is stored as a flat binary file named after
 * the lowercase hex-encoded SHA-256 of its content.
 *
 * Directory layout under <store_root>/chunks/:
 *
 *   chunks/
 *     ab/                          ← first 2 hex chars (sharding prefix)
 *       abcdef0123456789...hex     ← full 64-char hex filename = chunk data
 *
 * Reference counting is persisted in a separate binary refcount index file
 * (<store_root>/chunks/refcounts.idx) that is memory-mapped (or read/written)
 * at startup and synced on every mutation.  The index is also a Write-Ahead
 * Log (WAL) target — every mutation is appended to a WAL journal first,
 * then applied in-place, so a crash leaves the WAL to replay on restart.
 *
 * Pool invariants:
 *   - A chunk file exists  iff  its refcount >= 1.
 *   - refcount == 0 triggers immediate GC (file deletion).
 *   - Two-level directory sharding limits directory entry count per directory.
 */

#ifndef HELIX_STORAGE_H
#define HELIX_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sha256.h"
#include "hashtable.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/** Directory shard prefix length (chars). 2 → 256 shard directories max. */
#define STORAGE_SHARD_PREFIX_LEN 2

/** Magic bytes at start of refcount index file. */
#define STORAGE_IDX_MAGIC   0x48454C58U   /* "HELX" */
#define STORAGE_IDX_VERSION 1U

/* -------------------------------------------------------------------------
 * Data Structures
 * ---------------------------------------------------------------------- */

/**
 * ChunkRef — in-memory reference count record for one chunk.
 */
typedef struct ChunkRef {
    char     hex[SHA256_HEX_SIZE];  /**< Content-address (hex-encoded hash). */
    uint32_t refcount;              /**< Current reference count. */
} ChunkRef;

/**
 * StoragePool — the top-level storage engine handle.
 */
typedef struct StoragePool {
    char      root[1024];        /**< Absolute path to the pool root dir. */
    char      chunks_dir[1024];  /**< root/chunks/ path. */
    char      idx_path[1024];    /**< root/chunks/refcounts.idx path. */
    char      wal_path[1024];    /**< root/chunks/refcounts.wal path. */
    HashTable *refcount_ht;      /**< In-memory refcount table (key=hex). */
    Arena     *meta_arena;       /**< Arena for ChunkRef nodes. */
    size_t     total_chunks;     /**< Live chunk count. */
    size_t     total_bytes;      /**< Cumulative bytes of stored chunk data. */
} StoragePool;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * storage_pool_open — open (or create) a physical storage pool.
 *
 * Creates all required directories if they do not exist.
 * Replays the WAL journal if an incomplete transaction is found.
 * Loads the refcount index into memory.
 *
 * @param root_dir  Absolute or relative path to the pool root directory.
 * @return          Heap-allocated pool handle, or NULL on error.
 */
StoragePool *storage_pool_open(const char *root_dir);

/**
 * storage_pool_close — flush state and free all resources.
 *
 * Persists the in-memory refcount table to disk.
 * After this call `pool` is invalid.
 *
 * @param pool  Pool to close (may be NULL — no-op).
 */
void storage_pool_close(StoragePool *pool);

/* -------------------------------------------------------------------------
 * Chunk Operations
 * ---------------------------------------------------------------------- */

/**
 * storage_chunk_write — write a new chunk into the pool (refcount → 1),
 * or increment the refcount of an existing chunk.
 *
 * This is idempotent: writing identical content twice is safe and efficient
 * (the chunk is stored only once; the refcount is incremented on repeat).
 *
 * @param pool    Active storage pool.
 * @param data    Chunk payload bytes.
 * @param size    Number of bytes in `data`.
 * @param hex_out Output buffer of SHA256_HEX_SIZE bytes receiving the
 *                chunk's hex-encoded content hash (its storage key).
 * @return        0 on success, negative on error.
 */
int storage_chunk_write(StoragePool *pool,
                        const uint8_t *data, size_t size,
                        char hex_out[SHA256_HEX_SIZE]);

/**
 * storage_chunk_read — read a chunk's raw bytes into a caller-supplied buffer.
 *
 * @param pool      Active storage pool.
 * @param hex       Hex-encoded chunk identifier.
 * @param buf       Output buffer (must be at least `*out_size` bytes).
 * @param buf_size  Size of `buf`.
 * @param out_size  Set to the actual chunk size on success.
 * @return          0 on success, -1 if chunk not found, negative on I/O error.
 */
int storage_chunk_read(const StoragePool *pool,
                       const char *hex,
                       uint8_t *buf, size_t buf_size,
                       size_t *out_size);

/**
 * storage_chunk_read_alloc — read a chunk, allocating the buffer on the heap.
 *
 * Caller is responsible for calling free() on *buf_out.
 *
 * @param pool      Active storage pool.
 * @param hex       Hex-encoded chunk identifier.
 * @param buf_out   Receives pointer to heap-allocated chunk data.
 * @param size_out  Receives chunk byte count.
 * @return          0 on success, negative on error.
 */
int storage_chunk_read_alloc(const StoragePool *pool,
                             const char *hex,
                             uint8_t **buf_out, size_t *size_out);

/**
 * storage_chunk_decref — decrement the reference count of a chunk.
 *
 * If the refcount reaches zero the chunk file is deleted from disk (GC).
 *
 * @param pool  Active storage pool.
 * @param hex   Hex-encoded chunk identifier.
 * @return      0 on success, -1 if the chunk is unknown, negative on I/O error.
 */
int storage_chunk_decref(StoragePool *pool, const char *hex);

/**
 * storage_chunk_exists — test whether a chunk is present in the pool.
 *
 * @param pool  Active storage pool.
 * @param hex   Hex-encoded chunk identifier.
 * @return      true if the chunk exists and has refcount >= 1.
 */
bool storage_chunk_exists(const StoragePool *pool, const char *hex);

/**
 * storage_chunk_refcount — return the current reference count of a chunk.
 *
 * @param pool  Active storage pool.
 * @param hex   Hex-encoded chunk identifier.
 * @return      Current refcount (0 if unknown).
 */
uint32_t storage_chunk_refcount(const StoragePool *pool, const char *hex);

/* -------------------------------------------------------------------------
 * Garbage Collection
 * ---------------------------------------------------------------------- */

/**
 * storage_gc_run — scan the pool and delete all chunks with refcount == 0.
 *
 * Under normal operation refcount never reaches 0 without an immediate GC;
 * this routine is a safety sweep for crash-recovery scenarios.
 *
 * @param pool  Active storage pool.
 * @return      Number of chunks deleted.
 */
size_t storage_gc_run(StoragePool *pool);

/* -------------------------------------------------------------------------
 * Persistence (WAL + Index)
 * ---------------------------------------------------------------------- */

/**
 * storage_flush — persist the in-memory refcount table to disk.
 *
 * Uses the WAL pattern: write to <wal_path> first, rename atomically.
 *
 * @param pool  Active storage pool.
 * @return      0 on success, negative on I/O error.
 */
int storage_flush(StoragePool *pool);

/**
 * storage_pool_stats — print pool statistics to stderr.
 *
 * @param pool   Active storage pool.
 * @param label  Short label for the header line.
 */
void storage_pool_stats(const StoragePool *pool, const char *label);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/**
 * storage_chunk_path — compute the on-disk path for a chunk.
 *
 * @param pool    Active storage pool.
 * @param hex     Chunk hex identifier.
 * @param buf     Output path buffer.
 * @param buf_sz  Size of `buf`.
 */
void storage_chunk_path(const StoragePool *pool, const char *hex,
                        char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_STORAGE_H */
