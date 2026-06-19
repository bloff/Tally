/*
 * Virtual-budget conversion and scheduling helpers for the GCC minithread
 * runtime. The empirical calibration is supplied by the benchmark harness; this
 * file only applies it.
 */
#include "virtual_budget.h"

#include <limits.h>

static const double DEFAULT_SECONDS_PER_BUDGET_UNIT = 1.0e-9;
static const int64_t DEFAULT_MIN_INTERNAL_BUDGET = 1;
static const int64_t DEFAULT_MAX_INTERNAL_BUDGET = INT64_MAX / 4;

static double positive_or(double value, double fallback){
    return value > 0.0 ? value : fallback;
}

static int64_t min_budget_for(const TallyVirtualCalibration *calibration){
    if(calibration == NULL || calibration->min_internal_budget <= 0){
        return DEFAULT_MIN_INTERNAL_BUDGET;
    }
    return calibration->min_internal_budget;
}

static int64_t max_budget_for(const TallyVirtualCalibration *calibration){
    if(calibration == NULL || calibration->max_internal_budget <= 0){
        return DEFAULT_MAX_INTERNAL_BUDGET;
    }
    return calibration->max_internal_budget;
}

static double seconds_per_budget_unit_for(const TallyVirtualCalibration *calibration){
    if(calibration == NULL){
        return DEFAULT_SECONDS_PER_BUDGET_UNIT;
    }
    return positive_or(calibration->seconds_per_budget_unit, DEFAULT_SECONDS_PER_BUDGET_UNIT);
}

TallyVirtualCalibration tally_virtual_default_calibration(void){
    TallyVirtualCalibration calibration = {
        .seconds_per_budget_unit = DEFAULT_SECONDS_PER_BUDGET_UNIT,
        .seconds_per_activation = 0.0,
        .seconds_per_scheduler_round = 0.0,
        .min_internal_budget = DEFAULT_MIN_INTERNAL_BUDGET,
        .max_internal_budget = DEFAULT_MAX_INTERNAL_BUDGET,
    };
    return calibration;
}

double tally_virtual_context_switch_budget_units(const TallyVirtualCalibration *calibration){
    double unit_seconds = seconds_per_budget_unit_for(calibration);
    double activation_seconds = calibration == NULL ? 0.0 : calibration->seconds_per_activation;
    return activation_seconds > 0.0 ? activation_seconds / unit_seconds : 0.0;
}

double tally_virtual_scheduler_round_budget_units(const TallyVirtualCalibration *calibration){
    double unit_seconds = seconds_per_budget_unit_for(calibration);
    double round_seconds = calibration == NULL ? 0.0 : calibration->seconds_per_scheduler_round;
    return round_seconds > 0.0 ? round_seconds / unit_seconds : 0.0;
}

int64_t tally_virtual_budget_from_seconds(const TallyVirtualCalibration *calibration, double credit_seconds){
    if(credit_seconds <= 0.0){
        return 0;
    }

    double activation_seconds = calibration == NULL ? 0.0 : calibration->seconds_per_activation;
    double available_seconds = credit_seconds - positive_or(activation_seconds, 0.0);
    if(available_seconds <= 0.0){
        return 0;
    }

    int64_t min_budget = min_budget_for(calibration);
    int64_t max_budget = max_budget_for(calibration);
    if(max_budget < min_budget){
        max_budget = min_budget;
    }

    double raw_budget = (available_seconds / seconds_per_budget_unit_for(calibration)) + 1.0e-9;
    if(raw_budget < (double)min_budget){
        return 0;
    }
    if(raw_budget > (double)max_budget){
        return max_budget;
    }
    return (int64_t)raw_budget;
}

void tally_virtual_thread_init(TallyVirtualThread *virtual_thread, Minithread thread, double cpu_share){
    if(virtual_thread == NULL){
        return;
    }

    virtual_thread->thread = thread;
    virtual_thread->cpu_share = cpu_share > 0.0 ? cpu_share : 0.0;
    virtual_thread->credit_seconds = 0.0;
    virtual_thread->last_budget = 0;
    virtual_thread->activations = 0;
    virtual_thread->skipped_cycles = 0;
    virtual_thread->completed_cycles = 0;
}

void tally_virtual_thread_add_credit(TallyVirtualThread *virtual_thread, double elapsed_seconds){
    if(virtual_thread == NULL || elapsed_seconds <= 0.0 || virtual_thread->cpu_share <= 0.0){
        return;
    }
    virtual_thread->credit_seconds += elapsed_seconds * virtual_thread->cpu_share;
}

void tally_virtual_thread_charge_scheduler_round(
    TallyVirtualThread *virtual_thread,
    const TallyVirtualCalibration *calibration,
    size_t participating_threads
){
    if(virtual_thread == NULL || participating_threads == 0){
        return;
    }

    double round_seconds = calibration == NULL ? 0.0 : calibration->seconds_per_scheduler_round;
    if(round_seconds <= 0.0){
        return;
    }

    virtual_thread->credit_seconds -= round_seconds / (double)participating_threads;
}

TallyVirtualRunResult tally_virtual_thread_run_ready(
    TallyVirtualThread *virtual_thread,
    const TallyVirtualCalibration *calibration
){
    if(virtual_thread == NULL){
        return TALLY_VIRTUAL_ERRORED;
    }

    int64_t budget = tally_virtual_budget_from_seconds(calibration, virtual_thread->credit_seconds);
    virtual_thread->last_budget = budget;
    if(budget <= 0){
        virtual_thread->skipped_cycles++;
        return TALLY_VIRTUAL_NOT_READY;
    }
    if(virtual_thread->thread == NULL){
        return TALLY_VIRTUAL_ERRORED;
    }

    minithread_change_cycles(virtual_thread->thread, budget);
    minithread_run_cycle(virtual_thread->thread);

    int64_t charged_budget = budget;
    if(virtual_thread->thread->state == MINITHREAD_FORCE_YIELD){
        charged_budget = budget - virtual_thread->thread->cycles_left;
    }else if(virtual_thread->thread->cycles_left > 0 && virtual_thread->thread->cycles_left < budget){
        charged_budget = budget - virtual_thread->thread->cycles_left;
    }
    if(charged_budget < 0){
        charged_budget = 0;
    }

    double activation_seconds = calibration == NULL ? 0.0 : calibration->seconds_per_activation;
    virtual_thread->credit_seconds -= positive_or(activation_seconds, 0.0);
    virtual_thread->credit_seconds -= (double)charged_budget * seconds_per_budget_unit_for(calibration);
    virtual_thread->activations++;

    if(virtual_thread->thread->state == MINITHREAD_RETURNED){
        return TALLY_VIRTUAL_FINISHED;
    }
    if(virtual_thread->thread->state == MINITHREAD_ERRORED){
        return TALLY_VIRTUAL_ERRORED;
    }

    virtual_thread->completed_cycles++;
    return TALLY_VIRTUAL_RAN;
}
