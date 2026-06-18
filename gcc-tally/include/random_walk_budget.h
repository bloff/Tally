/*
 * Arguments passed to the budget-walk experiment's instrumented random walker.
 */
#ifndef RANDOM_WALK_BUDGET_H
#define RANDOM_WALK_BUDGET_H

#include <stdint.h>

struct random_walk_budget_args {
    int current_node;
    uint32_t seed;
};

#endif
