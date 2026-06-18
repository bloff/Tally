/*
 * Instrumented SHMALL public allocation/free entrypoints.
 */
#include "shmall_api.h"

#include <stdio.h>

const int SHMALL_HASH = HASH_S("SHMALL");

void* shmall_alloc(uint32_t size){
    //printf("calling %s aka %d with index: %d\n", SHMALL_NAME_I, HASH_S("SHMALL"),h);
    return _heap_alloc(minithread_find(SHMALL_HASH), size);
}

void shmall_free(void* p){
    _heap_free(minithread_find(SHMALL_HASH), p);
}
