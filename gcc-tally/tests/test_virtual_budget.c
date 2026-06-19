/*
 * Unit tests for the virtual-budget conversion helpers. These tests avoid
 * running instrumented code and focus on the accounting math used by callers.
 */
#include "virtual_budget.h"

#include <assert.h>
#include <math.h>

static int close_enough(double left, double right){
    double diff = left > right ? left - right : right - left;
    return diff < 1.0e-12;
}

int main(void){
    TallyVirtualCalibration calibration = {
        .seconds_per_budget_unit = 0.001,
        .seconds_per_activation = 0.010,
        .seconds_per_scheduler_round = 0.020,
        .min_internal_budget = 5,
        .max_internal_budget = 100,
    };

    assert(close_enough(tally_virtual_context_switch_budget_units(&calibration), 10.0));
    assert(close_enough(tally_virtual_scheduler_round_budget_units(&calibration), 20.0));

    assert(tally_virtual_budget_from_seconds(&calibration, 0.010) == 0);
    assert(tally_virtual_budget_from_seconds(&calibration, 0.014) == 0);
    assert(tally_virtual_budget_from_seconds(&calibration, 0.015) == 5);
    assert(tally_virtual_budget_from_seconds(&calibration, 0.060) == 50);
    assert(tally_virtual_budget_from_seconds(&calibration, 1.000) == 100);

    TallyVirtualThread virtual_thread;
    tally_virtual_thread_init(&virtual_thread, NULL, 0.25);
    tally_virtual_thread_add_credit(&virtual_thread, 4.0);
    assert(close_enough(virtual_thread.credit_seconds, 1.0));

    tally_virtual_thread_charge_scheduler_round(&virtual_thread, &calibration, 4);
    assert(close_enough(virtual_thread.credit_seconds, 0.995));

    TallyVirtualRunResult result = tally_virtual_thread_run_ready(&virtual_thread, &calibration);
    assert(result == TALLY_VIRTUAL_ERRORED);
    assert(virtual_thread.last_budget == 100);

    tally_virtual_thread_init(&virtual_thread, NULL, 0.5);
    result = tally_virtual_thread_run_ready(&virtual_thread, &calibration);
    assert(result == TALLY_VIRTUAL_NOT_READY);
    assert(virtual_thread.skipped_cycles == 1);

    return 0;
}
