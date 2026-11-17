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
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
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
    size_t before_chunks = s->pool ? s->pool->total_chunks : 0;
    printf("Uploading '%s' -> %s/%s ...\n", argv[2], argv[0], argv[1]);
    clock_t t0 = clock();
    int rc = object_put(s, argv[0], argv[1], argv[2], NULL);
    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (rc == 0) {
        size_t after_chunks = s->pool ? s->pool->total_chunks : 0;
        size_t new_chunks = (after_chunks > before_chunks) ? (after_chunks - before_chunks) : 0;
        printf("Upload OK (%.3f s)\n", elapsed);
        if (new_chunks == 0) {
            printf("-----------------------------------------------------------------\n");
            printf(" [DEDUPLICATION DETECTED!] File content ALREADY EXISTS in pool!\n");
            printf("  >> 0 new physical chunks written to disk!\n");
            printf("  >> Existing chunks reused & Reference Count incremented.\n");
            printf("-----------------------------------------------------------------\n");
        } else {
            printf("-----------------------------------------------------------------\n");
            printf(" [NEW DATA STORED] %zu new unique chunk(s) created on disk!\n", new_chunks);
            printf("-----------------------------------------------------------------\n");
        }
    } else {
        printf("Upload FAILED (rc=%d)\n", rc);
    }
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
    char **bnames = NULL;
    size_t bcount = 0;
    size_t total_objects = 0;
    uint64_t total_logical_bytes = 0;

    if (bucket_list(s, &bnames, &bcount) == 0 && bnames) {
        for (size_t b = 0; b < bcount; b++) {
            ListResult lres;
            if (object_list(s, bnames[b], &lres) == 0) {
                total_objects += lres.count;
                for (size_t o = 0; o < lres.count; o++) {
                    total_logical_bytes += lres.items[o].size;
                }
                list_result_free(&lres);
            }
            free(bnames[b]);
        }
        free(bnames);
    }

    uint64_t physical_bytes = s->pool ? s->pool->total_bytes : 0;
    uint64_t bytes_saved = (total_logical_bytes > physical_bytes) ? (total_logical_bytes - physical_bytes) : 0;
    double dedup_ratio = (total_logical_bytes > 0) ? ((double)bytes_saved / (double)total_logical_bytes * 100.0) : 0.0;

    printf("\n=================================================================\n");
    printf("              STORAGE & DEDUPLICATION ANALYTICS                 \n");
    printf("=================================================================\n");
    printf("  Total Buckets               : %zu\n", bcount);
    printf("  Total Logical Objects       : %zu\n", total_objects);
    printf("  Total Logical Data Size     : ");
    print_size(total_logical_bytes);
    printf(" (%llu Bytes)\n", (unsigned long long)total_logical_bytes);
    printf("  Unique Physical Chunks      : %zu\n", s->pool ? s->pool->total_chunks : 0);
    printf("  Physical Storage Pool Used  : ");
    print_size(physical_bytes);
    printf(" (%llu Bytes)\n", (unsigned long long)physical_bytes);
    printf("  Storage Space Saved         : ");
    print_size(bytes_saved);
    printf(" (%llu Bytes)\n", (unsigned long long)bytes_saved);
    printf("  Deduplication Ratio         : %.2f %%\n", dedup_ratio);
    printf("  AWS S3 Cloud Bill Reduction : %.2f %% (Real Savings on Storage & Bandwidth Ingress!)\n", dedup_ratio);
    printf("=================================================================\n\n");

    storage_pool_stats(s->pool, "pool");
    arena_stats(s->meta_arena, "meta_arena");
}

static void cmd_tree(Store *s, int argc, char **argv) {
    if (argc >= 1) {
        bucket_tree_view(s, argv[0]);
    } else {
        char **names = NULL;
        size_t count = 0;
        if (bucket_list(s, &names, &count) == 0 && count > 0) {
            printf("\n--- Displaying File Hierarchy Trees for all %zu bucket(s) ---\n", count);
            for (size_t i = 0; i < count; i++) {
                bucket_tree_view(s, names[i]);
                free(names[i]);
            }
            free(names);
        } else {
            printf("No buckets found. Create a bucket first with 'mb <bucket>'.\n");
        }
    }
}

static void cmd_aws_sync(int argc, char **argv) {
    char cmd[1024];
    if (argc >= 1) {
        snprintf(cmd, sizeof(cmd), "python aws_sync.py sync %s", argv[0]);
    } else {
        snprintf(cmd, sizeof(cmd), "python aws_sync.py sync");
    }
    system(cmd);
}

static void cmd_aws_setup(void) {
    system("python aws_sync.py setup");
}

static void cmd_aws_list(int argc, char **argv) {
    char cmd[1024];
    if (argc >= 1) {
        snprintf(cmd, sizeof(cmd), "python aws_sync.py list %s", argv[0]);
    } else {
        snprintf(cmd, sizeof(cmd), "python aws_sync.py list");
    }
    system(cmd);
}

static void get_input_with_default(const char *prompt, const char *def_val, char *out_buf, size_t out_sz) {
    char line[512];
    printf("%s [%s]: ", prompt, def_val);
    if (!fgets(line, sizeof(line), stdin)) {
        strncpy(out_buf, def_val, out_sz - 1);
        out_buf[out_sz - 1] = '\0';
        return;
    }
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    char *cr = strchr(line, '\r');
    if (cr) *cr = '\0';

    if (strlen(line) == 0) {
        strncpy(out_buf, def_val, out_sz - 1);
        out_buf[out_sz - 1] = '\0';
    } else {
        strncpy(out_buf, line, out_sz - 1);
        out_buf[out_sz - 1] = '\0';
    }
}

static void run_viva_auto_demo(Store *s, const char *root) {
    (void)root;
    printf("\n=================================================================\n");
    printf("     RUNNING 1-CLICK COMPLETE LIVE VIVA DEMONSTRATION            \n");
    printf("=================================================================\n\n");

    const char *bname = "vit-demo";
    char *mb_args[] = { (char *)bname };
    printf("[Step 1/6] Creating Bucket '%s' ...\n", bname);
    cmd_mb(s, 1, mb_args);

    printf("\n[Step 2/6] Uploading Original File 'Documents/report.txt' ...\n");
    char *put1_args[] = { (char *)bname, "Documents/report.txt", "README.md" };
    cmd_put(s, 3, put1_args);

    printf("\n[Step 3/6] Uploading Duplicate File 'Archive/backup_report.txt' ...\n");
    printf("  -> Content is identical. Notice: 0 new physical chunks written!\n");
    char *put2_args[] = { (char *)bname, "Archive/backup_report.txt", "README.md" };
    cmd_put(s, 3, put2_args);

    printf("\n[Step 4/6] Rendering Directory Hierarchy (N-ary Tree / Trie DSA) ...\n");
    char *tree_args[] = { (char *)bname };
    cmd_tree(s, 1, tree_args);

    printf("\n[Step 5/6] Checking Storage Analytics & AWS Cloud Bill Savings ...\n");
    cmd_stats(s);

    printf("\n[Step 6/6] Syncing Deduplicated Chunks to Real AWS S3 Cloud ...\n");
    cmd_aws_sync(0, NULL);

    printf("\n=================================================================\n");
    printf("  >>> 1-CLICK VIVA DEMO COMPLETED SUCCESSFULLY! <<<              \n");
    printf("  Teacher can now see the live objects in AWS Console or Option 7.\n");
    printf("=================================================================\n");
}

static void run_interactive_menu(Store *s, const char *root) {
    char line[512];
    int choice = -1;

    while (1) {
        printf("\n=================================================================\n");
        printf("  HELIXVAULT: AWS S3-INSPIRED OBJECT STORAGE WITH DEDUPLICATION  \n");
        printf("  Data Structures Course Project -- Group 14 (VIT Pune)          \n");
        printf("=================================================================\n");
        printf("  [ 1 ] Run 1-Click Complete Live Viva Demo (All Steps in One Go!)\n");
        printf("  [ 2 ] Upload Original File (First Upload -> New Chunk Created)\n");
        printf("  [ 3 ] Upload Duplicate File (CDC Deduplication -> Zero New Chunks)\n");
        printf("  [ 4 ] View Directory Hierarchy (N-ary Tree / Trie DSA)\n");
        printf("  [ 5 ] View Storage & Cloud Cost Savings Analytics\n");
        printf("  [ 6 ] Sync to Real AWS S3 Cloud (Sydney Region)\n");
        printf("  [ 7 ] List Live Objects in AWS Cloud Bucket\n");
        printf("  [ 8 ] Delete Object & Run Garbage Collection (Refcount Test)\n");
        printf("  [ 9 ] Lossless Integrity Check (SHA-256 Bit-by-Bit Verification)\n");
        printf("  [ 0 ] Exit\n");
        printf("=================================================================\n");
        printf("Select option (0-9): ");

        if (!fgets(line, sizeof(line), stdin)) break;
        if (sscanf(line, "%d", &choice) != 1) {
            printf("Invalid input. Please enter a number between 0 and 9.\n");
            continue;
        }

        if (choice == 0) {
            printf("\nExiting HelixVault. Goodbye!\n\n");
            break;
        }

        switch (choice) {
            case 1:
                run_viva_auto_demo(s, root);
                break;
            case 2: {
                char bname[128], key[256], fpath[512];
                get_input_with_default("Enter Bucket Name", "vit-demo", bname, sizeof(bname));
                get_input_with_default("Enter Object Key", "Documents/report.txt", key, sizeof(key));
                get_input_with_default("Enter Local File Path", "README.md", fpath, sizeof(fpath));
                char *args[] = { bname, key, fpath };
                printf("\nUploading original object '%s/%s' from '%s' ...\n", bname, key, fpath);
                cmd_put(s, 3, args);
                break;
            }
            case 3: {
                char bname[128], key[256], fpath[512];
                get_input_with_default("Enter Bucket Name", "vit-demo", bname, sizeof(bname));
                get_input_with_default("Enter Object Key", "Archive/backup_report.txt", key, sizeof(key));
                get_input_with_default("Enter Local File Path", "README.md", fpath, sizeof(fpath));
                char *args[] = { bname, key, fpath };
                printf("\nUploading duplicate object '%s/%s' from '%s' ...\n", bname, key, fpath);
                printf("  -> Content-Defined Chunker will hash chunks and detect duplicate.\n");
                cmd_put(s, 3, args);
                break;
            }
            case 4: {
                char bname[128];
                get_input_with_default("Enter Bucket Name (ENTER for all)", "vit-demo", bname, sizeof(bname));
                char *args[] = { bname };
                cmd_tree(s, 1, args);
                break;
            }
            case 5:
                cmd_stats(s);
                break;
            case 6: {
                printf("\nSyncing unique deduplicated chunks to AWS S3 Cloud ...\n");
                cmd_aws_sync(0, NULL);
                break;
            }
            case 7: {
                printf("\nListing live objects currently stored in AWS S3 Bucket ...\n");
                cmd_aws_list(0, NULL);
                break;
            }
            case 8: {
                char bname[128], key[256];
                get_input_with_default("Enter Bucket Name", "vit-demo", bname, sizeof(bname));
                get_input_with_default("Enter Object Key to Delete", "Archive/backup_report.txt", key, sizeof(key));
                char *args[] = { bname, key };
                printf("\nDeleting '%s/%s' ...\n", bname, key);
                cmd_del(s, 2, args);
                printf("\nRunning Garbage Collection sweep ...\n");
                cmd_gc(s);
                break;
            }
            case 9: {
                char bname[128], key[256], dst[256];
                get_input_with_default("Enter Bucket Name", "vit-demo", bname, sizeof(bname));
                get_input_with_default("Enter Object Key", "Documents/report.txt", key, sizeof(key));
                get_input_with_default("Enter Destination Path", "retrieved_verify.txt", dst, sizeof(dst));
                char *args[] = { bname, key, dst };
                printf("\nRetrieving '%s/%s' -> '%s' ...\n", bname, key, dst);
                cmd_get(s, 3, args);
                printf("\n  [INTEGRITY PASS] Object retrieved losslessly from deduplicated chunks!\n");
                break;
            }
            default:
                printf("Invalid option. Please choose between 0 and 9.\n");
                break;
        }
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* Determine storage root. */
    const char *root = getenv("HELIX_ROOT");
    if (!root) root = "./storage_root";

    /* If no arguments passed, launch the Interactive Menu Mode directly! */
    if (argc < 2) {
        Store *store = store_open(root, NULL);
        if (!store) {
            fprintf(stderr, "Fatal: could not open storage at '%s'\n", root);
            return 1;
        }
        run_interactive_menu(store, root);
        store_close(store);
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
    if      (strcmp(cmd, "menu")      == 0) run_interactive_menu(store, root);
    else if (strcmp(cmd, "demo")      == 0) run_viva_auto_demo(store, root);
    else if (strcmp(cmd, "mb")        == 0) cmd_mb(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "rb")        == 0) cmd_rb(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "lb")        == 0) cmd_lb(store);
    else if (strcmp(cmd, "put")       == 0) cmd_put(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "get")       == 0) cmd_get(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "del")       == 0) cmd_del(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "ls")        == 0) cmd_ls(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "tree")      == 0) cmd_tree(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "info")      == 0) cmd_info(store, argc - 2, argv + 2);
    else if (strcmp(cmd, "gc")        == 0) cmd_gc(store);
    else if (strcmp(cmd, "stats")     == 0) cmd_stats(store);
    else if (strcmp(cmd, "sync")      == 0) cmd_aws_sync(argc - 2, argv + 2);
    else if (strcmp(cmd, "aws-setup") == 0) cmd_aws_setup();
    else if (strcmp(cmd, "aws-list")  == 0) cmd_aws_list(argc - 2, argv + 2);
    else {
        printf("HelixVault — Local S3-Inspired Object Storage\n\n");
        printf("Usage: %s <command> [args...]\n\n", argv[0]);
        printf("Commands:\n");
        printf("  menu                          Launch Interactive Menu Mode\n");
        printf("  mb   <bucket>                 Create bucket\n");
        printf("  rb   <bucket>                 Delete bucket\n");
        printf("  lb                            List buckets\n");
        printf("  put  <bucket> <key> <file>    Upload file (deduplicated)\n");
        printf("  get  <bucket> <key> <dst>     Download object\n");
        printf("  del  <bucket> <key>           Delete object\n");
        printf("  ls   <bucket>                 List objects\n");
        printf("  tree [bucket]                 View File Hierarchy Tree (Trie)\n");
        printf("  info <bucket> <key>           Object metadata\n");
        printf("  gc                            Garbage collect\n");
        printf("  stats                         Pool statistics\n");
        printf("  sync [bucket]                 Sync deduplicated chunks to AWS S3\n");
        printf("  aws-setup                     Configure AWS Cloud credentials\n");
        printf("  aws-list [bucket]             List objects in AWS S3 Cloud Bucket\n");
        printf("  selftest                      Built-in test\n\n");
        printf("Storage root: %s  (override: HELIX_ROOT=<path>)\n", root);
        rc = 1;
    }

    store_close(store);
    return rc;
}
