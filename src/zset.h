#ifndef ZSET_H
#define ZSET_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uthash.h" // Assumed to be available

// --- Data Structures ---

/**
 * @brief AVL tree node.
 * This is the ONLY data structure. It stores the data.
 */
typedef struct ZSetNode {
    double score;
    char *member; // This node *owns* this string
    
    struct ZSetNode *left;
    struct ZSetNode *right;
    int height;
    size_t count; // Number of nodes in this subtree (for rank)
} ZSetNode;

/**
 * @brief The main ZSET object
 * Now it only holds the root of the AVL tree.
 */
typedef struct {
    ZSetNode *avl_root;
} RedisZSet;


// --- Internal AVL Tree Logic ---

static inline int _zset_avl_height(ZSetNode *n) { return n ? n->height : 0; }
static inline size_t _zset_avl_count(ZSetNode *n) { return n ? n->count : 0; }
static inline int _zset_max(int a, int b) { return (a > b) ? a : b; }

static inline void _zset_avl_update(ZSetNode *n) {
    if (n) {
        n->height = 1 + _zset_max(_zset_avl_height(n->left), _zset_avl_height(n->right));
        n->count = 1 + _zset_avl_count(n->left) + _zset_avl_count(n->right);
    }
}

static inline ZSetNode* _zset_node_new(double score, const char *member) {
    ZSetNode *node = (ZSetNode*)malloc(sizeof(ZSetNode));
    node->score = score;
    node->member = strdup(member);
    node->left = NULL;
    node->right = NULL;
    node->height = 1;
    node->count = 1;
    return node;
}

static inline int _zset_avl_cmp(double a_score, const char *a_member,
                                double b_score, const char *b_member)
{
    if (a_score < b_score) return -1;
    if (a_score > b_score) return 1;
    return strcmp(a_member, b_member);
}

static inline ZSetNode* _zset_avl_rotate_right(ZSetNode *y) {
    ZSetNode *x = y->left;
    ZSetNode *T2 = x->right;
    x->right = y;
    y->left = T2;
    _zset_avl_update(y);
    _zset_avl_update(x);
    return x;
}

static inline ZSetNode* _zset_avl_rotate_left(ZSetNode *x) {
    ZSetNode *y = x->right;
    ZSetNode *T2 = y->left;
    y->left = x;
    x->right = T2;
    _zset_avl_update(x);
    _zset_avl_update(y);
    return y;
}

static inline int _zset_avl_get_balance(ZSetNode *n) {
    return n ? _zset_avl_height(n->right) - _zset_avl_height(n->left) : 0;
}

static inline ZSetNode* _zset_avl_min_node(ZSetNode *node) {
    while (node && node->left) node = node->left;
    return node;
}

/**
 * @brief (Internal) Recursive find-by-member. O(N)
 */
static inline ZSetNode* _zset_find_by_member(ZSetNode *node, const char *member) {
    if (node == NULL) return NULL;
    if (strcmp(node->member, member) == 0) return node;
    
    ZSetNode *left = _zset_find_by_member(node->left, member);
    if (left) return left;
    
    return _zset_find_by_member(node->right, member);
}

/**
 * @brief (Internal) Recursive insert for AVL tree
 */
static inline ZSetNode* _zset_avl_insert(ZSetNode *node, ZSetNode *new_node) {
    if (node == NULL) {
        return new_node;
    }

    int cmp = _zset_avl_cmp(new_node->score, new_node->member,
                            node->score, node->member);
    if (cmp < 0) {
        node->left = _zset_avl_insert(node->left, new_node);
    } else {
        node->right = _zset_avl_insert(node->right, new_node);
    } 

    _zset_avl_update(node);
    int balance = _zset_avl_get_balance(node);

    // Rebalance
    if (balance < -1 && _zset_avl_cmp(new_node->score, new_node->member, node->left->score, node->left->member) < 0) // LL
        return _zset_avl_rotate_right(node);
    if (balance < -1 && _zset_avl_cmp(new_node->score, new_node->member, node->left->score, node->left->member) > 0) { // LR
        node->left = _zset_avl_rotate_left(node->left);
        return _zset_avl_rotate_right(node);
    }
    if (balance > 1 && _zset_avl_cmp(new_node->score, new_node->member, node->right->score, node->right->member) > 0) // RR
        return _zset_avl_rotate_left(node);
    if (balance > 1 && _zset_avl_cmp(new_node->score, new_node->member, node->right->score, node->right->member) < 0) { // RL
        node->right = _zset_avl_rotate_right(node->right);
        return _zset_avl_rotate_left(node);
    }
    return node;
}

/**
 * @brief (Internal) Recursive remove for AVL tree
 * This is the standard, correct remove. It unlinks the node
 * and frees its memory.
 */
static inline ZSetNode* _zset_avl_remove(ZSetNode *node, double score, const char *member) {
    if (node == NULL) return NULL;

    int cmp = _zset_avl_cmp(score, member, node->score, node->member);

    if (cmp < 0) {
        node->left = _zset_avl_remove(node->left, score, member);
    } else if (cmp > 0) {
        node->right = _zset_avl_remove(node->right, score, member);
    } else {
        // Node found! This is the one to delete.
        if (!node->left || !node->right) {
            // 0 or 1 child
            ZSetNode *temp = node->left ? node->left : node->right;
            if (temp == NULL) { // 0 child
                // Free this node and return NULL to its parent
                free(node->member);
                free(node);
                return NULL;
            } else { // 1 child
                // Free this node and return its child to its parent
                free(node->member);
                free(node);
                return temp;
            }
        } else {
            // 2 children
            ZSetNode *succ = _zset_avl_min_node(node->right);
            
            // Copy successor's data into this node
            free(node->member); // Free old member string
            node->score = succ->score;
            node->member = strdup(succ->member); // Copy new member string
            
            // Recursively remove the successor (which is now a duplicate)
            node->right = _zset_avl_remove(node->right, succ->score, succ->member);
        }
    }

    if (node == NULL) return NULL;
    
    _zset_avl_update(node);
    int balance = _zset_avl_get_balance(node);

    // Rebalance
    if (balance < -1 && _zset_avl_get_balance(node->left) <= 0) // LL
        return _zset_avl_rotate_right(node);
    if (balance < -1 && _zset_avl_get_balance(node->left) > 0) { // LR
        node->left = _zset_avl_rotate_left(node->left);
        return _zset_avl_rotate_right(node);
    }
    if (balance > 1 && _zset_avl_get_balance(node->right) >= 0) // RR
        return _zset_avl_rotate_left(node);
    if (balance > 1 && _zset_avl_get_balance(node->right) < 0) { // RL
        node->right = _zset_avl_rotate_right(node->right);
        return _zset_avl_rotate_left(node);
    }
    return node;
}


// --- Public API ---

static inline RedisZSet* zset_create() {
    RedisZSet *zset = (RedisZSet*)malloc(sizeof(RedisZSet));
    if (zset) {
        zset->avl_root = NULL;
    }
    return zset;
}

static inline void _zset_free_node_recursive(ZSetNode *node) {
    if (node == NULL) return;
    _zset_free_node_recursive(node->left);
    _zset_free_node_recursive(node->right);
    free(node->member);
    free(node);
}

static inline void zset_free(RedisZSet *zset) {
    if (zset == NULL) return;
    _zset_free_node_recursive(zset->avl_root);
    free(zset);
}

/**
 * @brief (Corrected) Adds/Updates a member in the Sorted Set.
 * @return 1 if a new element was added, 0 if updated.
 */
static inline int zset_add(RedisZSet *zset, double score, char *member) {
    // 1. Find the old node (O(N) search)
    ZSetNode *old_node = _zset_find_by_member(zset->avl_root, member);
    int added = 1;
    
    if (old_node) {
        if (old_node->score == score) return 0; // No change
        
        // Remove the old node (this frees its memory)
        zset->avl_root = _zset_avl_remove(zset->avl_root, old_node->score, old_node->member);
        added = 0; // It's an update
    }
    
    // 2. Add the new node
    ZSetNode *new_node = _zset_node_new(score, member);
    zset->avl_root = _zset_avl_insert(zset->avl_root, new_node);
    return added;
}

/**
 * @brief (Corrected) Removes a member from the Sorted Set.
 * @return 1 if removed, 0 if not found.
 */
static inline int zset_remove(RedisZSet *zset, char *member) {
    // 1. Find the node (O(N) search)
    ZSetNode *node = _zset_find_by_member(zset->avl_root, member);
    
    if (node == NULL) {
        return 0; // Not found
    }
    
    // 2. Remove it (this also frees its memory)
    zset->avl_root = _zset_avl_remove(zset->avl_root, node->score, node->member);
    
    return 1;
}

static inline ZSetNode* zset_get_by_rank(RedisZSet *zset, size_t rank) {
    if (zset == NULL || rank >= _zset_avl_count(zset->avl_root)) {
        return NULL; // Out of bounds
    }

    ZSetNode *node = zset->avl_root;
    while (node) {
        size_t left_count = _zset_avl_count(node->left);
        if (rank == left_count) {
            return node;
        }
        if (rank < left_count) {
            node = node->left;
        } else {
            node = node->right;
            rank = rank - left_count - 1;
        }
    }
    return NULL;
}

#endif // ZSET_H