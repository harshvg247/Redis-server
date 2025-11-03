#ifndef MINHEAP_H
#define MINHEAP_H

#include <stdlib.h>
#include <stddef.h>
#include <string.h> // For memmove

/**
 * @brief Function pointer type for comparing two items in the heap.
 *
 * Should return:
 * < 0 if a < b
 * = 0 if a == b
 * > 0 if a > b
 */
typedef int (*heap_compare_func)(const void *a, const void *b);

/**
 * @brief The heap structure.
 */
typedef struct {
    void **items;            // Array of pointers to items
    size_t size;             // Current number of items in the heap
    size_t capacity;         // Total allocated capacity of the array
    heap_compare_func cmp;   // The comparison function
} heap_t;

// --- Internal Helper Functions ---

/**
 * @brief Swaps two items in the heap's array.
 */
static inline void _heap_swap(void **a, void **b) {
    void *temp = *a;
    *a = *b;
    *b = temp;
}

/**
 * @brief Ensures the heap has enough capacity for one more item.
 * @return 0 on success, -1 on allocation failure.
 */
static inline int _heap_grow(heap_t *h) {
    if (h->size >= h->capacity) {
        size_t new_capacity = (h->capacity == 0) ? 16 : h->capacity * 2;
        void **new_items = (void **)realloc(h->items, new_capacity * sizeof(void *));
        if (new_items == NULL) {
            return -1; // Allocation failed
        }
        h->items = new_items;
        h->capacity = new_capacity;
    }
    return 0;
}

/**
 * @brief Moves an item up the heap to its correct position.
 */
static inline void _heap_sift_up(heap_t *h, size_t index) {
    if (index == 0) return; // At the root

    size_t parent_index = (index - 1) / 2;

    // While item is smaller than its parent, swap up
    while (index > 0 && h->cmp(h->items[index], h->items[parent_index]) < 0) {
        _heap_swap(&h->items[index], &h->items[parent_index]);
        index = parent_index;
        parent_index = (index - 1) / 2;
    }
}

/**
 * @brief Moves an item down the heap to its correct position.
 */
static inline void _heap_sift_down(heap_t *h, size_t index) {
    size_t left_child_index;
    size_t right_child_index;
    size_t min_index;

    while (1) {
        left_child_index = 2 * index + 1;
        right_child_index = 2 * index + 2;
        min_index = index;

        // Check if left child exists and is smaller
        if (left_child_index < h->size && h->cmp(h->items[left_child_index], h->items[min_index]) < 0) {
            min_index = left_child_index;
        }

        // Check if right child exists and is smaller than the (new) minimum
        if (right_child_index < h->size && h->cmp(h->items[right_child_index], h->items[min_index]) < 0) {
            min_index = right_child_index;
        }

        // If the smallest item is still the parent, we're done
        if (min_index == index) {
            break;
        }

        // Otherwise, swap down
        _heap_swap(&h->items[index], &h->items[min_index]);
        index = min_index;
    }
}

// --- Public API Functions ---

/**
 * @brief Creates and initializes a new heap.
 * @param cmp The comparison function for item priority.
 * @return A pointer to the new heap, or NULL on failure.
 */
static inline heap_t *heap_create(heap_compare_func cmp) {
    if (cmp == NULL) return NULL; // Must have a comparator

    heap_t *h = (heap_t *)malloc(sizeof(heap_t));
    if (h == NULL) return NULL;

    h->items = NULL;
    h->size = 0;
    h->capacity = 0;
    h->cmp = cmp;

    return h;
}

/**
 * @brief Frees all memory associated with the heap.
 * @note This does NOT free the items themselves, just the heap's internal array.
 */
static inline void heap_destroy(heap_t *h) {
    if (h == NULL) return;
    if (h->items != NULL) {
        free(h->items);
    }
    free(h);
}

/**
 * @brief Gets the number of items in the heap.
 */
static inline size_t heap_size(heap_t *h) {
    return h ? h->size : 0;
}

/**
 * @brief Adds a new item to the heap.
 * @return 0 on success, -1 on allocation failure.
 */
static inline int heap_push(heap_t *h, void *item) {
    if (_heap_grow(h) != 0) {
        return -1; // Failed to grow
    }

    // Add new item to the end
    h->items[h->size] = item;
    h->size++;

    // Sift it up to its correct position
    _heap_sift_up(h, h->size - 1);

    return 0;
}

/**
 * @brief Returns the minimum item from the heap without removing it.
 * @return The minimum item, or NULL if the heap is empty.
 */
static inline void *heap_peek(heap_t *h) {
    if (h == NULL || h->size == 0) {
        return NULL;
    }
    return h->items[0];
}

/**
 * @brief Removes and returns the minimum item from the heap.
 * @return The minimum item, or NULL if the heap is empty.
 */
static inline void *heap_pop(heap_t *h) {
    if (h == NULL || h->size == 0) {
        return NULL;
    }

    // The item to return is at the root
    void *min_item = h->items[0];

    // Move the last item to the root
    h->items[0] = h->items[h->size - 1];
    h->size--;

    // Sift the new root down to its correct position
    if (h->size > 0) {
        _heap_sift_down(h, 0);
    }

    return min_item;
}

#endif // MINHEAP_H