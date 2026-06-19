/*
 * Built-in instrumented probe used by tally_virtual_calibrate. It is deliberately
 * tiny and deterministic so calibration measures the runtime/instrumentation
 * contract rather than host callbacks or I/O.
 */
#include <stdint.h>

struct tally_virtual_probe_args {
    uint64_t work_done;
    uint64_t target_work;
    uint64_t state;
};

void tally_virtual_budget_probe(void *args){
    volatile struct tally_virtual_probe_args *input =
        (volatile struct tally_virtual_probe_args*)args;
    uint64_t work = input->work_done;
    uint64_t target = input->target_work;
    uint64_t state = input->state;

    while(work < target){
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        state ^= state >> 23;
        work++;
        input->state = state;
        input->work_done = work;
    }
}
