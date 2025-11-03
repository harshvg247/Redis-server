#ifndef HANDLER_H
#define HANDLER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "uthash.h"
#include "utils.h"      // Assumed to exist
#include "time_utils.h" // Assumed to exist
#include "parser.h"     // Include our parser functions
#include "minheap.h"    // Include our heap library
#include "zset.h"

// --- Defines ---
#define REDIS_OK "+OK\r\n"
#define NULL_BULK_STRING "$-1\r\n"
#define REDIS_WRONGTYPE "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
#define REDIS_EMPTY_ARRAY "*0\r\n"
#define dbg 1

// --- Data Structures ---

typedef enum
{
    VAL_TYPE_STRING,
    VAL_TYPE_LIST,
    VAL_TYPE_ZSET
} val_type;

typedef struct ListNode
{
    char *value;
    struct ListNode *next;
} ListNode;

typedef struct
{
    ListNode *head;
    ListNode *tail;
    size_t len;
} RedisList;

typedef struct db_entry
{
    char *key;
    void *value; // Points to a char* (string) or RedisList* (list)
    val_type type;
    long long expiry_ms;
    UT_hash_handle hh;
} db_entry;


// --- Struct for the HEAP ---
// This is stored in the heap, completely separate from db_entry
typedef struct expiry_entry
{
    long long expiry_ms;
    char *key; // A *copy* of the key
} expiry_entry_t;

// --- Compare function for the HEAP ---
static inline int compare_expiry_entry(const void *a, const void *b)
{
    expiry_entry_t *e1 = (expiry_entry_t *)a;
    expiry_entry_t *e2 = (expiry_entry_t *)b;
    if (e1->expiry_ms < e2->expiry_ms) return -1;
    if (e1->expiry_ms > e2->expiry_ms) return 1;
    return 0;
}


// --- Static Helper Functions ---

static inline void free_db_value(db_entry *e)
{
    if (e == NULL || e->value == NULL)
        return;

    if (e->type == VAL_TYPE_STRING)
    {
        free(e->value); // 'value' is a char*
    }
    else if (e->type == VAL_TYPE_LIST)
    {
        RedisList *list = (RedisList *)e->value;
        ListNode *current = list->head;
        while (current)
        {
            ListNode *next = current->next;
            free(current->value); // Free the string in the node
            free(current);        // Free the node itself
            current = next;
        }
        free(list); // Free the main list struct
    }
    else if (e->type == VAL_TYPE_ZSET) // 3. Add free logic
    {
        zset_free((RedisZSet*)e->value); // Calls the new lib's free func
    }
    
    e->value = NULL;
}

// --- Public Handler Functions ---

static inline void handle_echo(const char *str, int fd)
{
    if (dbg)
        printf("Handling echo command\n");
    char *encoded_str = encode_bulk_str(str);
    if (encoded_str) {
        send(fd, encoded_str, strlen(encoded_str), 0);
        free(encoded_str);
    }
}

static inline void handle_set(db_entry **db_head, heap_t *expiry_heap, char *key, char *value, long long expiry, int fd)
{
    if (dbg)
        printf("handle_set-> params recvd: %s %s %lld %d\n", key, value, expiry, fd);
    
    db_entry *e;
    HASH_FIND_STR(*db_head, key, e);

    if (e == NULL)
    {
        e = (db_entry *)malloc(sizeof(db_entry));
        e->key = strdup(key);
        e->value = NULL;
        HASH_ADD_STR(*db_head, key, e);
    }
    else
    {
        free_db_value(e);
        // Note: The *old* expiry_entry_t is still in the heap.
        // This is OK. Our new eviction loop will safely handle it.
    }

    e->value = strdup(value);
    e->type = VAL_TYPE_STRING;
    e->expiry_ms = expiry; // The hash table entry gets the expiry time

    // If the key has an expiry, push a NEW expiry_entry to the heap
    if (expiry != -1) {
        expiry_entry_t *heap_entry = (expiry_entry_t *)malloc(sizeof(expiry_entry_t));
        if (heap_entry) {
            heap_entry->expiry_ms = expiry;
            heap_entry->key = strdup(key); // Store a *copy* of the key
            if (heap_push(expiry_heap, heap_entry) != 0) {
                perror("heap_push");
                free(heap_entry->key);
                free(heap_entry);
            }
        }
    }

    send(fd, REDIS_OK, strlen(REDIS_OK), 0);
}

static inline void handle_get(db_entry **db_head, char *key, int fd)
{
    db_entry *e;
    HASH_FIND_STR(*db_head, key, e);

    if (e == NULL)
    {
        send(fd, NULL_BULK_STRING, strlen(NULL_BULK_STRING), 0);
        return;
    }

    // Passive eviction check
    if (e->expiry_ms != -1 && e->expiry_ms < current_time_ms())
    {
        if (dbg)
            printf("Passive evict (GET): %s\n", e->key);
        free_db_value(e);
        HASH_DEL(*db_head, e);
        free(e->key);
        free(e);
        send(fd, NULL_BULK_STRING, strlen(NULL_BULK_STRING), 0);
        return;
    }

    if (e->type != VAL_TYPE_STRING)
    {
        send(fd, REDIS_WRONGTYPE, strlen(REDIS_WRONGTYPE), 0);
        return;
    }

    char *value_str = (char *)e->value;
    char *response = encode_bulk_str(value_str);
    if (response)
    {
        send(fd, response, strlen(response), 0);
        free(response);
    }
}

static inline void handle_rpush(db_entry **db_head, char **cmnds, int cmnd_list_size, int fd)
{
    if (cmnd_list_size < 3) return;

    char *key = cmnds[1];
    db_entry *e;
    HASH_FIND_STR(*db_head, key, e);

    RedisList *list;

    if (e == NULL)
    {
        e = (db_entry *)malloc(sizeof(db_entry));
        e->key = strdup(key);
        e->expiry_ms = -1;
        e->type = VAL_TYPE_LIST;
        list = (RedisList *)malloc(sizeof(RedisList));
        list->head = NULL;
        list->tail = NULL;
        list->len = 0;
        e->value = list;
        HASH_ADD_STR(*db_head, key, e);
    }
    else
    {
        if (e->type != VAL_TYPE_LIST)
        {
            send(fd, REDIS_WRONGTYPE, strlen(REDIS_WRONGTYPE), 0);
            return;
        }

        // Passive eviction check
        if (e->expiry_ms != -1 && e->expiry_ms < current_time_ms())
        {
            if (dbg) printf("Passive evict (RPUSH): %s\n", e->key);
            free_db_value(e); // Free the old list
            // Re-initialize the list
            list = (RedisList *)malloc(sizeof(RedisList));
            list->head = NULL; list->tail = NULL; list->len = 0;
            e->value = list;
            e->expiry_ms = -1;
        }
        else
        {
            list = (RedisList *)e->value;
        }
    }

    for (int i = 2; i < cmnd_list_size; i++)
    {
        ListNode *new_node = (ListNode *)malloc(sizeof(ListNode));
        new_node->value = strdup(cmnds[i]);
        new_node->next = NULL;

        if (list->tail == NULL) {
            list->head = new_node;
            list->tail = new_node;
        } else {
            list->tail->next = new_node;
            list->tail = new_node;
        }
        list->len++;
    }

    char *response = encode_integer(list->len);
    if (response)
    {
        send(fd, response, strlen(response), 0);
        free(response);
    }
}

static inline void handle_lrange(db_entry **db_head, char **cmnds, int cmnd_list_size, int fd)
{
    if (cmnd_list_size != 4) return;

    char *key = cmnds[1];
    int start = strtol(cmnds[2], NULL, 10);
    int stop = strtol(cmnds[3], NULL, 10);

    db_entry *e;
    HASH_FIND_STR(*db_head, key, e);

    if (e == NULL)
    {
        send(fd, REDIS_EMPTY_ARRAY, strlen(REDIS_EMPTY_ARRAY), 0);
        return;
    }

    // Passive eviction check
    if (e->expiry_ms != -1 && e->expiry_ms < current_time_ms())
    {
        if (dbg) printf("Passive evict (LRANGE): %s\n", e->key);
        free_db_value(e);
        HASH_DEL(*db_head, e);
        free(e->key);
        free(e);
        send(fd, REDIS_EMPTY_ARRAY, strlen(REDIS_EMPTY_ARRAY), 0);
        return;
    }

    if (e->type != VAL_TYPE_LIST)
    {
        send(fd, REDIS_WRONGTYPE, strlen(REDIS_WRONGTYPE), 0);
        return;
    }

    RedisList *list = (RedisList *)e->value;
    int len = list->len;

    if (stop >= len) stop = len - 1;
    if (start >= len || start > stop) {
        send(fd, REDIS_EMPTY_ARRAY, strlen(REDIS_EMPTY_ARRAY), 0);
        return;
    }

    int count = (stop - start) + 1;
    char header[32];
    sprintf(header, "*%d\r\n", count);
    send(fd, header, strlen(header), 0);

    ListNode *current = list->head;
    for (int i = 0; i < start; i++)
    {
        current = current->next;
    }

    for (int i = 0; i < count; i++)
    {
        char *response_item = encode_bulk_str(current->value);
        if (response_item)
        {
            send(fd, response_item, strlen(response_item), 0);
            free(response_item);
        }
        current = current->next;
    }
}
static inline void handle_zadd(db_entry **db_head, char **cmnds, int cmnd_list_size, int fd)
{
    if (cmnd_list_size < 4 || (cmnd_list_size - 2) % 2 != 0) {
        return; // Wrong args
    }

    char *key = cmnds[1];
    db_entry *e;
    HASH_FIND_STR(*db_head, key, e);
    RedisZSet *zset;

    if (e == NULL) {
        // Create new ZSET
        e = (db_entry *)malloc(sizeof(db_entry));
        e->key = strdup(key);
        e->expiry_ms = -1;
        e->type = VAL_TYPE_ZSET;
        zset = zset_create(); // From new lib
        e->value = zset;
        HASH_ADD_STR(*db_head, key, e);
    } else {
        if (e->type != VAL_TYPE_ZSET) {
            send(fd, REDIS_WRONGTYPE, strlen(REDIS_WRONGTYPE), 0);
            return;
        }
        zset = (RedisZSet *)e->value;
    }

    int elements_added = 0;
    for (int i = 2; i < cmnd_list_size; i += 2) {
        double score = strtod(cmnds[i], NULL);
        char *member = cmnds[i+1];
        elements_added += zset_add(zset, score, member); // From new lib
    }
    
    char *response = encode_integer(elements_added);
    if (response) {
        send(fd, response, strlen(response), 0);
        free(response);
    }
}

static inline void handle_zrange(db_entry **db_head, char **cmnds, int cmnd_list_size, int fd)
{
    if (cmnd_list_size != 4) { /* send error */ return; }

    char *key = cmnds[1];
    long start = strtol(cmnds[2], NULL, 10);
    long stop = strtol(cmnds[3], NULL, 10);

    db_entry *e;
    HASH_FIND_STR(*db_head, key, e);

    if (e == NULL) {
        send(fd, REDIS_EMPTY_ARRAY, strlen(REDIS_EMPTY_ARRAY), 0);
        return;
    }
    if (e->type != VAL_TYPE_ZSET) {
        send(fd, REDIS_WRONGTYPE, strlen(REDIS_WRONGTYPE), 0);
        return;
    }
    
    RedisZSet *zset = (RedisZSet *)e->value;
    size_t total_elements = _zset_avl_count(zset->avl_root); // Get total size

    // Handle negative indices
    if (start < 0) start = total_elements + start;
    if (stop < 0) stop = total_elements + stop;
    if (start < 0) start = 0;
    if (start > stop || start >= total_elements) {
        send(fd, REDIS_EMPTY_ARRAY, strlen(REDIS_EMPTY_ARRAY), 0);
        return;
    }
    if (stop >= total_elements) stop = total_elements - 1;
    
    size_t count = (stop - start) + 1;
    
    char header[32];
    sprintf(header, "*%zu\r\n", count);
    send(fd, header, strlen(header), 0);
    
    // Iterate by rank
    for (size_t i = 0; i < count; i++) {
        ZSetNode *node = zset_get_by_rank(zset, start + i); // From new lib
        if (node) {
            char *response_item = encode_bulk_str(node->member);
            if (response_item) {
                send(fd, response_item, strlen(response_item), 0);
                free(response_item);
            }
        }
    }
}


#endif // HANDLER_H