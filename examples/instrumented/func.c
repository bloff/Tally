/*
 * Small instrumented playground workload for checking calls, printing,
 * allocator access, and explicit Tally yields.
 */
#include <stdio.h>
#include <stdint.h>

#include "shmall_api.h"

extern int64_t counter;

void f(void* args){
    int sum = 0;
    double d = 0.9;
 
    for(int i = 0; i < 10000; i++){
        sum += i;
        printf("sum: %d\n", sum);
        d *= 0.9;
    }

    //printf("sum: %d\n", sum);
    //shmall_free(p);
}
