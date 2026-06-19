/*
 * Virtual-budget conversion and scheduling helpers for the GCC minithread
 * runtime. This file owns both the conversion math and a built-in amortized
 * calibration probe.
 */
#define _POSIX_C_SOURCE 199309L

#include "virtual_budget.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <time.h>

static const double DEFAULT_SECONDS_PER_BUDGET_UNIT = 1.0e-9;
static const int64_t DEFAULT_MIN_INTERNAL_BUDGET = 1;
static const int64_t DEFAULT_MAX_INTERNAL_BUDGET = INT64_MAX / 4;
static const double DEFAULT_CALIBRATION_SECONDS = 30.0;
static const uint64_t DEFAULT_CALIBRATION_WORK = 200000;
static const uint64_t DEFAULT_CALIBRATION_STACK_WORDS = 1024;
static const char *CALIBRATION_FILE_HEADER = "tally_virtual_calibration_v1";

struct tally_virtual_probe_args {
    uint64_t work_done;
    uint64_t target_work;
    uint64_t state;
};

struct tally_virtual_sample {
    double run_seconds;
    uint64_t budget_units_consumed;
    uint64_t thread_cycles;
    uint64_t scheduler_cycles;
};

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

static double elapsed_seconds(struct timespec start, struct timespec end){
    return (double)(end.tv_sec - start.tv_sec) + ((double)(end.tv_nsec - start.tv_nsec) / 1000000000.0);
}

static uint64_t charged_budget_for_thread(Minithread thread, int64_t budget){
    int64_t charged = budget;
    if(thread->state == MINITHREAD_FORCE_YIELD){
        charged = budget - thread->cycles_left;
    }else if(thread->cycles_left > 0 && thread->cycles_left < budget){
        charged = budget - thread->cycles_left;
    }
    return charged > 0 ? (uint64_t)charged : 0;
}

static uint64_t target_for_thread(uint64_t total_work, size_t thread_count, size_t index){
    uint64_t base = total_work / (uint64_t)thread_count;
    uint64_t remainder = total_work % (uint64_t)thread_count;
    return base + (index < remainder ? 1u : 0u);
}

static int probe_shared_object_exists(void){
    char path[4096];
    int written = snprintf(
        path,
        sizeof(path),
        "%s/dl/src/tally/runtime/instrumented/virtual_budget_probe.so",
        TALLY_SOURCE_DIR
    );
    if(written < 0 || (size_t)written >= sizeof(path)){
        return 0;
    }

    FILE *file = fopen(path, "r");
    if(file == NULL){
        return 0;
    }
    fclose(file);
    return 1;
}

static int run_probe_sample(
    size_t thread_count,
    int64_t budget,
    uint64_t target_work,
    uint64_t stack_words,
    struct tally_virtual_sample *sample
){
    static int probe_compiled = -1;
    if(probe_compiled < 0){
        probe_compiled = probe_shared_object_exists();
    }

    Minithread *threads = calloc(thread_count, sizeof(Minithread));
    struct tally_virtual_probe_args *args = calloc(thread_count, sizeof(struct tally_virtual_probe_args));
    bool *done = calloc(thread_count, sizeof(bool));
    if(threads == NULL || args == NULL || done == NULL){
        free(threads);
        free(args);
        free(done);
        return -1;
    }

    struct minithreadFuncOpt fOpt;
    fOpt.file_name = "src/tally/runtime/instrumented/virtual_budget_probe";
    fOpt.func_name = "tally_virtual_budget_probe";
    fOpt.compiled = probe_compiled;

    size_t active_threads = 0;
    for(size_t i = 0; i < thread_count; i++){
        args[i].work_done = 0;
        args[i].target_work = target_for_thread(target_work, thread_count, i);
        args[i].state = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)i * 0xbf58476d1ce4e5b9ULL);
        done[i] = args[i].target_work == 0;
        if(!done[i]){
            active_threads++;
        }

        threads[i] = minithread_init(NULL, stack_words, &args[i], &fOpt, NULL, 0, budget);
        probe_compiled = 1;
        fOpt.compiled = 1;
    }

    uint64_t scheduler_cycles = 0;
    uint64_t thread_cycles = 0;
    uint64_t budget_units_consumed = 0;
    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while(active_threads > 0){
        scheduler_cycles++;
        for(size_t i = 0; i < thread_count; i++){
            if(done[i]){
                continue;
            }
            if(threads[i]->state == MINITHREAD_RETURNED){
                done[i] = true;
                active_threads--;
                continue;
            }
            if(threads[i]->state == MINITHREAD_ERRORED){
                for(size_t j = 0; j < thread_count; j++){
                    if(threads[j] != NULL){
                        minithread_join(threads[j]);
                        free(threads[j]);
                    }
                }
                free(threads);
                free(args);
                free(done);
                return -1;
            }

            minithread_run_cycle(threads[i]);
            thread_cycles++;
            budget_units_consumed += charged_budget_for_thread(threads[i], budget);

            if(threads[i]->state == MINITHREAD_RETURNED || args[i].work_done >= args[i].target_work){
                done[i] = true;
                active_threads--;
            }else if(threads[i]->state == MINITHREAD_ERRORED){
                for(size_t j = 0; j < thread_count; j++){
                    if(threads[j] != NULL){
                        minithread_join(threads[j]);
                        free(threads[j]);
                    }
                }
                free(threads);
                free(args);
                free(done);
                return -1;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    sample->run_seconds = elapsed_seconds(start, end);
    sample->budget_units_consumed = budget_units_consumed;
    sample->thread_cycles = thread_cycles;
    sample->scheduler_cycles = scheduler_cycles;

    for(size_t i = 0; i < thread_count; i++){
        minithread_join(threads[i]);
        free(threads[i]);
    }
    free(threads);
    free(args);
    free(done);
    return 0;
}

static int solve_linear_system_4(double matrix[4][5], double result[4]){
    for(size_t column = 0; column < 4; column++){
        size_t pivot = column;
        double pivot_abs = fabs(matrix[column][column]);
        for(size_t row = column + 1; row < 4; row++){
            double value = fabs(matrix[row][column]);
            if(value > pivot_abs){
                pivot = row;
                pivot_abs = value;
            }
        }
        if(pivot_abs < 1.0e-18){
            return -1;
        }
        if(pivot != column){
            for(size_t j = column; j < 5; j++){
                double tmp = matrix[column][j];
                matrix[column][j] = matrix[pivot][j];
                matrix[pivot][j] = tmp;
            }
        }

        double pivot_value = matrix[column][column];
        for(size_t j = column; j < 5; j++){
            matrix[column][j] /= pivot_value;
        }

        for(size_t row = 0; row < 4; row++){
            if(row == column){
                continue;
            }
            double factor = matrix[row][column];
            if(factor == 0.0){
                continue;
            }
            for(size_t j = column; j < 5; j++){
                matrix[row][j] -= factor * matrix[column][j];
            }
        }
    }

    for(size_t i = 0; i < 4; i++){
        result[i] = matrix[i][4];
    }
    return 0;
}

static int fit_samples(
    const struct tally_virtual_sample *samples,
    size_t sample_count,
    TallyVirtualCalibration *calibration
){
    double scales[4] = {0.0, 0.0, 0.0, 0.0};
    for(size_t i = 0; i < sample_count; i++){
        double features[4] = {
            (double)samples[i].budget_units_consumed,
            (double)samples[i].thread_cycles,
            (double)samples[i].scheduler_cycles,
            1.0,
        };
        for(size_t j = 0; j < 4; j++){
            scales[j] += features[j] * features[j];
        }
    }
    for(size_t j = 0; j < 4; j++){
        scales[j] = scales[j] > 0.0 ? sqrt(scales[j]) : 1.0;
    }

    double matrix[4][5] = {{0.0}};
    for(size_t i = 0; i < sample_count; i++){
        double y = samples[i].run_seconds;
        double features[4] = {
            (double)samples[i].budget_units_consumed / scales[0],
            (double)samples[i].thread_cycles / scales[1],
            (double)samples[i].scheduler_cycles / scales[2],
            1.0 / scales[3],
        };
        for(size_t row = 0; row < 4; row++){
            matrix[row][4] += features[row] * y;
            for(size_t col = 0; col < 4; col++){
                matrix[row][col] += features[row] * features[col];
            }
        }
    }
    for(size_t i = 0; i < 4; i++){
        matrix[i][i] += 1.0e-10;
    }

    double scaled_solution[4] = {0.0};
    if(solve_linear_system_4(matrix, scaled_solution) != 0){
        return -1;
    }

    double seconds_per_budget_unit = scaled_solution[0] / scales[0];
    double seconds_per_activation = scaled_solution[1] / scales[1];
    double seconds_per_scheduler_round = scaled_solution[2] / scales[2];

    uint64_t total_budget_units = 0;
    double total_seconds = 0.0;
    for(size_t i = 0; i < sample_count; i++){
        total_budget_units += samples[i].budget_units_consumed;
        total_seconds += samples[i].run_seconds;
    }
    if(seconds_per_budget_unit <= 0.0 && total_budget_units > 0){
        seconds_per_budget_unit = total_seconds / (double)total_budget_units;
    }

    calibration->seconds_per_budget_unit = positive_or(seconds_per_budget_unit, DEFAULT_SECONDS_PER_BUDGET_UNIT);
    calibration->seconds_per_activation = seconds_per_activation > 0.0 ? seconds_per_activation : 0.0;
    calibration->seconds_per_scheduler_round = seconds_per_scheduler_round > 0.0 ? seconds_per_scheduler_round : 0.0;
    calibration->min_internal_budget = DEFAULT_MIN_INTERNAL_BUDGET;
    calibration->max_internal_budget = DEFAULT_MAX_INTERNAL_BUDGET;
    return 0;
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

TallyVirtualCalibrationConfig tally_virtual_default_calibration_config(void){
    TallyVirtualCalibrationConfig config = {
        .target_seconds = DEFAULT_CALIBRATION_SECONDS,
        .work_per_sample = DEFAULT_CALIBRATION_WORK,
        .stack_words = DEFAULT_CALIBRATION_STACK_WORDS,
    };
    return config;
}

int tally_virtual_calibrate(
    const TallyVirtualCalibrationConfig *config,
    TallyVirtualCalibration *calibration
){
    if(calibration == NULL){
        return -1;
    }

    TallyVirtualCalibrationConfig effective =
        config != NULL ? *config : tally_virtual_default_calibration_config();
    if(effective.work_per_sample == 0){
        effective.work_per_sample = DEFAULT_CALIBRATION_WORK;
    }
    if(effective.stack_words == 0){
        effective.stack_words = DEFAULT_CALIBRATION_STACK_WORDS;
    }

    static const int64_t budgets[] = {50, 100, 500, 1000};
    static const size_t thread_counts[] = {1, 2, 5, 10};
    enum { MAX_SAMPLES = 512 };
    struct tally_virtual_sample samples[MAX_SAMPLES];
    size_t sample_count = 0;

    struct timespec start;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        for(size_t t = 0; t < sizeof(thread_counts) / sizeof(thread_counts[0]); t++){
            for(size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++){
                if(sample_count >= MAX_SAMPLES){
                    break;
                }
                if(run_probe_sample(
                    thread_counts[t],
                    budgets[b],
                    effective.work_per_sample,
                    effective.stack_words,
                    &samples[sample_count]
                ) != 0){
                    return -1;
                }
                sample_count++;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while(
        sample_count < MAX_SAMPLES &&
        effective.target_seconds > 0.0 &&
        elapsed_seconds(start, now) < effective.target_seconds
    );

    if(sample_count < 4){
        return -1;
    }
    return fit_samples(samples, sample_count, calibration);
}

int tally_virtual_calibration_write_file(
    const char *path,
    const TallyVirtualCalibration *calibration
){
    if(path == NULL || calibration == NULL){
        return -1;
    }

    FILE *file = fopen(path, "w");
    if(file == NULL){
        return -1;
    }

    int ok = fprintf(
        file,
        "%s\n"
        "seconds_per_budget_unit=%.17g\n"
        "seconds_per_activation=%.17g\n"
        "seconds_per_scheduler_round=%.17g\n"
        "min_internal_budget=%" PRId64 "\n"
        "max_internal_budget=%" PRId64 "\n",
        CALIBRATION_FILE_HEADER,
        calibration->seconds_per_budget_unit,
        calibration->seconds_per_activation,
        calibration->seconds_per_scheduler_round,
        calibration->min_internal_budget,
        calibration->max_internal_budget
    ) > 0;

    if(fclose(file) != 0){
        ok = 0;
    }
    return ok ? 0 : -1;
}

int tally_virtual_calibration_read_file(
    const char *path,
    TallyVirtualCalibration *calibration
){
    if(path == NULL || calibration == NULL){
        return -1;
    }

    FILE *file = fopen(path, "r");
    if(file == NULL){
        return -1;
    }

    TallyVirtualCalibration parsed = tally_virtual_default_calibration();
    char line[256];
    int saw_header = 0;
    int fields = 0;
    while(fgets(line, sizeof(line), file) != NULL){
        line[strcspn(line, "\r\n")] = '\0';
        if(line[0] == '\0' || line[0] == '#'){
            continue;
        }
        if(!saw_header){
            if(strcmp(line, CALIBRATION_FILE_HEADER) != 0){
                fclose(file);
                return -1;
            }
            saw_header = 1;
            continue;
        }

        char key[128];
        char value[128];
        if(sscanf(line, "%127[^=]=%127s", key, value) != 2){
            fclose(file);
            return -1;
        }
        if(strcmp(key, "seconds_per_budget_unit") == 0){
            parsed.seconds_per_budget_unit = strtod(value, NULL);
            fields++;
        }else if(strcmp(key, "seconds_per_activation") == 0){
            parsed.seconds_per_activation = strtod(value, NULL);
            fields++;
        }else if(strcmp(key, "seconds_per_scheduler_round") == 0){
            parsed.seconds_per_scheduler_round = strtod(value, NULL);
            fields++;
        }else if(strcmp(key, "min_internal_budget") == 0){
            parsed.min_internal_budget = strtoll(value, NULL, 10);
            fields++;
        }else if(strcmp(key, "max_internal_budget") == 0){
            parsed.max_internal_budget = strtoll(value, NULL, 10);
            fields++;
        }
    }
    fclose(file);

    if(!saw_header || fields < 5 || parsed.seconds_per_budget_unit <= 0.0 ||
       parsed.min_internal_budget <= 0 || parsed.max_internal_budget < parsed.min_internal_budget){
        return -1;
    }

    *calibration = parsed;
    return 0;
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
    virtual_thread->last_budget_units_consumed = 0;
    virtual_thread->activations = 0;
    virtual_thread->skipped_cycles = 0;
    virtual_thread->completed_cycles = 0;
    virtual_thread->budget_units_consumed = 0;
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
    if(virtual_thread->thread == NULL){
        return TALLY_VIRTUAL_ERRORED;
    }
    if(virtual_thread->thread->state == MINITHREAD_RETURNED){
        return TALLY_VIRTUAL_FINISHED;
    }
    if(virtual_thread->thread->state == MINITHREAD_ERRORED){
        return TALLY_VIRTUAL_ERRORED;
    }

    int64_t budget = tally_virtual_budget_from_seconds(calibration, virtual_thread->credit_seconds);
    virtual_thread->last_budget = budget;
    if(budget <= 0){
        virtual_thread->skipped_cycles++;
        return TALLY_VIRTUAL_NOT_READY;
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
    virtual_thread->last_budget_units_consumed = charged_budget;
    virtual_thread->budget_units_consumed += (uint64_t)charged_budget;
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

void tally_virtual_adaptive_state_init(TallyVirtualAdaptiveState *state){
    if(state == NULL){
        return;
    }
    state->smoothing = 0.10;
    state->min_observation_seconds = 0.005;
    state->accumulated_observed_seconds = 0.0;
    state->accumulated_budget_units = 0;
    state->accumulated_activations = 0;
    state->accumulated_scheduler_rounds = 0;
    state->observations = 0;
    state->updates = 0;
    state->last_sample_seconds_per_budget_unit = 0.0;
}

void tally_virtual_adaptive_observe(
    TallyVirtualAdaptiveState *state,
    TallyVirtualCalibration *calibration,
    double observed_seconds,
    uint64_t budget_units_consumed,
    uint64_t activations,
    uint64_t scheduler_rounds
){
    if(state == NULL || calibration == NULL || observed_seconds <= 0.0){
        return;
    }

    state->accumulated_observed_seconds += observed_seconds;
    state->accumulated_budget_units += budget_units_consumed;
    state->accumulated_activations += activations;
    state->accumulated_scheduler_rounds += scheduler_rounds;
    state->observations++;

    if(state->accumulated_observed_seconds < positive_or(state->min_observation_seconds, 0.005) ||
       state->accumulated_budget_units == 0){
        return;
    }

    double fixed_seconds =
        ((double)state->accumulated_activations * positive_or(calibration->seconds_per_activation, 0.0)) +
        ((double)state->accumulated_scheduler_rounds * positive_or(calibration->seconds_per_scheduler_round, 0.0));
    double work_seconds = state->accumulated_observed_seconds - fixed_seconds;
    if(work_seconds > 0.0){
        double sample_unit = work_seconds / (double)state->accumulated_budget_units;
        double old_unit = seconds_per_budget_unit_for(calibration);
        double min_sample = old_unit / 4.0;
        double max_sample = old_unit * 4.0;
        if(sample_unit < min_sample){
            sample_unit = min_sample;
        }else if(sample_unit > max_sample){
            sample_unit = max_sample;
        }

        double smoothing = state->smoothing;
        if(smoothing <= 0.0 || smoothing > 1.0){
            smoothing = 0.10;
        }
        calibration->seconds_per_budget_unit =
            (old_unit * (1.0 - smoothing)) + (sample_unit * smoothing);
        state->last_sample_seconds_per_budget_unit = sample_unit;
        state->updates++;
    }

    state->accumulated_observed_seconds = 0.0;
    state->accumulated_budget_units = 0;
    state->accumulated_activations = 0;
    state->accumulated_scheduler_rounds = 0;
}
