#include <stdint.h>

#include "./../include/graph_api.h"
#include "./../include/random_walk_budget.h"

static uint32_t next_random(uint32_t *seed){
    *seed = (*seed * 1664525u) + 1013904223u;
    return *seed;
}

void run_walk_budget(void* args){
    struct random_walk_budget_args *input = (struct random_walk_budget_args*) args;
    int u = input->current_node;
    uint32_t seed = input->seed;

    while(1){
        int n = get_n_neighbours(u);
        if(n <= 0){
            input->current_node = u;
            input->seed = seed;
            MINITHREAD_ERROR;
        }

        uint32_t r = next_random(&seed);
        u = get_x_neighbour(u, (int)(r % (uint32_t)n));
        add_hop();
    }
}
