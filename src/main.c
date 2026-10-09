/**
 * main.c — HelixVault CLI Entry Point
 *
 * A minimal command-line interface demonstrating all storage operations.
 *
 * Usage:
 *   helixvault <command> [args...]
 *
 * Commands:
 *   mb  <bucket>                     Create a bucket
 *   rb  <bucket>                     Delete a bucket
 *   lb                               List all buckets
 *   put <bucket> <key> <file>        Upload a file
 *   get <bucket> <key> <dst>         Download an object
 *   del <bucket> <key>               Delete an object
 *   ls  <bucket>                     List objects in a bucket
 *   info <bucket> <key>              Show object metadata
 *   gc                               Run garbage collection sweep
 *   stats                            Show pool statistics
 *   selftest                         Run built-in correctness self-test
 *
 * Storage root defaults to "./storage_root" (override with HELIX_ROOT env var).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "bucket.h"
#include "chunker.h"
#include "sha256.h"
#include "arena.h"

/* -------------------------------------------------------------------------
 * Helper: print a human-readable file size
 * ---------------------------------------------------------------------- */
static void print_size(uint64_t bytes) {
    if      (bytes >= 1024ULL*1024*1024) printf("%.2f GiB", bytes / (1024.0*1024*1024));
    else if (bytes >= 1024ULL*1024)      printf("%.2f MiB", bytes / (1024.0*1024));
    else if (bytes >= 1024ULL)           printf("%.2f KiB", bytes / 1024.0);
    else                                 printf("%llu B", (unsigned long long)bytes);
}

/* -------------------------------------------------------------------------
 * Self-test: SHA-256, CDC chunker, full round-trip
 * ---------------------------------------------------------------------- */

static void test_sha256(void) {
    printf("  [sha256] Testing known vectors...\n");

    /* RFC 4634 test vector: SHA-256("abc") */
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_hash("abc", 3, digest);
    char hex[SHA256_HEX_SIZE];
    sha256_hex(digest, hex);
    const char *expected =
        "ba7816bf8f01cfea414140de5dae2ec73b00361bbef0469348423f656b0f5d3";
    if (strcmp(hex, expected) == 0)
        printf("  [sha256] PASS  'abc' -> %s\n", hex);
    else
        printf("  [sha256] FAIL! Got %s, expected %s\n", hex, expected);

    /* Empty string. */
    sha256_hash("", 0, digest);
    sha256_hex(digest, hex);
    const char *empty_expected =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    if (strcmp(hex, empty_expected) == 0)
        printf("  [sha256] PASS  '' -> %s\n", hex);
    else
        printf("  [sha256] FAIL! Got %s, expected %s\n", hex, empty_expected);
}

/* Count chunks during self-test. */
static size_t st_chunk_count = 0;
static int selftest_callback(const ChunkInfo *info, void *ud) {
    (void)ud;
    st_chunk_count++;
    printf("  [cdc] Chunk #%zu  offset=%-8llu size=%zu\n",
           st_chunk_count,
           (unsigned long long)info->offset,
           info->size);
    return 0;
}

static void test_cdc(void) {
    printf("  [cdc] Testing chunker with synthetic data...\n");
    st_chunk_count = 0;

    /* Generate 2 MiB of pseudo-random-ish data with repeated 64-KiB blocks
     * to exercise deduplication. */
    size_t block  = 64 * 1024;
    size_t total  = 2 * 1024 * 1024;
    uint8_t *data = (uint8_t *)malloc(total);
    if (!data) { printf("  [cdc] OOM\n"); return; }

    /* First block: pseudo-random. */
    uint32_t seed = 0xDEADBEEF;
    for (size_t i = 0; i < block; i++) {
        seed = seed * 1664525u + 1013904223u;
        data[i] = (uint8_t)(seed >> 16);
    }
    /* Remaining blocks: alternate between the first block and its inverse
     * to create real duplication patterns. */
    for (size_t off = block; off < total; off += block) {
        size_t sz = (off + block <= total) ? block : (total - off);
        if ((off / block) % 2 == 0)
            memcpy(data + off, data, sz);
        else
            for (size_t k = 0; k < sz; k++) data[off+k] = ~data[k];
    }

    CdcConfig cfg = { 2*1024, 8*1024, 64*1024 };
    ChunkerCtx *ctx = chunker_create(&cfg, selftest_callback, NULL);
    chunker_feed(ctx, data, total);
    chunker_flush(ctx);
    chunker_stats(ctx, "selftest_cdc");
    chunker_destroy(ctx);
    free(data);

    printf("  [cdc] Total chunks emitted: %zu\n", st_chunk_count);
}

static void test_roundtrip(const char *root) {
    printf("  [roundtrip] Full PUT/GET/DELETE test in '%s'...\n", root);

    Store *store = store_open(root, NULL);
    if (!store) { printf("  [roundtrip] Failed to open store\n"); return; }

    /* Create a bucket. */
    bucket_create(store, "test-bucket");

    /* Create a test payload. */
    const char payload[] =
        "HelixVault roundtrip test data.\n"
        "This content will be chunked, deduplicated, stored, and retrieved.\n"
        "If you read this back correctly, the system works end-to-end!\n";

    /* Upload. */
    int rc = object_put_buffer(store, "test-bucket", "hello.txt",
                               (const uint8_t *)payload, strlen(payload),
                               "text/plain");
    printf("  [roundtrip] PUT: %s\n", rc == 0 ? "OK" : "FAILED");

    /* Retrieve. */
    uint8_t *retrieved = NULL;
    size_t   retrieved_sz = 0;
    rc = object_get_buffer(store, "test-bucket", "hello.txt",
                           &retrieved, &retrieved_sz);
    printf("  [roundtrip] GET: %s (size=%zu)\n",
           rc == 0 ? "OK" : "FAILED", retrieved_sz);

    if (rc == 0 && retrieved) {
        if (retrieved_sz == strlen(payload) &&
            memcmp(retrieved, payload, retrieved_sz) == 0)
            printf("  [roundtrip] Content match: PASS\n");
        else
            printf("  [roundtrip] Content match: FAIL\n");
        free(retrieved);
    }

    /* Verify deduplication: upload the same content under a second key. */
    rc = object_put_buffer(store, "test-bucket", "hello-copy.txt",
                           (const uint8_t *)payload, strlen(payload),
                           "text/plain");
    printf("  [roundtrip] PUT (dedup): %s\n", rc == 0 ? "OK" : "FAILED");

    /* Show pool stats: should see same total_chunks as before (dedup working). */
    storage_pool_stats(store->pool, "after_dedup_put");

    /* Delete the first object. */
    rc = object_delete(store, "test-bucket", "hello.txt");
    printf("  [roundtrip] DEL hello.txt: %s\n", rc == 0 ? "OK" : "FAILED");

    /* Pool should still have chunks referenced by the copy. */
    storage_pool_stats(store->pool, "after_del_original");

    /* Delete the copy — should GC all chunks. */
    rc = object_delete(store, "test-bucket", "hello-copy.txt");
    printf("  [roundtrip] DEL hello-copy.txt: %s\n", rc == 0 ? "OK" : "FAILED");

    storage_pool_stats(store->pool, "after_del_copy");

    /* Clean up bucket. */
    bucket_delete(store, "test-bucket");
    store_close(store);
    printf("  [roundtrip] DONE\n");
}

static void run_selftest(const char *root) {
    printf("\n=== HelixVault Self-Test ===\n\n");
    test_sha256();
    printf("\n");
    test_cdc();
    printf("\n");
    test_roundtrip(root);
    printf("\n=== Self-Test Complete ===\n\n");
}

/* -------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------- */

static void cmd_mb(Store *s, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: mb <bucket>\n"); return; }
    int rc = bucket_create(s, argv[0]);
    if (rc == 0)  printf("Bucket '%s' created.\n", argv[0]);
    else if (rc == -1) printf("Bucket '%s' already exists.\n", argv[0]);
    else printf("Error creating bucket '%s'.\n", argv[0]);
}

static void cmd_rb(Store *s, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: rb <bucket>\n"); return; }
    int rc = bucket_delete(s, argv[0]);
    if (rc == 0)   printf("Bucket '%s' deleted.\n", argv[0]);
    else if (rc == -1) printf("Bucket '%s' not found.\n", argv[0]);
    else if (rc == -2) printf("Bucket '%s' is not empty.\n", argv[0]);
    else printf("Error deleting bucket '%s'.\n", argv[0]);
}

static void cmd_lb(Store *s) {
    char  **names = NULL;
    size_t  count = 0;
    if (bucket_list(s, &names, &count) != 0) {
        fprintf(stderr, "Error listing buckets.\n"); return;
    }
    printf("%zu bucket(s):\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  %s\n", names[i]);
        free(names[i]);
    }
    free(names);
}

static void cmd_put(Store *s, int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: put <bucket> <key> <file>\n"); return; }
    if (!bucket_exists(s, argv[0])) {
        fprintf(stderr, "Bucket '%s' does not exist.\n", argv[0]);
        return;
    }
    printf("Uploading '%s' -> %s/%s ...\n", argv[2], argv[0], argv[1]);
    clock_t t0 = clock();
    int rc = object_put(s, argv[0], argv[1], argv[2], NULL);
    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (rc == 0) printf("Upload OK (%.3f s)\n", elapsed);
    else         printf("Upload FAILED (rc=%d)\n", rc);
}

static void cmd_get(Store *s, int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: get <bucket> <key> <dst>\n"); return; }
    printf("Downloading %s/%s -> '%s' ...\n", argv[0], argv[1], argv[2]);
    clock_t t0 = clock();
    int rc = object_get(s, argv[0], argv[1], argv[2]);
    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (rc == 0) printf("Download OK (%.3f s)\n", elapsed);
    else         printf("Download FAILED (rc=%d)\n", rc);
}

static void cmd_del(Store *s, int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: del <bucket> <key>\n"); return; }
    int rc = object_delete(s, argv[0], argv[1]);
    if (rc == 0)  printf("Deleted %s/%s\n", argv[0], argv[1]);
    else          printf("Delete FAILED (rc=%d)\n", rc);
}

static void cmd_ls(Store *s, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: ls <bucket>\n"); return; }
    ListResult r;
    if (object_list(s, argv[0], &r) != 0) {
        fprintf(stderr, "Error listing bucket '%s'\n", argv[0]); return;
    }
    printf("%-50s  %12s  %s\n", "Key", "Size", "Content-Type");
    printf("%-50s  %12s  %s\n",
           "--------------------------------------------------",
           "------------", "--------------------------");
    for (size_t i = 0; i < r.count; i++) {
        const ObjectInfo *o = &r.items[i];
        printf("%-50s  ", o->key);
        print_size(o->size);
        printf("  %s\n", o->content_type);
    }
    printf("\n%zu object(s) in '%s'\n", r.count, argv[0]);
    list_result_free(&r);
}

static void cmd_info(Store *s, int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: info <bucket> <key>\n"); return; }
    ObjectInfo info;
    if (object_info(s, argv[0], argv[1], &info) != 0) {
        printf("Object '%s/%s' not found.\n", argv[0], argv[1]); return;
    }
    printf("Key          : %s\n", info.key);
    printf("Size         : "); print_size(info.size); printf("\n");
    printf("Content-Type : %s\n", info.content_type);

    char ts[64];
    struct tm *tm = localtime(&info.created_at);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    printf("Created      : %s\n", ts);
}

static void cmd_gc(Store *s) {
    size_t deleted = storage_gc_run(s->pool);
    printf("GC: deleted %zu orphan chunk(s).\n", deleted);
    storage_flush(s->pool);
}

static void cmd_stats(Store *s) {
    storage_pool_stats(s->pool, "pool");
    arena_stats(s->meta_arena, "meta_arena");
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* Determine storage root. */
    const char *root = getenv("HELIX_ROOT");
    if (!root) root = "./storage_root";

    if (argc < 2) {
        printf("HelixVault — Local S3-Inspired Object Storage\n\n");
        printf("Usage: %s <command> [args...]\n\n", argv[0]);
        printf("Commands:\n");
        printf("  mb  <bucket>                  Create bucket\n");
        printf("  rb  <bucket>                  Delete bucket\n");
        printf("  lb                            List buckets\n");
        printf("  put <bucket> <key> <file>     Upload file\n");
        printf("  get <bucket> <key> <dst>      Download object\n");
        printf("  del <bucket> <key>            Delete object\n");
        printf("  ls  <bucket>                  List objects\n");
        printf("  info <bucket> <key>           Object metadata\n");
        printf("  gc                            Garbage collect\n");
        printf("  stats                         Pool statistics\n");
        printf("  selftest                      Built-in test\n\n");
        printf("Storage root: %s  (override: HELIX_ROOT=<path>)\n", root);
        return 0;
    }

    const char *cmd = argv[1];

    /* selftest does not require an open store to initialise the root path. */
    if (strcmp(cmd, "selftest") == 0) {
        run_selftest(root);
        return 0;
    }

    /* Open store for all other commands. */
    Store *store = store_open(root, NULL);
    if (!store) {
        fprintf(stderr, "Fatal: could not open storage at '%s'\n", root);
        return 1;
    }

    int rc = 0;
    if      (strcmp(cmd, "mb")    == 0) cmd_mb(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "rb")    == 0) cmd_rb(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "lb")    == 0) cmd_lb(store);
    else if (strcmp(cmd, "put")   == 0) cmd_put(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "get")   == 0) cmd_get(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "del")   == 0) cmd_del(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "ls")    == 0) cmd_ls(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "info")  == 0) cmd_info(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "gc")    == 0) cmd_gc(store);
    else if (strcmp(cmd, "stats") == 0) cmd_stats(store);
    else {
        fprintf(stderr, "Unknown command '%s'. Run without arguments for help.\n", cmd);
        rc = 1;
    }

    store_close(store);
    return rc;
}
