# HelixVault — AWS S3-Inspired Deduplicating Object Storage

> **Department of Information Technology, Vishwakarma Institute of Technology (VIT), Pune**  
> **Course:** Data Structures Course Project (MSE Review)  
> **Group 14:** Kunal Honde (62), Sahil Godage (54), Aditya Gavane (48), Shivam Gunjal (57), Aditi Ghule (51)  
> 
> A high-performance, S3-inspired hybrid cloud object storage backend in pure C (C11) featuring Content-Defined Chunking (FastCDC), SHA-256 cryptographic addressing, N-ary Directory Hierarchy Trees (Trie DSA), Reference-Counted Garbage Collection, and Real AWS S3 Cloud Synchronization.

---

## Key Data Structures & System Architecture

```
helixvault/
├── include/              Header files (Public API)
│   ├── arena.h           Memory pool / slab-chain bump allocator
│   ├── hashtable.h       Open-addressing hash table (linear probing)
│   ├── sha256.h          Portable cryptographic SHA-256 (FIPS 180-4)
│   ├── chunker.h         Content-defined chunking via Gear fingerprinting
│   ├── tree.h            N-ary Directory Hierarchy Tree (First-Child / Next-Sibling Trie)
│   ├── storage.h         Physical chunk pool (dedup + WAL + refcounts + GC)
│   ├── manifest.h        Object manifest (binary, CRC-32 validated)
│   └── bucket.h          Bucket + object operations (PUT/GET/DEL/TREE/STATS)
│
├── src/                  C11 Implementation files
│   ├── arena.c
│   ├── hashtable.c
│   ├── sha256.c
│   ├── chunker.c
│   ├── tree.c            Directory tree parsing, tokenization & ASCII rendering
│   ├── storage.c
│   ├── manifest.c
│   ├── bucket.c
│   └── main.c            Interactive 9-step menu & 1-click viva demo harness
│
├── aws_sync.py           Real AWS S3 Cloud Synchronization (Boto3 SDK)
├── web_ui.py             Python HTTP server bridging Web UI to C11 engine & AWS S3
├── dashboard.html        Authentic AWS S3 Cloudscape Web Dashboard Frontend
├── full_scale_test.py    Automated 7-stage benchmark & system verification suite
├── Makefile              MinGW / GCC build script
├── build_msvc.bat        MSVC compiler build script
└── README.md             System architecture & presentation documentation
```

---

## 🌐 AWS S3-Style Web Dashboard UI

Launch the interactive browser dashboard (connected live to `helixvault.exe` and Real AWS S3):
```powershell
python web_ui.py
```
Then open **`http://localhost:8080`** in Chrome/Edge for drag-and-drop file upload, instant deduplication detection banners, live N-ary Directory Tree visualization, SHA-256 integrity checks, RefCount Garbage Collection, and 1-click Real AWS S3 Cloud synchronization.

---

## ⚡ 1-Click Interactive Viva Demonstration

To launch the interactive menu, simply run without arguments:
```powershell
.\helixvault.exe
```

### Menu Options (0 - 9)
```text
=================================================================
  HELIXVAULT: AWS S3-INSPIRED OBJECT STORAGE WITH DEDUPLICATION  
  Data Structures Course Project — Group 14 (VIT Pune)           
=================================================================
  [ 1 ] ⚡ 1-Click Complete Live Viva Demo (All Steps in One Go!)
  [ 2 ] 📤 Upload Original File (First Upload -> New Chunk Created)
  [ 3 ] 🔄 Upload Duplicate File (CDC Deduplication -> Zero New Chunks)
  [ 4 ] 📁 View Directory Hierarchy (N-ary Tree / Trie DSA)
  [ 5 ] 📊 View Storage & Cloud Cost Savings Analytics
  [ 6 ] ☁️ Sync to Real AWS S3 Cloud (Sydney Region)
  [ 7 ] 🌐 List Live Objects in AWS Cloud Bucket
  [ 8 ] 🗑️ Delete Object & Run Garbage Collection (Refcount Test)
  [ 9 ] 🔍 Lossless Integrity Check (SHA-256 Bit-by-Bit Verification)
  [ 0 ] ❌ Exit
=================================================================
```

Press **`1`** to automatically execute the complete end-to-end lifecycle:
1. Bucket Creation (`vit-demo`)
2. Initial Upload of `Documents/report.txt`
3. Duplicate Upload of `Archive/backup_report.txt` (Zero new chunks written)
4. Rendering the N-ary Directory Tree
5. Computing Deduplication & AWS Cloud Bill Reduction (~60% savings)
6. Synchronizing unique deduplicated chunks to Live AWS S3 Cloud (`sahil-vit-group14-storage`)!

---

## ☁️ Real AWS S3 Cloud Integration

HelixVault features a true hybrid cloud architecture:
- Deduplication runs locally on the client or storage node.
- Only **unique physical chunks** (`chunks/xx/xxxx...`) and metadata manifests are transmitted across the wire to the AWS S3 cloud.
- Drastically reduces AWS S3 Storage and Ingress/Egress bandwidth bills by ~60%.

To test live AWS connection:
```powershell
python aws_sync.py test
```
To list live AWS S3 bucket contents:
```powershell
python aws_sync.py list
```

### On-Disk Layout

```
storage_root/
├── chunks/
│   ├── refcounts.idx    Binary refcount index (WAL-backed)
│   ├── refcounts.wal    Write-ahead log (replayed on startup)
│   └── ab/              Shard directory (first 2 hex chars)
│       └── ab3f7e...    Chunk file (64-char hex SHA-256 = content)
└── buckets/
    └── my-bucket/
        ├── _bucket.meta         Bucket creation metadata
        └── document.pdf.manifest  Object manifest (binary)
```

---

## Key Design Decisions

### 1. Content-Defined Chunking (CDC) — Rabin/Gear Fingerprint

Instead of fixed-size chunks, we use a **sliding window Gear hash** to find
boundaries based on content:

- The hash of the last `RABIN_WINDOW_SIZE` (64) bytes is checked against a mask.
- When `(hash & MASK) == 0` and the chunk is at least `min_chunk_size`, we cut.
- `MASK` is derived from `target_chunk_size` so the *expected* cut frequency
  matches the target.
- Hard limits (`max_chunk_size`) prevent pathological single huge chunks.

This means **identical file regions produce identical chunks even when the file
is prepended or shifted**, unlike fixed-size block chunking.

| Parameter | Default |
|-----------|---------|
| `min_chunk_size` | 2 KiB |
| `target_chunk_size` | 8 KiB |
| `max_chunk_size` | 64 KiB |

### 2. Physical Chunk Pool — Content Addressing + Deduplication

Every chunk is named by the **SHA-256 hex digest of its bytes**.  
If two objects share a byte sequence of that length, the chunk is stored **once**
and its reference count is incremented.

```
chunk file: chunks/ab/ab3f7e8c... (binary data)
refcount:   3 (referenced by 3 manifests)
```

### 3. Manifest Layer

A manifest is a **binary file** listing chunks in order:

```
[ManifestHeader]   — magic, version, total size, num chunks, CRC-32, key, MIME
[ManifestEntry×N]  — chunk hex + byte offset + chunk size  (per chunk)
```

The CRC-32 over the entry array guards against disk corruption.  
Manifests are written atomically via `write + rename`.

### 4. Reference-Counted Garbage Collection

- **PUT**: for each chunk, if new → write + refcount=1; if exists → refcount++.
- **DELETE**: for each chunk in the manifest, refcount--. If hits 0 → `remove()`.

No separate GC pass is needed for routine operation.  
`storage_gc_run()` provides a **safety sweep** for crash-recovery edge cases.

### 5. Write-Ahead Log (WAL)

Every refcount mutation is appended to `refcounts.wal` **before** it is applied.  
On startup, an incomplete WAL is replayed to restore consistency.  
After a successful `storage_flush()`, the WAL is removed.

Each WAL record contains:
- 4-byte magic (`WALX`)
- 1-byte operation code (`INCREF | DECREF | DELETE`)
- 65-byte chunk hex
- 4-byte new refcount
- 4-byte CRC-32 (integrity validation during replay)

### 6. Hash Table

Open-addressing with **FNV-1a 64-bit** hash and **linear probing**.  
Tombstone slots handle deletions without breaking probe chains.  
Automatic rehash at **75% load factor** with doubling capacity.  
Optional arena integration for key-string storage.

### 7. Arena Allocator

A slab-chaining bump-pointer allocator:
- Hands out aligned, zeroed memory from large pre-allocated slabs.
- New slab allocated transparently on exhaustion.
- `O(1)` allocation in the fast path (pointer bump).
- `O(n_slabs)` free — frees the entire arena in one walk.

---

## Building

### Option A — GCC (Linux, macOS, MSYS2/MinGW on Windows)

```bash
make          # Release build → ./helixvault
make debug    # Debug + ASan/UBSan
make test     # Run self-test
make demo     # End-to-end demo
make clean    # Remove build artefacts
```

### Option B — MSVC (Developer Command Prompt)

Open **"Developer Command Prompt for VS 20xx"** (or run `vcvarsall.bat x64`),
then:

```bat
build_msvc.bat           # Release build → helixvault.exe
build_msvc.bat debug     # Debug build
build_msvc.bat clean     # Remove build artefacts
```

> **Note:** The project uses only standard C99 types and Win32 APIs on Windows.
> No third-party libraries are required.

---

## CLI Usage

```
helixvault <command> [args...]

Commands:
  mb  <bucket>                  Create a bucket
  rb  <bucket>                  Delete a bucket (must be empty)
  lb                            List all buckets
  put <bucket> <key> <file>     Upload a file (CDC chunked + deduped)
  get <bucket> <key> <dst>      Reconstruct and download an object
  del <bucket> <key>            Delete object + GC unreferenced chunks
  ls  <bucket>                  List objects in a bucket
  info <bucket> <key>           Show object metadata
  gc                            Safety GC sweep (removes refcount=0 chunks)
  stats                         Print pool and arena statistics
  selftest                      Run built-in SHA-256 + CDC + round-trip tests

Storage root defaults to ./storage_root
Override with:  set HELIX_ROOT=D:\my\vault   (Windows)
                export HELIX_ROOT=/data/vault  (Linux/macOS)
```

### Example Session

```bash
# Create a bucket
./helixvault mb photos

# Upload two files (second one shares chunks with the first → dedup)
./helixvault put photos vacation.jpg  /path/to/vacation.jpg
./helixvault put photos vacation2.jpg /path/to/vacation.jpg   # identical → no extra disk use

# List objects
./helixvault ls photos
# Key                                                 Size          Content-Type
# --------------------------------------------------  ------------  --------------------------
# vacation.jpg                                        4.21 MiB      application/octet-stream
# vacation2.jpg                                       4.21 MiB      application/octet-stream

# Retrieve
./helixvault get photos vacation.jpg /tmp/recovered.jpg

# Delete one copy (chunks stay — still referenced by vacation2.jpg)
./helixvault del photos vacation.jpg

# Delete second copy (all chunks GC'd from disk)
./helixvault del photos vacation2.jpg

# Pool stats
./helixvault stats

# Built-in self-test
./helixvault selftest
```

---

## Self-Test Output (example)

```
=== HelixVault Self-Test ===

  [sha256] Testing known vectors...
  [sha256] PASS  'abc' -> ba7816bf8f01cfea414140de5dae2ec73b00361...
  [sha256] PASS  ''    -> e3b0c44298fc1c149afbf4c8996fb92427ae41e...

  [cdc] Testing chunker with synthetic data...
  [cdc] Chunk #1  offset=0        size=7823
  [cdc] Chunk #2  offset=7823     size=12044
  ...
  [chunker_stats] selftest_cdc       | chunks=247  avg=8291 B  min=2048 B  max=65536 B

  [roundtrip] Full PUT/GET/DELETE test ...
  [roundtrip] PUT: OK
  [roundtrip] GET: OK (size=204)
  [roundtrip] Content match: PASS
  [roundtrip] PUT (dedup): OK
  [storage_pool_stats] after_dedup_put      | chunks=1  bytes=204
  [roundtrip] DEL hello.txt: OK
  [storage_pool_stats] after_del_original   | chunks=1  bytes=204
  [roundtrip] DEL hello-copy.txt: OK
  [storage_pool_stats] after_del_copy       | chunks=0  bytes=204

=== Self-Test Complete ===
```

---

## Module Summary

| File | Responsibility |
|------|----------------|
| `arena.h/c` | Slab-chain bump-pointer allocator |
| `hashtable.h/c` | FNV-1a open-addressing hash table |
| `sha256.h/c` | Portable FIPS 180-4 SHA-256 |
| `chunker.h/c` | Rabin/Gear CDC with configurable min/max |
| `storage.h/c` | Physical chunk pool, WAL, refcount index, GC |
| `manifest.h/c` | Binary object manifest, CRC-32 integrity |
| `bucket.h/c` | Bucket CRUD + object PUT/GET/DELETE/LIST |
| `main.c` | CLI, self-test harness |
