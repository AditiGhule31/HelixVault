/**
 * bucket.h — Bucket & Object Metadata Management
 *
 * Implements the S3-style bucket abstraction and the high-level object
 * operations (PUT, GET, DELETE, LIST) that sit on top of the storage pool
 * and manifest layers.
 *
 * On-disk directory layout under <store_root>/:
 *
 *   buckets/
 *     <bucket_name>/
 *       _bucket.meta          — bucket metadata (JSON-ish flat text)
 *       <object_key>.manifest — per-object manifest (binary, see manifest.h)
 *   chunks/                   — physical dedup pool (see storage.h)
 *   wal/                      — write-ahead log directory
 *
 * Bucket metadata file format (one field per line, colon-delimited):
 *   created_at:<unix_seconds>
 *   object_count:<count>
 */

#ifndef HELIX_BUCKET_H
#define HELIX_BUCKET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "storage.h"
#include "manifest.h"
#include "hashtable.h"
#include "arena.h"
#include "chunker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/** Maximum valid bucket name length (excluding '\0'). */
#define BUCKET_NAME_MAX 63

/** Maximum valid object key length (excluding '\0'). */
#define OBJECT_KEY_MAX  1023

/** Name of the per-bucket metadata file. */
#define BUCKET_META_FILE "_bucket.meta"

/** File extension for manifest files. */
#define MANIFEST_EXT ".manifest"

/* -------------------------------------------------------------------------
 * Data Structures
 * ---------------------------------------------------------------------- */

/** In-memory metadata record for one bucket. */
typedef struct BucketMeta {
    char   name[BUCKET_NAME_MAX + 1];  /**< Bucket name. */
    time_t created_at;                 /**< Unix creation timestamp. */
    size_t object_count;               /**< Number of manifests in this bucket. */
} BucketMeta;

/**
 * ObjectInfo — lightweight summary of one object (used for LIST results).
 */
typedef struct ObjectInfo {
    char     key[OBJECT_KEY_MAX + 1];  /**< Object key. */
    uint64_t size;                      /**< Total object size in bytes. */
    time_t   created_at;               /**< Upload timestamp. */
    char     content_type[MANIFEST_CONTENT_TYPE_MAX]; /**< MIME type. */
} ObjectInfo;

/**
 * Store — top-level context for the entire storage system.
 *
 * A single Store instance governs all buckets and the shared chunk pool.
 */
typedef struct Store {
    char         root[1024];     /**< Absolute root directory path. */
    char         buckets_dir[1024]; /**< root/buckets/ path. */
    StoragePool *pool;           /**< Shared physical chunk pool. */
    HashTable   *bucket_cache;   /**< In-memory BucketMeta cache (name→meta). */
    Arena       *meta_arena;     /**< Arena for metadata allocations. */
    CdcConfig    cdc_cfg;        /**< CDC parameters for PUT operations. */
} Store;

/**
 * ListResult — holds the output of a bucket LIST operation.
 *
 * Callers must call list_result_free() when done.
 */
typedef struct ListResult {
    ObjectInfo *items;    /**< Heap-allocated array of ObjectInfo. */
    size_t      count;    /**< Number of items. */
    size_t      capacity; /**< Allocated capacity of items[]. */
} ListResult;

/* -------------------------------------------------------------------------
 * Store Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * store_open — open (or initialise) the HelixVault storage system.
 *
 * Creates all required directories on first run.
 *
 * @param root_dir  Path to the storage root (created if absent).
 * @param cdc_cfg   CDC configuration; pass NULL for defaults.
 * @return          Heap-allocated Store handle, or NULL on error.
 */
Store *store_open(const char *root_dir, const CdcConfig *cdc_cfg);

/**
 * store_close — flush and free all resources.
 *
 * @param store  Store to close (may be NULL — no-op).
 */
void store_close(Store *store);

/* -------------------------------------------------------------------------
 * Bucket Operations
 * ---------------------------------------------------------------------- */

/**
 * bucket_create — create a new bucket.
 *
 * @param store  Active store.
 * @param name   Bucket name (alphanumeric and hyphens only, max 63 chars).
 * @return       0 on success, -1 if bucket already exists, negative on error.
 */
int bucket_create(Store *store, const char *name);

/**
 * bucket_delete — delete an empty bucket.
 *
 * Returns an error if the bucket contains objects.
 *
 * @param store  Active store.
 * @param name   Bucket name.
 * @return       0 on success, -1 if not found, -2 if not empty, negative on error.
 */
int bucket_delete(Store *store, const char *name);

/**
 * bucket_exists — test whether a bucket exists.
 *
 * @param store  Active store.
 * @param name   Bucket name.
 * @return       true if the bucket exists.
 */
bool bucket_exists(const Store *store, const char *name);

/**
 * bucket_list — enumerate all bucket names.
 *
 * @param store       Active store.
 * @param names_out   Heap-allocated array of heap-allocated name strings.
 * @param count_out   Number of entries in names_out.
 * @return            0 on success, negative on error.
 *                    Caller must free each names_out[i] and then names_out.
 */
int bucket_list(const Store *store, char ***names_out, size_t *count_out);

/* -------------------------------------------------------------------------
 * Object Operations
 * ---------------------------------------------------------------------- */

/**
 * object_put — upload a file, chunk it, deduplicate, and save its manifest.
 *
 * Streams the file at `src_path`, applies CDC chunking, writes each unique
 * chunk to the pool, and saves the resulting manifest in the bucket.
 *
 * @param store         Active store.
 * @param bucket        Destination bucket name.
 * @param key           Object key (relative path within the bucket).
 * @param src_path      Path to the local file to upload.
 * @param content_type  MIME type (NULL → "application/octet-stream").
 * @return              0 on success, negative on error.
 */
int object_put(Store *store, const char *bucket, const char *key,
               const char *src_path, const char *content_type);

/**
 * object_put_buffer — upload an in-memory buffer as an object.
 *
 * @param store         Active store.
 * @param bucket        Destination bucket name.
 * @param key           Object key.
 * @param data          Input bytes.
 * @param size          Input byte count.
 * @param content_type  MIME type (NULL → "application/octet-stream").
 * @return              0 on success, negative on error.
 */
int object_put_buffer(Store *store, const char *bucket, const char *key,
                      const uint8_t *data, size_t size,
                      const char *content_type);

/**
 * object_get — reconstruct an object and write it to a destination file.
 *
 * Reads the manifest, fetches each chunk in order, and streams the
 * reconstructed bytes to `dst_path`.
 *
 * @param store    Active store.
 * @param bucket   Source bucket name.
 * @param key      Object key.
 * @param dst_path Destination file path (created or overwritten).
 * @return         0 on success, -1 if not found, negative on error.
 */
int object_get(const Store *store, const char *bucket, const char *key,
               const char *dst_path);

/**
 * object_get_buffer — reconstruct an object into a heap-allocated buffer.
 *
 * Caller must call free(*buf_out) when done.
 *
 * @param store     Active store.
 * @param bucket    Source bucket name.
 * @param key       Object key.
 * @param buf_out   Receives pointer to heap buffer with object bytes.
 * @param size_out  Receives total object byte count.
 * @return          0 on success, negative on error.
 */
int object_get_buffer(const Store *store, const char *bucket, const char *key,
                      uint8_t **buf_out, size_t *size_out);

/**
 * object_delete — delete an object and decrement chunk reference counts.
 *
 * Triggers garbage collection for any chunks whose refcount drops to zero.
 *
 * @param store   Active store.
 * @param bucket  Bucket name.
 * @param key     Object key.
 * @return        0 on success, -1 if not found, negative on error.
 */
int object_delete(Store *store, const char *bucket, const char *key);

/**
 * object_exists — test whether an object exists.
 *
 * @param store   Active store.
 * @param bucket  Bucket name.
 * @param key     Object key.
 * @return        true if the manifest file is present.
 */
bool object_exists(const Store *store, const char *bucket, const char *key);

/**
 * object_info — retrieve lightweight metadata for an object.
 *
 * @param store   Active store.
 * @param bucket  Bucket name.
 * @param key     Object key.
 * @param info    Output struct filled on success.
 * @return        0 on success, -1 if not found, negative on error.
 */
int object_info(const Store *store, const char *bucket, const char *key,
                ObjectInfo *info);

/**
 * object_list — list all objects in a bucket.
 *
 * @param store    Active store.
 * @param bucket   Bucket name.
 * @param result   Output list (must be freed with list_result_free()).
 * @return         0 on success, negative on error.
 */
int object_list(const Store *store, const char *bucket, ListResult *result);

/**
 * list_result_free — free all resources in a ListResult.
 *
 * @param result  ListResult to free.
 */
void list_result_free(ListResult *result);

/* -------------------------------------------------------------------------
 * Manifest Path Helpers
 * ---------------------------------------------------------------------- */

/**
 * store_manifest_path — build the on-disk path to an object's manifest.
 *
 * @param store   Active store.
 * @param bucket  Bucket name.
 * @param key     Object key.
 * @param buf     Output buffer.
 * @param buf_sz  Size of buffer.
 */
void store_manifest_path(const Store *store,
                         const char *bucket, const char *key,
                         char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_BUCKET_H */
