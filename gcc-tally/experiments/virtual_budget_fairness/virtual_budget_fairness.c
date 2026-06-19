/*
 * Native GCC/C virtual-budget fairness experiment. It calibrates the GCC Tally
 * runtime, creates many minithreads with random virtual CPU shares summing to a
 * requested total, runs timed rounds, and emits per-thread work data.
 */
#define _POSIX_C_SOURCE 199309L

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "minithread.h"
#include "self_walk.h"
#include "virtual_budget.h"

static uint64_t next_random(uint64_t *state){
    *state = (*state * 6364136223846793005ULL) + 1442695040888963407ULL;
    uint64_t value = *state;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return value;
}

static double random_unit(uint64_t *state){
    return (double)(next_random(state) >> 11) * (1.0 / 9007199254740992.0);
}

static double parse_double_arg(char **argv, int argc, int index, double fallback){
    if(index >= argc){
        return fallback;
    }
    char *end = NULL;
    double parsed = strtod(argv[index], &end);
    return end != argv[index] && *end == '\0' ? parsed : fallback;
}

static uint64_t parse_u64_arg(char **argv, int argc, int index, uint64_t fallback){
    if(index >= argc){
        return fallback;
    }
    char *end = NULL;
    unsigned long long parsed = strtoull(argv[index], &end, 10);
    return end != argv[index] && *end == '\0' ? (uint64_t)parsed : fallback;
}

static size_t parse_size_arg(char **argv, int argc, int index, size_t fallback){
    return (size_t)parse_u64_arg(argv, argc, index, fallback);
}

static bool parse_bool_arg(char **argv, int argc, int index, bool fallback){
    if(index >= argc){
        return fallback;
    }
    return argv[index][0] != '\0' && argv[index][0] != '0';
}

static double elapsed_seconds(struct timespec start, struct timespec end){
    return (double)(end.tv_sec - start.tv_sec) +
        ((double)(end.tv_nsec - start.tv_nsec) / 1000000000.0);
}

static struct timespec now_monotonic(void){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now;
}

static double choose_round_seconds(uint64_t *seed, double min_seconds, double max_seconds){
    if(max_seconds <= min_seconds){
        return min_seconds;
    }
    return min_seconds + (random_unit(seed) * (max_seconds - min_seconds));
}

int main(int argc, char **argv){
    size_t thread_count = parse_size_arg(argv, argc, 1, 50000);
    size_t rounds = parse_size_arg(argv, argc, 2, 3);
    double min_round_seconds = parse_double_arg(argv, argc, 3, 10.0);
    double max_round_seconds = parse_double_arg(argv, argc, 4, 20.0);
    double total_virtual_budget = parse_double_arg(argv, argc, 5, 0.5);
    uint64_t seed = parse_u64_arg(argv, argc, 6, 0x5eed1234ULL);
    double calibration_seconds = parse_double_arg(argv, argc, 7, 30.0);
    uint64_t calibration_work = parse_u64_arg(argv, argc, 8, 200000);
    uint64_t stack_words = parse_u64_arg(argv, argc, 9, 1024);
    bool adaptive = parse_bool_arg(argv, argc, 10, false);

    if(thread_count == 0 || rounds == 0 || total_virtual_budget <= 0.0 ||
       min_round_seconds <= 0.0 || max_round_seconds <= 0.0){
        fprintf(stderr, "invalid virtual-budget fairness arguments\n");
        return 1;
    }

    TallyVirtualCalibrationConfig config = tally_virtual_default_calibration_config();
    config.target_seconds = calibration_seconds;
    config.work_per_sample = calibration_work;
    config.stack_words = stack_words;

    TallyVirtualCalibration calibration;
    if(tally_virtual_calibrate(&config, &calibration) != 0){
        fprintf(stderr, "native GCC virtual calibration failed\n");
        return 1;
    }

    double *shares = calloc(thread_count, sizeof(double));
    TallyVirtualThread *virtual_threads = calloc(thread_count, sizeof(TallyVirtualThread));
    Minithread *threads = calloc(thread_count, sizeof(Minithread));
    struct self_walk_args *thread_args = calloc(thread_count, sizeof(struct self_walk_args));
    uint64_t *work_start = calloc(thread_count, sizeof(uint64_t));
    uint64_t *activation_start = calloc(thread_count, sizeof(uint64_t));
    uint64_t *budget_unit_start = calloc(thread_count, sizeof(uint64_t));
    if(shares == NULL || virtual_threads == NULL || threads == NULL ||
       thread_args == NULL || work_start == NULL || activation_start == NULL ||
       budget_unit_start == NULL){
        fprintf(stderr, "failed to allocate fairness state\n");
        return 1;
    }

    double total_weight = 0.0;
    for(size_t i = 0; i < thread_count; i++){
        double u = random_unit(&seed);
        double weight = exp(-3.0 + (6.0 * u));
        shares[i] = weight;
        total_weight += weight;
    }
    for(size_t i = 0; i < thread_count; i++){
        shares[i] = (shares[i] / total_weight) * total_virtual_budget;
    }

    struct minithreadFuncOpt fOpt;
    fOpt.file_name = "experiments/self_walk/instrumented/self_walk";
    fOpt.func_name = "run_self_walk";
    const char *assume_compiled = getenv("TALLY_ASSUME_COMPILED");
    fOpt.compiled = assume_compiled != NULL && assume_compiled[0] != '\0' && assume_compiled[0] != '0';

    for(size_t i = 0; i < thread_count; i++){
        thread_args[i].current_node = (uint32_t)i & 1023u;
        thread_args[i].seed = 0x9e3779b9u ^ ((uint32_t)i * 0x85ebca6bu);
        thread_args[i].vertices_walked = 0;
        thread_args[i].target_vertices = UINT64_MAX / 4;
        threads[i] = minithread_init(NULL, stack_words, &thread_args[i], &fOpt, NULL, 0, 1);
        fOpt.compiled = 1;
        tally_virtual_thread_init(&virtual_threads[i], threads[i], shares[i]);
    }

    printf("implementation: gcc-c\n");
    printf("thread_count: %zu\n", thread_count);
    printf("rounds: %zu\n", rounds);
    printf("min_round_seconds: %.9f\n", min_round_seconds);
    printf("max_round_seconds: %.9f\n", max_round_seconds);
    printf("total_virtual_budget: %.17g\n", total_virtual_budget);
    printf("adaptive: %d\n", adaptive ? 1 : 0);
    printf("calibration_seconds: %.9f\n", calibration_seconds);
    printf("seconds_per_budget_unit: %.17g\n", calibration.seconds_per_budget_unit);
    printf("seconds_per_activation: %.17g\n", calibration.seconds_per_activation);
    printf("seconds_per_scheduler_round: %.17g\n", calibration.seconds_per_scheduler_round);
    printf("context_switch_budget_units: %.17g\n", tally_virtual_context_switch_budget_units(&calibration));
    printf("\n");
    printf("thread,round,virtual_budget,activation_corrected_budget_seconds,work,activations,budget_units_consumed,round_seconds\n");

    TallyVirtualAdaptiveState adaptive_state;
    tally_virtual_adaptive_state_init(&adaptive_state);

    for(size_t round = 0; round < rounds; round++){
        for(size_t i = 0; i < thread_count; i++){
            virtual_threads[i].credit_seconds = 0.0;
            work_start[i] = thread_args[i].vertices_walked;
            activation_start[i] = virtual_threads[i].activations;
            budget_unit_start[i] = virtual_threads[i].budget_units_consumed;
        }

        double requested_seconds = choose_round_seconds(&seed, min_round_seconds, max_round_seconds);
        struct timespec round_start = now_monotonic();
        struct timespec last_tick = round_start;
        uint64_t scheduler_rounds = 0;

        while(true){
            struct timespec tick_start = now_monotonic();
            double elapsed = elapsed_seconds(round_start, tick_start);
            if(elapsed >= requested_seconds){
                break;
            }

            double delta = elapsed_seconds(last_tick, tick_start);
            if(delta <= 0.0){
                delta = 1.0e-9;
            }
            last_tick = tick_start;
            scheduler_rounds++;

            for(size_t i = 0; i < thread_count; i++){
                tally_virtual_thread_add_credit(&virtual_threads[i], delta);
                tally_virtual_thread_charge_scheduler_round(&virtual_threads[i], &calibration, thread_count);
            }

            uint64_t window_units = 0;
            uint64_t window_activations = 0;
            for(size_t i = 0; i < thread_count; i++){
                uint64_t before_units = virtual_threads[i].budget_units_consumed;
                uint64_t before_activations = virtual_threads[i].activations;
                TallyVirtualRunResult result =
                    tally_virtual_thread_run_ready(&virtual_threads[i], &calibration);
                if(result == TALLY_VIRTUAL_ERRORED){
                    fprintf(stderr, "thread %zu errored\n", i);
                    return 1;
                }
                window_units += virtual_threads[i].budget_units_consumed - before_units;
                window_activations += virtual_threads[i].activations - before_activations;
            }

            if(adaptive){
                struct timespec tick_end = now_monotonic();
                tally_virtual_adaptive_observe(
                    &adaptive_state,
                    &calibration,
                    elapsed_seconds(tick_start, tick_end),
                    window_units,
                    window_activations,
                    1
                );
            }
        }

        struct timespec round_end = now_monotonic();
        double actual_seconds = elapsed_seconds(round_start, round_end);
        for(size_t i = 0; i < thread_count; i++){
            uint64_t work = thread_args[i].vertices_walked - work_start[i];
            uint64_t activations = virtual_threads[i].activations - activation_start[i];
            uint64_t budget_units =
                virtual_threads[i].budget_units_consumed - budget_unit_start[i];
            double corrected_seconds =
                (shares[i] * actual_seconds) -
                ((double)activations * calibration.seconds_per_activation);
            if(corrected_seconds < 0.0){
                corrected_seconds = 0.0;
            }
            printf("%zu,%zu,%.17g,%.17g,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.9f\n",
                i,
                round,
                shares[i],
                corrected_seconds,
                work,
                activations,
                budget_units,
                actual_seconds
            );
        }
        fflush(stdout);
        (void)scheduler_rounds;
    }

    /*
     * These minithreads are intentionally still suspended inside instrumented
     * code. The legacy C runtime is not robust when tearing down thousands of
     * such stacks at once, and this short-lived experiment process exits after
     * emitting the CSV, so the OS reclaims the stacks.
     */
    free(shares);
    free(virtual_threads);
    free(threads);
    free(thread_args);
    free(work_start);
    free(activation_start);
    free(budget_unit_start);
    return 0;
}
