/**
 * arena.h — Memory Pool / Arena Allocator
 *
 * A bump-pointer arena allocator that hands out memory from large pre-allocated
 * slabs. Dramatically reduces malloc() overhead and fragmentation for
 * short-lived metadata nodes.
 *
 * Design:
 *   - An arena is a linked list of "slabs" (large contiguous blocks).
 *   - Allocation bumps a pointer forward inside the current slab.
 *   - When a slab is exhausted, a new one is allocated transparently.
 *   - The entire arena is freed in O(1) by walking the slab list once.
 */

#ifndef HELIX_ARENA_H
#define HELIX_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

/** Default slab size: 1 MiB per slab. */
#define ARENA_DEFAULT_SLAB_SIZE (1024UL * 1024UL)

/** Alignment for all allocations (must be a power-of-two). */
#define ARENA_ALIGNMENT 16

/* -------------------------------------------------------------------------
 * Data Structures
 * ---------------------------------------------------------------------- */

/**
 * ArenaSlat — one contiguous block of memory managed by the arena.
 * Slabs form a singly-linked list; each slab header lives at the very
 * beginning of its own allocated region.
 */
typedef struct ArenaSlab {
    struct ArenaSlab *next;  /**< Next slab in the chain (or NULL). */
    size_t            capacity; /**< Total usable bytes in this slab. */
    size_t            used;     /**< Bytes already handed out. */
    /* Data bytes follow immediately after this struct in memory. */
} ArenaSlab;

/**
 * Arena — the top-level arena handle held by callers.
 */
typedef struct Arena {
    ArenaSlab *head;           /**< Current (most recent) slab. */
    size_t     default_slab_size; /**< Size of new slabs (bytes). */
    size_t     total_allocated;   /**< Cumulative bytes handed to callers. */
    size_t     total_wasted;      /**< Alignment padding bytes wasted. */
} Arena;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * arena_create — initialise an arena with a given slab size.
 *
 * @param slab_size  Bytes per slab; pass 0 to use ARENA_DEFAULT_SLAB_SIZE.
 * @return           Pointer to a heap-allocated Arena, or NULL on OOM.
 */
Arena *arena_create(size_t slab_size);

/**
 * arena_alloc — allocate `size` bytes from the arena.
 *
 * The returned memory is zeroed and aligned to ARENA_ALIGNMENT bytes.
 * Never returns NULL for size == 0 (returns a valid unique pointer).
 *
 * @param arena  The arena to allocate from.
 * @param size   Number of bytes requested.
 * @return       Pointer to allocated memory, or NULL on OOM.
 */
void *arena_alloc(Arena *arena, size_t size);

/**
 * arena_strdup — duplicate a C-string into the arena.
 *
 * @param arena  Target arena.
 * @param s      Null-terminated source string.
 * @return       Pointer to the duplicate, or NULL on OOM / NULL input.
 */
char *arena_strdup(Arena *arena, const char *s);

/**
 * arena_reset — release all allocations but keep the slab memory.
 *
 * Useful for reusing an arena across request boundaries without the
 * overhead of free/malloc for slabs.
 *
 * @param arena  Arena to reset.
 */
void arena_reset(Arena *arena);

/**
 * arena_destroy — free all slabs and the arena struct itself.
 *
 * After this call `arena` is an invalid pointer.
 *
 * @param arena  Arena to destroy (may be NULL — no-op).
 */
void arena_destroy(Arena *arena);

/**
 * arena_stats — print a one-line summary of arena usage to stderr.
 *
 * @param arena  Arena to inspect (may be NULL — prints "no arena").
 * @param label  Short human-readable label for the output line.
 */
void arena_stats(const Arena *arena, const char *label);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_ARENA_H */
