/*
 * Clean-side SHMALL module setup used by the host runtime before instrumented
 * allocation calls begin.
 */
#include "shmall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint shmall_get_bin_index(size_t sz);
void shmall_create_foot(node_t *head);
void shmall_add_node(bin_t *bin, node_t* node);
footer_t *shmall_get_foot(node_t *node);

uint64_t HEAP_MAX_SIZE = 0xF0000;
uint64_t HEAP_MIN_SIZE = 0x10000;
const char* SHMALL_NAME = "SHMALL";

void init_shmall(void* module_strut, void* args_ptr){
    printf("size mod: %d\n", sizeof *module_strut);
    heap_t* heap = (heap_t*) module_strut; 
    struct shmall_args* args = (struct shmall_args*) args_ptr;

    for (int i = 0; i < BIN_COUNT; i++) {
        heap->bins[i] = malloc(sizeof(bin_t));
        memset(heap->bins[i], 0, sizeof(bin_t));
    }

    heap->module.size = args->size;

    printf("got args\n");

    node_t *init_region = (node_t *) heap->module.start;

    printf("got start region %d %d\n", args->size, heap->module.start);

    init_region->hole = 1;
    init_region->size = (args->size) - sizeof(node_t) - sizeof(footer_t);

    printf("got region %d %d\n", init_region->size, args->size);

    shmall_create_foot(init_region);

    printf("phase 1\n");

    shmall_add_node(heap->bins[shmall_get_bin_index(init_region->size)], init_region);

    printf("phase 2\n");
    
    heap->start = (char *) heap->module.start;
    heap->end = heap->start + args->size;

    printf("ended func\n");
}

void clean_shmall(){
    return;
}

//duplicated code from heap.c but this one will not be instrumented, so it can run on module init process
uint shmall_get_bin_index(size_t sz) {
    uint index = 0;
    sz = sz < 4 ? 4 : sz;

    while (sz >>= 1) index++; 
    index -= 2; 
    
    if (index > BIN_MAX_IDX) index = BIN_MAX_IDX;
    printf("bin: %d\n", index); 
    return index;
}

//duplicated code from heap.c but this one will not be instrumented, so it can run on module init process
void shmall_create_foot(node_t *head) {
    footer_t *foot = shmall_get_foot(head);
    foot->header = head;
}

//duplicated code from llist.c but this one will not be instrumented, so it can run on module init process
void shmall_add_node(bin_t *bin, node_t* node) {
    printf("hey\n");
    
    node->next = NULL;
    node->prev = NULL;

    //printf("before pref %d\n", bin->head);

    node_t *temp = bin->head;

    printf("temp\n");

    if (bin->head == NULL) {
        bin->head = node;
        return;
    }

    printf("head != null\n");

    // we need to save next and prev while we iterate
    node_t *current = bin->head;
    node_t *previous = NULL;
    // iterate until we get the the end of the list or we find a 
    // node whose size is
    while (current != NULL && current->size <= node->size) {
        previous = current;
        current = current->next;
    }

    if (current == NULL) { // we reached the end of the list
        previous->next = node;
        node->prev = previous;
    }
    else {
        if (previous != NULL) { // middle of list, connect all links!
            node->next = current;
            previous->next = node;

            node->prev = previous;
            current->prev = node;
        }
        else { // head is the only element
            node->next = bin->head;
            bin->head->prev = node;
            bin->head = node;
        }
    }
}

footer_t *shmall_get_foot(node_t *node) {
    return (footer_t *) ((char *) node + sizeof(node_t) + node->size);
}
