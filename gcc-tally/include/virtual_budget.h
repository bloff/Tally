/*
 * Virtual-budget helpers for mapping a calibrated CPU share onto GCC Tally's
 * internal per-cycle budget units.
 */
#ifndef TALLY_VIRTUAL_BUDGET_H
#define TALLY_VIRTUAL_BUDGET_H

#include <stdint.h>
#include <stddef.h>

#include "minithread.h"

#if __cplusplus
extern "C" {
#endif

typedef struct tally_virtual_calibration {
    double seconds_per_budget_unit;
    double seconds_per_activation;
    double seconds_per_scheduler_round;
    int64_t min_internal_budget;
    int64_t max_internal_budget;
} TallyVirtualCalibration;

typedef struct tally_virtual_thread {
    Minithread thread;
    double cpu_share;
    double credit_seconds;
    int64_t last_budget;
    uint64_t activations;
    uint64_t skipped_cycles;
    uint64_t completed_cycles;
} TallyVirtualThread;

typedef enum tally_virtual_run_result {
    TALLY_VIRTUAL_NOT_READY = 0,
    TALLY_VIRTUAL_RAN = 1,
    TALLY_VIRTUAL_FINISHED = 2,
    TALLY_VIRTUAL_ERRORED = 3
} TallyVirtualRunResult;

TallyVirtualCalibration tally_virtual_default_calibration(void);
double tally_virtual_context_switch_budget_units(const TallyVirtualCalibration *calibration);
double tally_virtual_scheduler_round_budget_units(const TallyVirtualCalibration *calibration);
int64_t tally_virtual_budget_from_seconds(const TallyVirtualCalibration *calibration, double credit_seconds);

void tally_virtual_thread_init(TallyVirtualThread *virtual_thread, Minithread thread, double cpu_share);
void tally_virtual_thread_add_credit(TallyVirtualThread *virtual_thread, double elapsed_seconds);
void tally_virtual_thread_charge_scheduler_round(
    TallyVirtualThread *virtual_thread,
    const TallyVirtualCalibration *calibration,
    size_t participating_threads
);
TallyVirtualRunResult tally_virtual_thread_run_ready(
    TallyVirtualThread *virtual_thread,
    const TallyVirtualCalibration *calibration
);

#if __cplusplus
}
#endif

#endif
