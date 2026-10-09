/**
 * storage.c — Physical Chunk Storage Pool Implementation
 *
 * Manages the on-disk chunk store with:
 *   - Two-level directory sharding (first 2 hex chars)
 *   - Binary refcount index with WAL (write-ahead log)
 *   - In-memory hash table for O(1) refcount queries
 *   - Automatic GC when refcount reaches zero
 *
 * All nested-function patterns have been avoided for strict C99 portability.
 * Iteration callbacks use a separate top-level context struct.
 */

#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define MKDIR(p) _mkdir(p)
#  define STAT_ST  struct _stat
#  define STAT_FN  _stat
#else
#  define MKDIR(p) mkdir((p), 0755)
#  define STAT_ST  struct stat
#  define STAT_FN  stat
#endif

/* -------------------------------------------------------------------------
 * Internal WAL record format
 * ---------------------------------------------------------------------- */

#define WAL_MAGIC    0x57414C58U   /* "WALX" */

typedef enum {
    WAL_OP_INCREF = 1,
    WAL_OP_DECREF = 2,
    WAL_OP_DELETE = 3,
} WalOp;

#pragma pack(push, 1)
typedef struct WalRecord {
    uint32_t magic;
    uint8_t  op;
    char     hex[SHA256_HEX_SIZE];
    uint32_t new_refcount;
    uint32_t crc;
} WalRecord;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * CRC-32 (IEEE 802.3 polynomial 0xEDB88320)
 * ---------------------------------------------------------------------- */

static uint32_t storage_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFU;
}

/* -------------------------------------------------------------------------
 * Refcount index (binary format)
 *
 *   [IdxHeader]             — fixed header
 *   [ChunkRef × count]      — flat array of refcount records
 * ---------------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct IdxHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t crc;
} IdxHeader;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Filesystem helpers
 * ---------------------------------------------------------------------- */

static void mkdirs(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len-1] == '/' || tmp[len-1] == '\\'))
        tmp[--len] = '\0';

    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == '\0') {
            char save = tmp[i];
            tmp[i] = '\0';
            MKDIR(tmp);
            tmp[i] = save;
        }
    }
}

static bool file_exists(const char *path) {
    STAT_ST st;
    return STAT_FN(path, &st) == 0;
}

static long get_file_size(const char *path) {
    STAT_ST st;
    if (STAT_FN(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static int atomic_write(const char *path, const void *data, size_t size) {
    char tmp_path[2048];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -1;

    int ok = (size == 0 || fwrite(data, 1, size, f) == size);
    fclose(f);

    if (!ok) { remove(tmp_path); return -1; }

#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return -1; }
    return 0;
}

/* -------------------------------------------------------------------------
 * Index serialisation helpers
 *
 * We cannot use nested functions in C99, so the iteration callback is a
 * top-level function that receives a pointer to a local context struct.
 * ---------------------------------------------------------------------- */

typedef struct {
    ChunkRef *arr;   /* destination array */
    size_t   *pidx;  /* pointer to current write index */
} CollectCtx;

static void collect_iter_fn(const char *key, void *value, void *ud) {
    (void)key;
    CollectCtx *ctx = (CollectCtx *)ud;
    ChunkRef   *ref = (ChunkRef   *)value;
    ctx->arr[(*ctx->pidx)++] = *ref;
}

static uint8_t *idx_serialise(const HashTable *ht, size_t *out_size) {
    size_t count      = ht_count(ht);
    size_t entries_sz = count * sizeof(ChunkRef);
    size_t total      = sizeof(IdxHeader) + entries_sz;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    ChunkRef *entries = (ChunkRef *)(buf + sizeof(IdxHeader));
    size_t    idx_val = 0;

    CollectCtx cctx = { entries, &idx_val };
    ht_iterate(ht, collect_iter_fn, &cctx);

    uint32_t crc = storage_crc32(entries, entries_sz);

    IdxHeader hdr;
    hdr.magic   = STORAGE_IDX_MAGIC;
    hdr.version = STORAGE_IDX_VERSION;
    hdr.count   = (uint32_t)count;
    hdr.crc     = crc;
    memcpy(buf, &hdr, sizeof(IdxHeader));

    *out_size = total;
    return buf;
}

static int idx_load(StoragePool *pool, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;   /* No index yet — OK on first run. */

    IdxHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f); return -1;
    }
    if (hdr.magic != STORAGE_IDX_MAGIC || hdr.version != STORAGE_IDX_VERSION) {
        fprintf(stderr, "[storage] Bad index magic/version in %s\n", path);
        fclose(f); return -1;
    }

    size_t   entries_sz = (size_t)hdr.count * sizeof(ChunkRef);
    ChunkRef *entries   = NULL;

    if (entries_sz > 0) {
        entries = (ChunkRef *)malloc(entries_sz);
        if (!entries) { fclose(f); return -1; }
        if (fread(entries, 1, entries_sz, f) != entries_sz) {
            free(entries); fclose(f); return -1;
        }
    }
    fclose(f);

    uint32_t crc = storage_crc32(entries, entries_sz);
    if (crc != hdr.crc) {
        fprintf(stderr, "[storage] Index CRC mismatch in %s — possible corruption\n", path);
        free(entries);
        return -1;
    }

    for (uint32_t i = 0; i < hdr.count; i++) {
        ChunkRef *ref = (ChunkRef *)arena_alloc(pool->meta_arena, sizeof(ChunkRef));
        if (!ref) { free(entries); return -1; }
        *ref = entries[i];
        ht_insert(pool->refcount_ht, ref->hex, ref);
    }

    pool->total_chunks = hdr.count;
    free(entries);
    return 0;
}

/* -------------------------------------------------------------------------
 * WAL replay & append
 * ---------------------------------------------------------------------- */

static int wal_replay(StoragePool *pool) {
    FILE *f = fopen(pool->wal_path, "rb");
    if (!f) return 0;

    fprintf(stderr, "[storage] Replaying WAL journal %s ...\n", pool->wal_path);
    int       replayed = 0;
    WalRecord rec;

    while (fread(&rec, 1, sizeof(rec), f) == sizeof(rec)) {
        /* Validate record integrity (CRC covers everything except the crc field). */
        size_t   body_sz = sizeof(rec) - sizeof(rec.crc);
        uint32_t crc     = storage_crc32(&rec, body_sz);
        if (crc != rec.crc || rec.magic != WAL_MAGIC) break;

        ChunkRef *ref = (ChunkRef *)ht_lookup(pool->refcount_ht, rec.hex);

        if (rec.op == WAL_OP_DELETE || rec.new_refcount == 0) {
            if (ref) {
                ht_delete(pool->refcount_ht, rec.hex);
                if (pool->total_chunks > 0) pool->total_chunks--;
            }
        } else {
            if (!ref) {
                ref = (ChunkRef *)arena_alloc(pool->meta_arena, sizeof(ChunkRef));
                if (!ref) break;
                memcpy(ref->hex, rec.hex, SHA256_HEX_SIZE);
                ref->refcount = rec.new_refcount;
                ht_insert(pool->refcount_ht, ref->hex, ref);
                pool->total_chunks++;
            } else {
                ref->refcount = rec.new_refcount;
            }
        }
        replayed++;
    }

    fclose(f);
    fprintf(stderr, "[storage] WAL replayed %d records.\n", replayed);
    remove(pool->wal_path);
    return 0;
}

static int wal_append(StoragePool *pool, WalOp op,
                      const char *hex, uint32_t new_refcount) {
    FILE *f = fopen(pool->wal_path, "ab");
    if (!f) return -1;

    WalRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic        = WAL_MAGIC;
    rec.op           = (uint8_t)op;
    memcpy(rec.hex, hex, SHA256_HEX_SIZE);
    rec.new_refcount = new_refcount;
    size_t body_sz   = sizeof(rec) - sizeof(rec.crc);
    rec.crc          = storage_crc32(&rec, body_sz);

    int rc = (fwrite(&rec, 1, sizeof(rec), f) == sizeof(rec)) ? 0 : -1;
    fclose(f);
    return rc;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

StoragePool *storage_pool_open(const char *root_dir) {
    StoragePool *pool = (StoragePool *)calloc(1, sizeof(StoragePool));
    if (!pool) return NULL;

    snprintf(pool->root,       sizeof(pool->root),
             "%s", root_dir);
    snprintf(pool->chunks_dir, sizeof(pool->chunks_dir),
             "%s/chunks", root_dir);
    snprintf(pool->idx_path,   sizeof(pool->idx_path),
             "%s/chunks/refcounts.idx", root_dir);
    snprintf(pool->wal_path,   sizeof(pool->wal_path),
             "%s/chunks/refcounts.wal", root_dir);

    mkdirs(pool->root);
    mkdirs(pool->chunks_dir);

    pool->meta_arena  = arena_create(0);
    pool->refcount_ht = ht_create(NULL, NULL);

    if (!pool->meta_arena || !pool->refcount_ht) goto fail;

    /* Replay any pending WAL before loading the stable index. */
    wal_replay(pool);
    idx_load(pool, pool->idx_path);

    return pool;

fail:
    arena_destroy(pool->meta_arena);
    ht_destroy(pool->refcount_ht);
    free(pool);
    return NULL;
}

void storage_pool_close(StoragePool *pool) {
    if (!pool) return;
    storage_flush(pool);
    arena_destroy(pool->meta_arena);
    ht_destroy(pool->refcount_ht);
    free(pool);
}

/* -------------------------------------------------------------------------
 * Path helpers
 * ---------------------------------------------------------------------- */

void storage_chunk_path(const StoragePool *pool, const char *hex,
                        char *buf, size_t buf_sz) {
    char shard[STORAGE_SHARD_PREFIX_LEN + 1];
    memcpy(shard, hex, STORAGE_SHARD_PREFIX_LEN);
    shard[STORAGE_SHARD_PREFIX_LEN] = '\0';
    snprintf(buf, buf_sz, "%s/%s/%s", pool->chunks_dir, shard, hex);
}

/* -------------------------------------------------------------------------
 * Chunk write — deduplication logic
 * ---------------------------------------------------------------------- */

int storage_chunk_write(StoragePool *pool,
                        const uint8_t *data, size_t size,
                        char hex_out[SHA256_HEX_SIZE]) {
    if (!pool || !data || !hex_out) return -1;

    /* Derive content address. */
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_hash(data, size, digest);
    sha256_hex(digest, hex_out);

    /* Existing chunk — just bump its refcount. */
    ChunkRef *ref = (ChunkRef *)ht_lookup(pool->refcount_ht, hex_out);
    if (ref) {
        ref->refcount++;
        wal_append(pool, WAL_OP_INCREF, hex_out, ref->refcount);
        return 0;
    }

    /* New chunk — write the physical file. */
    char chunk_path[2048];
    storage_chunk_path(pool, hex_out, chunk_path, sizeof(chunk_path));

    /* Ensure the shard directory exists. */
    char shard_dir[2048];
    char shard[STORAGE_SHARD_PREFIX_LEN + 1];
    memcpy(shard, hex_out, STORAGE_SHARD_PREFIX_LEN);
    shard[STORAGE_SHARD_PREFIX_LEN] = '\0';
    snprintf(shard_dir, sizeof(shard_dir), "%s/%s", pool->chunks_dir, shard);
    mkdirs(shard_dir);

    if (atomic_write(chunk_path, data, size) != 0) {
        fprintf(stderr, "[storage] Failed to write chunk %s\n", hex_out);
        return -1;
    }

    /* Create in-memory record. */
    ref = (ChunkRef *)arena_alloc(pool->meta_arena, sizeof(ChunkRef));
    if (!ref) return -1;
    memcpy(ref->hex, hex_out, SHA256_HEX_SIZE);
    ref->refcount = 1;

    ht_insert(pool->refcount_ht, ref->hex, ref);
    pool->total_chunks++;
    pool->total_bytes += size;

    wal_append(pool, WAL_OP_INCREF, hex_out, 1);
    return 0;
}

/* -------------------------------------------------------------------------
 * Chunk read
 * ---------------------------------------------------------------------- */

int storage_chunk_read(const StoragePool *pool,
                       const char *hex,
                       uint8_t *buf, size_t buf_size,
                       size_t *out_size) {
    if (!pool || !hex || !buf || !out_size) return -1;

    char path[2048];
    storage_chunk_path(pool, hex, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t n = fread(buf, 1, buf_size, f);
    fclose(f);

    if (n == 0 && buf_size > 0) return -1;
    *out_size = n;
    return 0;
}

int storage_chunk_read_alloc(const StoragePool *pool,
                             const char *hex,
                             uint8_t **buf_out, size_t *size_out) {
    if (!pool || !hex || !buf_out || !size_out) return -1;

    char path[2048];
    storage_chunk_path(pool, hex, path, sizeof(path));

    long sz = get_file_size(path);
    if (sz < 0) return -1;

    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); return -1; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if ((long)n != sz) { free(buf); return -1; }

    *buf_out  = buf;
    *size_out = (size_t)sz;
    return 0;
}

/* -------------------------------------------------------------------------
 * Chunk decrement + inline GC
 * ---------------------------------------------------------------------- */

int storage_chunk_decref(StoragePool *pool, const char *hex) {
    if (!pool || !hex) return -1;

    ChunkRef *ref = (ChunkRef *)ht_lookup(pool->refcount_ht, hex);
    if (!ref || ref->refcount == 0) return -1;

    ref->refcount--;

    if (ref->refcount == 0) {
        /* GC: delete the physical file immediately. */
        char path[2048];
        storage_chunk_path(pool, hex, path, sizeof(path));
        remove(path);

        wal_append(pool, WAL_OP_DELETE, hex, 0);
        ht_delete(pool->refcount_ht, hex);
        if (pool->total_chunks > 0) pool->total_chunks--;
    } else {
        wal_append(pool, WAL_OP_DECREF, hex, ref->refcount);
    }

    return 0;
}

bool storage_chunk_exists(const StoragePool *pool, const char *hex) {
    if (!pool || !hex) return false;
    return ht_contains(pool->refcount_ht, hex);
}

uint32_t storage_chunk_refcount(const StoragePool *pool, const char *hex) {
    if (!pool || !hex) return 0;
    ChunkRef *ref = (ChunkRef *)ht_lookup(pool->refcount_ht, hex);
    return ref ? ref->refcount : 0;
}

/* -------------------------------------------------------------------------
 * Garbage Collection sweep
 *
 * Collects zero-refcount entries in a two-pass approach (iterate then delete)
 * to avoid invalidating the hash table during iteration.
 * ---------------------------------------------------------------------- */

typedef struct {
    char  **keys;
    size_t  count;
    size_t  cap;
} GcCollectCtx;

static void gc_iter_fn(const char *key, void *value, void *ud) {
    ChunkRef      *ref = (ChunkRef      *)value;
    GcCollectCtx  *ctx = (GcCollectCtx  *)ud;

    if (ref->refcount != 0) return;

    if (ctx->count >= ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 16;
        char **nk = (char **)realloc(ctx->keys, new_cap * sizeof(char *));
        if (!nk) return;
        ctx->keys = nk;
        ctx->cap  = new_cap;
    }
    ctx->keys[ctx->count++] = strdup(key);
}

size_t storage_gc_run(StoragePool *pool) {
    if (!pool) return 0;

    GcCollectCtx gc = {NULL, 0, 0};
    ht_iterate(pool->refcount_ht, gc_iter_fn, &gc);

    for (size_t i = 0; i < gc.count; i++) {
        char path[2048];
        storage_chunk_path(pool, gc.keys[i], path, sizeof(path));
        remove(path);
        ht_delete(pool->refcount_ht, gc.keys[i]);
        if (pool->total_chunks > 0) pool->total_chunks--;
        free(gc.keys[i]);
    }

    free(gc.keys);
    return gc.count;
}

/* -------------------------------------------------------------------------
 * Persistence flush (WAL → stable index)
 * ---------------------------------------------------------------------- */

int storage_flush(StoragePool *pool) {
    if (!pool) return -1;

    size_t   buf_sz = 0;
    uint8_t *buf    = idx_serialise(pool->refcount_ht, &buf_sz);
    if (!buf) return -1;

    int rc = atomic_write(pool->idx_path, buf, buf_sz);
    free(buf);

    if (rc == 0)
        remove(pool->wal_path);   /* WAL is now redundant. */

    return rc;
}

void storage_pool_stats(const StoragePool *pool, const char *label) {
    if (!label) label = "storage_pool";
    fprintf(stderr,
            "[storage_pool_stats] %-20s | chunks=%zu  bytes=%zu\n",
            label,
            pool ? pool->total_chunks : (size_t)0,
            pool ? pool->total_bytes  : (size_t)0);
}
