/*
 * Instrumented self-contained synthetic random walk. The graph is generated
 * arithmetically inside the workload so the benchmark does not call back into
 * host graph helpers while measuring work.
 */
#include <stdint.h>

#include "self_walk.h"

#define SELF_WALK_NODE_MASK 1023u

static uint32_t next_random(uint32_t seed){
    return (seed * 1664525u) + 1013904223u;
}

static uint32_t synthetic_neighbor(uint32_t node, uint32_t index){
    static const uint32_t offsets[4] = {1u, 7u, 31u, 127u};
    uint32_t mixed = node ^ (node << 5) ^ (node >> 3);
    return (mixed + offsets[index & 3u]) & SELF_WALK_NODE_MASK;
}

void run_self_walk(void* args){
    volatile struct self_walk_args *input = (volatile struct self_walk_args*) args;
    uint32_t node = input->current_node;
    uint32_t seed = input->seed;
    uint64_t vertices = input->vertices_walked;
    uint64_t target = input->target_vertices;

    while(vertices < target){
        seed = next_random(seed);
        node = synthetic_neighbor(node, seed & 3u);
        vertices++;

        input->current_node = node;
        input->seed = seed;
        input->vertices_walked = vertices;
    }
}
