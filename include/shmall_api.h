/*
 * Instrumented SHMALL allocation API callable from minithread workloads.
 */
#ifndef SHMALL_API
#define SHMALL_API

#include <stdint.h>
#include "minithread_api.h"
#include "shmall.h"

void* shmall_alloc(uint32_t size);
void shmall_free(void* p);

#endif
