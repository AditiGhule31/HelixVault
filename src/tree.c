/**
 * tree.c — File Hierarchy Tree Implementation
 *
 * Implements N-ary Tree using first-child / next-sibling representation.
 * Supports tokenized path insertion, tree traversal, and formatted ASCII printing.
 */

#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper for human-readable file sizes */
static void format_tree_size(uint64_t bytes, char *buf, size_t buf_sz) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, buf_sz, "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(buf, buf_sz, "%.2f MiB", bytes / (1024.0 * 1024));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, buf_sz, "%.2f KiB", bytes / 1024.0);
    } else {
        snprintf(buf, buf_sz, "%lu B", (unsigned long)bytes);
    }
}

TreeNode *tree_node_create(const char *name, bool is_file, uint64_t size, const char *content_type) {
    TreeNode *node = (TreeNode *)malloc(sizeof(TreeNode));
    if (!node) return NULL;

    strncpy(node->name, name ? name : "", sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';

    node->is_file = is_file;
    node->size = size;

    if (content_type && strlen(content_type) > 0) {
        strncpy(node->content_type, content_type, sizeof(node->content_type) - 1);
        node->content_type[sizeof(node->content_type) - 1] = '\0';
    } else {
        strcpy(node->content_type, is_file ? "application/octet-stream" : "directory");
    }

    node->first_child  = NULL;
    node->next_sibling = NULL;

    return node;
}

void tree_free(TreeNode *root) {
    if (!root) return;

    /* Free all children recursively */
    TreeNode *child = root->first_child;
    while (child) {
        TreeNode *next = child->next_sibling;
        tree_free(child);
        child = next;
    }

    free(root);
}

static TreeNode *find_or_add_child(TreeNode *parent, const char *name, bool is_file, uint64_t size, const char *content_type) {
    TreeNode *prev = NULL;
    TreeNode *curr = parent->first_child;

    /* Check if child already exists */
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            /* If it was previously an intermediate dir but now updated */
            if (is_file) {
                curr->is_file = true;
                curr->size = size;
                if (content_type) {
                    strncpy(curr->content_type, content_type, sizeof(curr->content_type) - 1);
                }
            }
            return curr;
        }
        prev = curr;
        curr = curr->next_sibling;
    }

    /* Create new child node */
    TreeNode *new_node = tree_node_create(name, is_file, size, content_type);
    if (!new_node) return NULL;

    if (!prev) {
        parent->first_child = new_node;
    } else {
        prev->next_sibling = new_node;
    }

    return new_node;
}

void tree_insert_path(TreeNode *root, const char *path, uint64_t size, const char *content_type) {
    if (!root || !path || strlen(path) == 0) return;

    char path_buf[1024];
    strncpy(path_buf, path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    /* Count number of tokens */
    char *tokens[64];
    int token_count = 0;

#ifdef _WIN32
    char *tok = strtok(path_buf, "/\\");
    while (tok && token_count < 64) {
        tokens[token_count++] = tok;
        tok = strtok(NULL, "/\\");
    }
#else
    char *saveptr = NULL;
    char *tok = strtok_r(path_buf, "/\\", &saveptr);
    while (tok && token_count < 64) {
        tokens[token_count++] = tok;
        tok = strtok_r(NULL, "/\\", &saveptr);
    }
#endif

    if (token_count == 0) return;

    TreeNode *curr = root;
    for (int i = 0; i < token_count; i++) {
        bool is_last = (i == token_count - 1);
        curr = find_or_add_child(curr, tokens[i],
                                is_last ? true : false,
                                is_last ? size : 0,
                                is_last ? content_type : "directory");
        if (!curr) break;
    }
}

static void print_subtree(const TreeNode *node, const char *prefix, bool is_last) {
    if (!node) return;

    printf("%s%s-- %s%s",
           prefix,
           is_last ? "`" : "|",
           node->name,
           node->is_file ? "" : "/");

    if (node->is_file) {
        char sz_buf[32];
        format_tree_size(node->size, sz_buf, sizeof(sz_buf));
        printf("  [%s, %s]\n", sz_buf, node->content_type);
    } else {
        printf("\n");
    }

    /* Prepare prefix for children */
    char new_prefix[256];
    snprintf(new_prefix, sizeof(new_prefix), "%s%s   ",
             prefix,
             is_last ? " " : "|");

    /* Count children to know which one is last */
    TreeNode *child = node->first_child;
    while (child) {
        bool child_is_last = (child->next_sibling == NULL);
        print_subtree(child, new_prefix, child_is_last);
        child = child->next_sibling;
    }
}

void tree_display(const TreeNode *root, const char *bucket_name) {
    printf("\n=================================================================\n");
    printf("        FILE HIERARCHY TREE (AWS S3 DIRECTORY TRIE)             \n");
    printf("=================================================================\n");
    printf(" Bucket: [ %s ]\n", bucket_name ? bucket_name : "root");
    printf("-----------------------------------------------------------------\n");

    if (!root || !root->first_child) {
        printf("  (Bucket is empty — no files or folders)\n");
    } else {
        TreeNode *child = root->first_child;
        while (child) {
            bool is_last = (child->next_sibling == NULL);
            print_subtree(child, "  ", is_last);
            child = child->next_sibling;
        }
    }
    printf("=================================================================\n\n");
}
