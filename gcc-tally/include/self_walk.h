/*
 * Shared argument structure for the self-contained synthetic random-walk
 * benchmark used to compare GCC/C and LLVM/Rust Tally throughput.
 */
#ifndef _SELF_WALK_H
#define _SELF_WALK_H

#include <stdint.h>

struct self_walk_args {
    uint32_t current_node;
    uint32_t seed;
    uint64_t vertices_walked;
};

#endif
