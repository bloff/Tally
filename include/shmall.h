/*
 * Internal declarations for the SHMALL private heap module.
 */
#ifndef SHMALL_H
#define SHMALL_H

#include <stdint.h>
#include <stddef.h>
#include "minithread_api.h"

extern const char* SHMALL_NAME;

struct shmall_args{
    uint32_t size;
};

void init_shmall(void*, void*);
void clean_shmall();

#define MIN_ALLOC_SZ 4

#define MIN_WILDERNESS 0x2000
#define MAX_WILDERNESS 0x1000000

#define BIN_COUNT 9
#define BIN_MAX_IDX (BIN_COUNT - 1)

typedef unsigned int uint;

typedef struct node_t {
    uint hole;
    uint size;
    struct node_t* next;
    struct node_t* prev;
} node_t;

typedef struct { 
    node_t *header;
} footer_t;

typedef struct {
    node_t* head;
} bin_t;

typedef struct heap_t_{
    struct gcctally_module_wrapper module;
    char *start;
    char *end;
    bin_t *bins[BIN_COUNT];
} heap_t;

static uint overhead = sizeof(footer_t) + sizeof(node_t);

void *_heap_alloc(heap_t *heap, size_t size);
void _heap_free(heap_t *heap, void *p);
uint expand(heap_t *heap, size_t sz);
void contract(heap_t *heap, size_t sz);

uint get_bin_index(size_t sz);
void create_foot(node_t *head);
footer_t *get_foot(node_t *head);

node_t *get_wilderness(heap_t *heap);

#endif //SHMALL_H
