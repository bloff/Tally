/*
 * Unit test for the DJB2-style runtime hash and compile-time HASH_S macro used
 * to identify Tally modules.
 */
#include <stdint.h>
#include <stdio.h>

#include "minithread_api.h"

int main(void) {
    uint32_t graph_runtime = HASH("graph");
    uint32_t graph_static = HASH_S("graph");
    uint32_t shmall_runtime = HASH("SHMALL");
    uint32_t shmall_static = HASH_S("SHMALL");

    if (graph_runtime == 0 || shmall_runtime == 0) {
        fprintf(stderr, "hashes should be non-zero\n");
        return 1;
    }

    if (graph_runtime != graph_static) {
        fprintf(stderr, "HASH/HASH_S mismatch for graph: %u != %u\n", graph_runtime, graph_static);
        return 1;
    }

    if (shmall_runtime != shmall_static) {
        fprintf(stderr, "HASH/HASH_S mismatch for SHMALL: %u != %u\n", shmall_runtime, shmall_static);
        return 1;
    }

    if (graph_runtime == shmall_runtime) {
        fprintf(stderr, "distinct module names collided in hash smoke test\n");
        return 1;
    }

    return 0;
}
