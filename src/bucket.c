/**
 * bucket.c — Bucket & Object Operations Implementation
 *
 * Implements the high-level S3-style API: bucket CRUD, object PUT/GET/DELETE/LIST.
 * Wires together the chunker, storage pool, and manifest layers.
 */

#include "bucket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define MKDIR(p)          _mkdir(p)
#  define STAT_ST           struct _stat
#  define STAT_FN           _stat
#  define S_ISDIR(m)        (((m) & _S_IFMT) == _S_IFDIR)
#  define S_ISREG(m)        (((m) & _S_IFMT) == _S_IFREG)
/* Portable directory scanning on Windows using _findfirst/_findnext. */
#  include <windows.h>
#else
#  include <dirent.h>
#  define MKDIR(p)          mkdir((p), 0755)
#  define STAT_ST           struct stat
#  define STAT_FN           stat
#endif

/* -------------------------------------------------------------------------
 * Filesystem helpers
 * ---------------------------------------------------------------------- */

static int mkdirs(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len-1]=='/' || tmp[len-1]=='\\'))
        tmp[--len] = '\0';

    for (size_t i = 1; i <= len; i++) {
        if (tmp[i]=='/' || tmp[i]=='\\' || tmp[i]=='\0') {
            char save = tmp[i]; tmp[i] = '\0';
            MKDIR(tmp);
            tmp[i] = save;
        }
    }
    return 0;
}

static bool dir_exists(const char *path) {
    STAT_ST st;
    return STAT_FN(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const char *path) {
    STAT_ST st;
    return STAT_FN(path, &st) == 0;
}

/** Validate a bucket name: alphanumeric plus hyphens, 1-63 chars. */
static bool valid_bucket_name(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len > BUCKET_NAME_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Bucket metadata file I/O
 * ---------------------------------------------------------------------- */

/** Write bucket metadata to <bucket_dir>/_bucket.meta */
static int bucket_meta_write(const char *bucket_dir, const BucketMeta *meta) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", bucket_dir, BUCKET_META_FILE);

    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    fprintf(f, "created_at:%lld\n",    (long long)meta->created_at);
    fprintf(f, "object_count:%zu\n",   meta->object_count);
    fclose(f);

#ifdef _WIN32
    remove(path);
#endif
    return rename(tmp, path) == 0 ? 0 : -1;
}

/** Read bucket metadata from <bucket_dir>/_bucket.meta */
static int bucket_meta_read(const char *bucket_dir, BucketMeta *meta) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", bucket_dir, BUCKET_META_FILE);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long ll;
        size_t   sz;
        if (sscanf(line, "created_at:%lld", &ll)   == 1) meta->created_at   = (time_t)ll;
        if (sscanf(line, "object_count:%zu", &sz)  == 1) meta->object_count = sz;
    }
    fclose(f);
    return 0;
}

/** Count manifests in a bucket directory (object count). */
static size_t count_manifests(const char *bucket_dir) {
    size_t count = 0;
#ifdef _WIN32
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*" MANIFEST_EXT, bucket_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { count++; } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(bucket_dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        size_t elen = strlen(MANIFEST_EXT);
        if (nlen > elen && strcmp(ent->d_name + nlen - elen, MANIFEST_EXT) == 0)
            count++;
    }
    closedir(d);
#endif
    return count;
}

/* -------------------------------------------------------------------------
 * Store Lifecycle
 * ---------------------------------------------------------------------- */

Store *store_open(const char *root_dir, const CdcConfig *cdc_cfg) {
    Store *s = (Store *)calloc(1, sizeof(Store));
    if (!s) return NULL;

    snprintf(s->root,       sizeof(s->root),       "%s", root_dir);
    snprintf(s->buckets_dir, sizeof(s->buckets_dir), "%s/buckets", root_dir);

    mkdirs(s->root);
    mkdirs(s->buckets_dir);

    s->meta_arena   = arena_create(0);
    s->bucket_cache = ht_create(s->meta_arena, NULL);
    if (!s->meta_arena || !s->bucket_cache) goto fail;

    /* Open the physical chunk pool. */
    s->pool = storage_pool_open(root_dir);
    if (!s->pool) goto fail;

    /* CDC configuration. */
    if (cdc_cfg)
        s->cdc_cfg = *cdc_cfg;
    /* Zeros in cdc_cfg mean "use defaults", which chunker_create handles. */

    return s;

fail:
    arena_destroy(s->meta_arena);
    ht_destroy(s->bucket_cache);
    storage_pool_close(s->pool);
    free(s);
    return NULL;
}

void store_close(Store *store) {
    if (!store) return;
    storage_pool_close(store->pool);
    ht_destroy(store->bucket_cache);
    arena_destroy(store->meta_arena);
    free(store);
}

/* -------------------------------------------------------------------------
 * Bucket Operations
 * ---------------------------------------------------------------------- */

int bucket_create(Store *store, const char *name) {
    if (!store || !name) return -3;
    if (!valid_bucket_name(name)) {
        fprintf(stderr, "[bucket] Invalid bucket name '%s'\n", name);
        return -3;
    }

    char bucket_dir[2048];
    snprintf(bucket_dir, sizeof(bucket_dir), "%s/%s", store->buckets_dir, name);

    if (dir_exists(bucket_dir)) {
        fprintf(stderr, "[bucket] Bucket '%s' already exists\n", name);
        return -1;
    }

    if (mkdirs(bucket_dir) != 0) return -3;

    BucketMeta meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.name, sizeof(meta.name), "%s", name);
    meta.created_at   = time(NULL);
    meta.object_count = 0;

    if (bucket_meta_write(bucket_dir, &meta) != 0) return -3;

    /* Cache the metadata in-memory. */
    BucketMeta *cached = (BucketMeta *)arena_alloc(store->meta_arena, sizeof(BucketMeta));
    if (cached) {
        *cached = meta;
        ht_insert(store->bucket_cache, name, cached);
    }

    return 0;
}

int bucket_delete(Store *store, const char *name) {
    if (!store || !name) return -3;

    char bucket_dir[2048];
    snprintf(bucket_dir, sizeof(bucket_dir), "%s/%s", store->buckets_dir, name);

    if (!dir_exists(bucket_dir)) return -1;

    /* Refuse to delete non-empty buckets. */
    size_t obj_count = count_manifests(bucket_dir);
    if (obj_count > 0) {
        fprintf(stderr, "[bucket] Bucket '%s' is not empty (%zu objects)\n",
                name, obj_count);
        return -2;
    }

    /* Remove the meta file. */
    char meta_path[2048];
    snprintf(meta_path, sizeof(meta_path), "%s/%s", bucket_dir, BUCKET_META_FILE);
    remove(meta_path);

    /* Remove the (now empty) directory. */
#ifdef _WIN32
    _rmdir(bucket_dir);
#else
    rmdir(bucket_dir);
#endif

    ht_delete(store->bucket_cache, name);
    return 0;
}

bool bucket_exists(const Store *store, const char *name) {
    if (!store || !name) return false;
    char bucket_dir[2048];
    snprintf(bucket_dir, sizeof(bucket_dir), "%s/%s", store->buckets_dir, name);
    return dir_exists(bucket_dir);
}

int bucket_list(const Store *store, char ***names_out, size_t *count_out) {
    if (!store || !names_out || !count_out) return -1;

    *names_out = NULL;
    *count_out = 0;

    size_t cap  = 0;
    size_t count = 0;
    char **arr  = NULL;

#ifdef _WIN32
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*", store->buckets_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { *names_out = NULL; *count_out = 0; return 0; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0) continue;
            /* Verify it has a _bucket.meta */
            char meta_p[2048];
            snprintf(meta_p, sizeof(meta_p), "%s\\%s\\%s",
                     store->buckets_dir, fd.cFileName, BUCKET_META_FILE);
            if (!file_exists(meta_p)) continue;

            if (count >= cap) {
                cap = cap ? cap * 2 : 8;
                arr = (char **)realloc(arr, cap * sizeof(char *));
                if (!arr) { FindClose(h); return -1; }
            }
            arr[count++] = strdup(fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(store->buckets_dir);
    if (!d) { *names_out = NULL; *count_out = 0; return 0; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char bucket_dir[2048];
        snprintf(bucket_dir, sizeof(bucket_dir), "%s/%s",
                 store->buckets_dir, ent->d_name);

        STAT_ST st;
        if (STAT_FN(bucket_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char meta_p[2048];
        snprintf(meta_p, sizeof(meta_p), "%s/%s", bucket_dir, BUCKET_META_FILE);
        if (!file_exists(meta_p)) continue;

        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            arr = (char **)realloc(arr, cap * sizeof(char *));
            if (!arr) { closedir(d); return -1; }
        }
        arr[count++] = strdup(ent->d_name);
    }
    closedir(d);
#endif

    *names_out = arr;
    *count_out = count;
    return 0;
}

/* -------------------------------------------------------------------------
 * Manifest path helper
 * ---------------------------------------------------------------------- */

void store_manifest_path(const Store *store,
                         const char *bucket, const char *key,
                         char *buf, size_t buf_sz) {
    snprintf(buf, buf_sz, "%s/%s/%s%s",
             store->buckets_dir, bucket, key, MANIFEST_EXT);
}

/* -------------------------------------------------------------------------
 * Object PUT — CDC + chunk store + manifest
 * ---------------------------------------------------------------------- */

/**
 * Context passed to the CDC callback during an object_put operation.
 */
typedef struct PutCtx {
    StoragePool *pool;        /**< Physical pool to write chunks into. */
    Manifest    *manifest;    /**< Manifest being built. */
    int          error;       /**< Set to non-zero on first error. */
} PutCtx;

/** CDC callback: receives each chunk, writes it to the pool, updates manifest. */
static int put_chunk_callback(const ChunkInfo *info, void *user_data) {
    PutCtx *ctx = (PutCtx *)user_data;
    if (ctx->error) return -1;

    char hex[SHA256_HEX_SIZE];
    int rc = storage_chunk_write(ctx->pool, info->data, info->size, hex);
    if (rc != 0) {
        fprintf(stderr, "[put] Failed to write chunk at offset %llu\n",
                (unsigned long long)info->offset);
        ctx->error = rc;
        return rc;
    }

    rc = manifest_add_chunk(ctx->manifest, hex, info->offset, (uint32_t)info->size);
    if (rc != 0) {
        ctx->error = rc;
        return rc;
    }

    return 0;
}

int object_put(Store *store, const char *bucket, const char *key,
               const char *src_path, const char *content_type) {
    if (!store || !bucket || !key || !src_path) return -1;

    FILE *f = fopen(src_path, "rb");
    if (!f) {
        fprintf(stderr, "[put] Cannot open source file '%s': %s\n",
                src_path, strerror(errno));
        return -1;
    }

    /* Stream the file through the chunker. */
    Manifest *m = manifest_create(key, content_type);
    if (!m) { fclose(f); return -1; }

    PutCtx ctx = { store->pool, m, 0 };
    ChunkerCtx *chunker = chunker_create(&store->cdc_cfg,
                                          put_chunk_callback, &ctx);
    if (!chunker) { manifest_free(m); fclose(f); return -1; }

    uint8_t buf[65536];  /* 64 KiB read buffer */
    size_t  n;
    while (!ctx.error && (n = fread(buf, 1, sizeof(buf), f)) > 0)
        chunker_feed(chunker, buf, n);

    if (!ctx.error)
        chunker_flush(chunker);

    chunker_stats(chunker, key);
    chunker_destroy(chunker);
    fclose(f);

    if (ctx.error) {
        manifest_free(m);
        return ctx.error;
    }

    /* Write the manifest to disk. */
    char manifest_path[2048];
    store_manifest_path(store, bucket, key, manifest_path, sizeof(manifest_path));

    int rc = manifest_write(m, manifest_path);
    manifest_free(m);

    if (rc != 0) {
        fprintf(stderr, "[put] Failed to write manifest for '%s/%s'\n",
                bucket, key);
        return rc;
    }

    /* Flush pool state after a successful PUT. */
    storage_flush(store->pool);
    return 0;
}

int object_put_buffer(Store *store, const char *bucket, const char *key,
                      const uint8_t *data, size_t size,
                      const char *content_type) {
    if (!store || !bucket || !key || !data) return -1;

    Manifest *m = manifest_create(key, content_type);
    if (!m) return -1;

    PutCtx ctx = { store->pool, m, 0 };
    ChunkerCtx *chunker = chunker_create(&store->cdc_cfg,
                                          put_chunk_callback, &ctx);
    if (!chunker) { manifest_free(m); return -1; }

    chunker_feed(chunker, data, size);
    chunker_flush(chunker);
    chunker_destroy(chunker);

    if (ctx.error) { manifest_free(m); return ctx.error; }

    char manifest_path[2048];
    store_manifest_path(store, bucket, key, manifest_path, sizeof(manifest_path));

    int rc = manifest_write(m, manifest_path);
    manifest_free(m);
    if (rc == 0) storage_flush(store->pool);
    return rc;
}

/* -------------------------------------------------------------------------
 * Object GET — manifest → chunk fetch → reconstruct
 * ---------------------------------------------------------------------- */

int object_get(const Store *store, const char *bucket, const char *key,
               const char *dst_path) {
    if (!store || !bucket || !key || !dst_path) return -1;

    char manifest_path[2048];
    store_manifest_path(store, bucket, key, manifest_path, sizeof(manifest_path));

    Manifest *m = manifest_read(manifest_path);
    if (!m) {
        fprintf(stderr, "[get] Manifest not found for '%s/%s'\n", bucket, key);
        return -1;
    }

    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        fprintf(stderr, "[get] Cannot open destination '%s': %s\n",
                dst_path, strerror(errno));
        manifest_free(m);
        return -1;
    }

    int rc = 0;
    for (uint64_t i = 0; i < m->header.num_chunks && rc == 0; i++) {
        const ManifestEntry *e = &m->entries[i];

        uint8_t *chunk_data = NULL;
        size_t   chunk_size = 0;

        rc = storage_chunk_read_alloc(store->pool, e->hex,
                                       &chunk_data, &chunk_size);
        if (rc != 0) {
            fprintf(stderr, "[get] Missing chunk %.16s... for '%s/%s'\n",
                    e->hex, bucket, key);
            break;
        }

        if (fwrite(chunk_data, 1, chunk_size, out) != chunk_size) {
            fprintf(stderr, "[get] Write error at chunk %llu\n",
                    (unsigned long long)i);
            free(chunk_data);
            rc = -1;
            break;
        }

        free(chunk_data);
    }

    fclose(out);
    manifest_free(m);
    return rc;
}

int object_get_buffer(const Store *store, const char *bucket, const char *key,
                      uint8_t **buf_out, size_t *size_out) {
    if (!store || !bucket || !key || !buf_out || !size_out) return -1;

    char manifest_path[2048];
    store_manifest_path(store, bucket, key, manifest_path, sizeof(manifest_path));

    Manifest *m = manifest_read(manifest_path);
    if (!m) return -1;

    size_t total = (size_t)m->header.object_size;
    uint8_t *buf = (uint8_t *)malloc(total + 1);
    if (!buf) { manifest_free(m); return -1; }

    size_t offset = 0;
    int    rc     = 0;

    for (uint64_t i = 0; i < m->header.num_chunks && rc == 0; i++) {
        const ManifestEntry *e = &m->entries[i];
        uint8_t *chunk_data = NULL;
        size_t   chunk_size = 0;

        rc = storage_chunk_read_alloc(store->pool, e->hex,
                                       &chunk_data, &chunk_size);
        if (rc != 0) break;

        if (offset + chunk_size > total + 1) {
            free(chunk_data); rc = -1; break;
        }

        memcpy(buf + offset, chunk_data, chunk_size);
        offset += chunk_size;
        free(chunk_data);
    }

    manifest_free(m);

    if (rc != 0) { free(buf); return rc; }

    buf[offset] = '\0';  /* NUL-terminate for text convenience. */
    *buf_out  = buf;
    *size_out = offset;
    return 0;
}

/* -------------------------------------------------------------------------
 * Object DELETE — manifest removal + refcount GC
 * ---------------------------------------------------------------------- */

int object_delete(Store *store, const char *bucket, const char *key) {
    if (!store || !bucket || !key) return -1;

    char manifest_path[2048];
    store_manifest_path(store, bucket, key, manifest_path, sizeof(manifest_path));

    Manifest *m = manifest_read(manifest_path);
    if (!m) {
        fprintf(stderr, "[delete] Object '%s/%s' not found\n", bucket, key);
        return -1;
    }

    /* Decrement refcounts for every chunk referenced by this manifest.
     * Chunks whose refcount hits zero are automatically GC'd inside
     * storage_chunk_decref(). */
    int rc = 0;
    for (uint64_t i = 0; i < m->header.num_chunks; i++) {
        if (storage_chunk_decref(store->pool, m->entries[i].hex) != 0) {
            fprintf(stderr, "[delete] Warning: missing chunk %.16s...\n",
                    m->entries[i].hex);
            /* Continue anyway — don't leave a dangling manifest. */
        }
    }

    manifest_free(m);

    /* Remove the manifest file. */
    if (remove(manifest_path) != 0) {
        fprintf(stderr, "[delete] Failed to remove manifest '%s': %s\n",
                manifest_path, strerror(errno));
        rc = -1;
    }

    /* Persist pool state. */
    storage_flush(store->pool);
    return rc;
}

/* -------------------------------------------------------------------------
 * Object EXISTS / INFO
 * ---------------------------------------------------------------------- */

bool object_exists(const Store *store, const char *bucket, const char *key) {
    if (!store || !bucket || !key) return false;
    char manifest_path[2048];
    store_manifest_path(store, bucket, key, manifest_path, sizeof(manifest_path));
    return file_exists(manifest_path);
}

int object_info(const Store *store, const char *bucket, const char *key,
                ObjectInfo *info) {
    if (!store || !bucket || !key || !info) return -1;

    char manifest_path[2048];
    store_manifest_path(store, bucket, key, manifest_path, sizeof(manifest_path));

    Manifest *m = manifest_read(manifest_path);
    if (!m) return -1;

    memset(info, 0, sizeof(*info));
    snprintf(info->key,          sizeof(info->key),          "%s", key);
    snprintf(info->content_type, sizeof(info->content_type),
             "%s", m->header.content_type);
    info->size       = m->header.object_size;
    info->created_at = (time_t)m->header.created_at;

    manifest_free(m);
    return 0;
}

/* -------------------------------------------------------------------------
 * Object LIST
 * ---------------------------------------------------------------------- */

int object_list(const Store *store, const char *bucket, ListResult *result) {
    if (!store || !bucket || !result) return -1;

    memset(result, 0, sizeof(*result));

    char bucket_dir[2048];
    snprintf(bucket_dir, sizeof(bucket_dir), "%s/%s",
             store->buckets_dir, bucket);

    if (!dir_exists(bucket_dir)) {
        fprintf(stderr, "[list] Bucket '%s' not found\n", bucket);
        return -1;
    }

#ifdef _WIN32
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*" MANIFEST_EXT, bucket_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        /* Strip the .manifest extension to recover the object key. */
        char key_buf[OBJECT_KEY_MAX + 1];
        snprintf(key_buf, sizeof(key_buf), "%s", fd.cFileName);
        size_t klen = strlen(key_buf);
        size_t elen = strlen(MANIFEST_EXT);
        if (klen > elen) key_buf[klen - elen] = '\0';

        /* Read the manifest. */
        char mpath[2048];
        snprintf(mpath, sizeof(mpath), "%s\\%s", bucket_dir, fd.cFileName);
        Manifest *m = manifest_read(mpath);
        if (!m) continue;

        /* Grow the result array. */
        if (result->count >= result->capacity) {
            size_t new_cap = result->capacity ? result->capacity * 2 : 8;
            ObjectInfo *ni = (ObjectInfo *)realloc(
                result->items, new_cap * sizeof(ObjectInfo));
            if (!ni) { manifest_free(m); FindClose(h); return -1; }
            result->items    = ni;
            result->capacity = new_cap;
        }

        ObjectInfo *info = &result->items[result->count++];
        snprintf(info->key,          sizeof(info->key),          "%s", key_buf);
        snprintf(info->content_type, sizeof(info->content_type),
                 "%s", m->header.content_type);
        info->size       = m->header.object_size;
        info->created_at = (time_t)m->header.created_at;

        manifest_free(m);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

#else
    DIR *d = opendir(bucket_dir);
    if (!d) return -1;

    size_t ext_len = strlen(MANIFEST_EXT);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= ext_len) continue;
        if (strcmp(ent->d_name + nlen - ext_len, MANIFEST_EXT) != 0) continue;

        char key_buf[OBJECT_KEY_MAX + 1];
        snprintf(key_buf, sizeof(key_buf), "%s", ent->d_name);
        key_buf[nlen - ext_len] = '\0';

        char mpath[2048];
        snprintf(mpath, sizeof(mpath), "%s/%s", bucket_dir, ent->d_name);
        Manifest *m = manifest_read(mpath);
        if (!m) continue;

        if (result->count >= result->capacity) {
            size_t new_cap = result->capacity ? result->capacity * 2 : 8;
            ObjectInfo *ni = (ObjectInfo *)realloc(
                result->items, new_cap * sizeof(ObjectInfo));
            if (!ni) { manifest_free(m); closedir(d); return -1; }
            result->items    = ni;
            result->capacity = new_cap;
        }

        ObjectInfo *info = &result->items[result->count++];
        snprintf(info->key,          sizeof(info->key),          "%s", key_buf);
        snprintf(info->content_type, sizeof(info->content_type),
                 "%s", m->header.content_type);
        info->size       = m->header.object_size;
        info->created_at = (time_t)m->header.created_at;

        manifest_free(m);
    }
    closedir(d);
#endif

    return 0;
}

void list_result_free(ListResult *result) {
    if (!result) return;
    free(result->items);
    result->items    = NULL;
    result->count    = 0;
    result->capacity = 0;
}
