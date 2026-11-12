/**
 * tree.h — File Hierarchy Tree (N-ary Tree / Directory Trie)
 *
 * Implements the hierarchical folder/file tree structure inside buckets,
 * as required by the AWS S3-inspired Object Storage architecture.
 *
 * Each bucket can project its flat object keys (e.g., "Documents/2026/notes.txt")
 * into a full hierarchical tree of directory and file nodes using this module.
 */

#ifndef HELIX_TREE_H
#define HELIX_TREE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Data Structure: N-ary Tree Node (First-Child / Next-Sibling representation)
 * ---------------------------------------------------------------------- */
typedef struct TreeNode {
    char              name[128];         /**< Directory or file name. */
    bool              is_file;           /**< True if file, false if directory. */
    uint64_t          size;              /**< File size in bytes (0 for dirs). */
    char              content_type[64];  /**< MIME type (e.g. "text/plain"). */
    struct TreeNode  *first_child;       /**< Pointer to first child node. */
    struct TreeNode  *next_sibling;      /**< Pointer to next sibling node. */
} TreeNode;

/* -------------------------------------------------------------------------
 * Tree Lifecycle & Operations
 * ---------------------------------------------------------------------- */

/**
 * tree_node_create — allocate and initialize a new tree node.
 */
TreeNode *tree_node_create(const char *name, bool is_file, uint64_t size, const char *content_type);

/**
 * tree_free — recursively free the tree and all its descendants.
 */
void tree_free(TreeNode *root);

/**
 * tree_insert_path — insert an S3 object key (path) into the hierarchy tree.
 *
 * Example: path = "Documents/Reports/final.pdf"
 * Creates "Documents" (dir) -> "Reports" (dir) -> "final.pdf" (file).
 */
void tree_insert_path(TreeNode *root, const char *path, uint64_t size, const char *content_type);

/**
 * tree_display — print the tree with standard ASCII/branch formatting.
 */
void tree_display(const TreeNode *root, const char *bucket_name);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_TREE_H */

