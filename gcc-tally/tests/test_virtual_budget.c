/*
 * Unit tests for the virtual-budget conversion helpers. These tests avoid
 * running instrumented code and focus on the accounting math used by callers.
 */
#include "virtual_budget.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

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
    assert(virtual_thread.budget_units_consumed == 0);

    tally_virtual_thread_charge_scheduler_round(&virtual_thread, &calibration, 4);
    assert(close_enough(virtual_thread.credit_seconds, 0.995));

    TallyVirtualRunResult result = tally_virtual_thread_run_ready(&virtual_thread, &calibration);
    assert(result == TALLY_VIRTUAL_ERRORED);
    assert(virtual_thread.last_budget == 0);

    tally_virtual_thread_init(&virtual_thread, NULL, 0.5);
    result = tally_virtual_thread_run_ready(&virtual_thread, &calibration);
    assert(result == TALLY_VIRTUAL_ERRORED);
    assert(virtual_thread.skipped_cycles == 0);

    const char *path = "/tmp/tally_virtual_calibration_test.txt";
    assert(tally_virtual_calibration_write_file(path, &calibration) == 0);
    TallyVirtualCalibration loaded = tally_virtual_default_calibration();
    assert(tally_virtual_calibration_read_file(path, &loaded) == 0);
    assert(close_enough(loaded.seconds_per_budget_unit, calibration.seconds_per_budget_unit));
    assert(close_enough(loaded.seconds_per_activation, calibration.seconds_per_activation));
    assert(close_enough(loaded.seconds_per_scheduler_round, calibration.seconds_per_scheduler_round));
    assert(loaded.min_internal_budget == calibration.min_internal_budget);
    assert(loaded.max_internal_budget == calibration.max_internal_budget);
    remove(path);

    TallyVirtualAdaptiveState adaptive;
    tally_virtual_adaptive_state_init(&adaptive);
    adaptive.min_observation_seconds = 0.0;
    double old_unit = calibration.seconds_per_budget_unit;
    tally_virtual_adaptive_observe(&adaptive, &calibration, 1.0, 500, 0, 0);
    assert(adaptive.updates == 1);
    assert(calibration.seconds_per_budget_unit > old_unit);

    TallyVirtualCalibrationConfig config = tally_virtual_default_calibration_config();
    config.target_seconds = 0.0;
    config.work_per_sample = 1000;
    TallyVirtualCalibration measured = tally_virtual_default_calibration();
    assert(tally_virtual_calibrate(&config, &measured) == 0);
    assert(measured.seconds_per_budget_unit > 0.0);
    assert(measured.min_internal_budget > 0);
    assert(measured.max_internal_budget >= measured.min_internal_budget);

    return 0;
}
