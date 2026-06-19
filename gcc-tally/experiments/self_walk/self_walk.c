/*
 * Host-side self-contained random-walk benchmark. It runs k instrumented C
 * minithreads with the same budget until they collectively traverse a requested
 * number of synthetic graph edges.
 */
#define _POSIX_C_SOURCE 199309L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "minithread.h"
#include "self_walk.h"

#ifndef TALLY_DEFAULT_TARGET_EDGES
#define TALLY_DEFAULT_TARGET_EDGES 50000000ULL
#endif

static bool parse_bool_arg(const char *value){
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static int64_t parse_positive_i64(const char *value, int64_t fallback){
    char *end = NULL;
    int64_t parsed = strtoll(value, &end, 10);
    if(end == value || *end != '\0' || parsed <= 0){
        return fallback;
    }
    return parsed;
}

static size_t parse_positive_size(const char *value, size_t fallback){
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if(end == value || *end != '\0' || parsed == 0){
        return fallback;
    }
    return (size_t)parsed;
}

static uint64_t parse_positive_u64(const char *value, uint64_t fallback){
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if(end == value || *end != '\0' || parsed == 0){
        return fallback;
    }
    return (uint64_t)parsed;
}

static uint64_t target_for_thread(uint64_t total_edges, size_t thread_count, size_t index){
    uint64_t base = total_edges / (uint64_t)thread_count;
    uint64_t remainder = total_edges % (uint64_t)thread_count;
    return base + (index < remainder ? 1u : 0u);
}

static double elapsed_seconds(struct timespec start, struct timespec end){
    return (double)(end.tv_sec - start.tv_sec) + ((double)(end.tv_nsec - start.tv_nsec) / 1000000000.0);
}

static uint64_t charged_budget_for_cycle(Minithread thread, int64_t budget){
    int64_t charged = budget;
    if(thread->state == MINITHREAD_FORCE_YIELD){
        charged = budget - thread->cycles_left;
    }else if(thread->cycles_left > 0 && thread->cycles_left < budget){
        charged = budget - thread->cycles_left;
    }
    return charged > 0 ? (uint64_t)charged : 0;
}

int main(int argc, char* argv[]){
    size_t thread_count = 10;
    int64_t budget = 100;
    uint64_t target_edges = TALLY_DEFAULT_TARGET_EDGES;
    size_t stack_words = 1 << 10;
    bool summary_only = false;

    if(argc > 1) thread_count = parse_positive_size(argv[1], thread_count);
    if(argc > 2) budget = parse_positive_i64(argv[2], budget);
    if(argc > 3) target_edges = parse_positive_u64(argv[3], target_edges);
    if(argc > 4) stack_words = parse_positive_size(argv[4], stack_words);
    if(argc > 5) summary_only = parse_bool_arg(argv[5]);

    struct minithreadFuncOpt fOpt;
    fOpt.file_name = "experiments/self_walk/instrumented/self_walk";
    fOpt.func_name = "run_self_walk";
    const char *assume_compiled = getenv("TALLY_ASSUME_COMPILED");
    fOpt.compiled = assume_compiled != NULL && assume_compiled[0] != '\0' && assume_compiled[0] != '0';

    Minithread *threads = calloc(thread_count, sizeof(Minithread));
    struct self_walk_args *args = calloc(thread_count, sizeof(struct self_walk_args));
    bool *done = calloc(thread_count, sizeof(bool));
    uint64_t *cycles_run = summary_only ? NULL : calloc(thread_count, sizeof(uint64_t));
    if(threads == NULL || args == NULL || done == NULL || (!summary_only && cycles_run == NULL)){
        fprintf(stderr, "failed to allocate benchmark state\n");
        return 1;
    }

    size_t active_threads = 0;
    for(size_t i = 0; i < thread_count; i++){
        args[i].current_node = (uint32_t)i & 1023u;
        args[i].seed = 0x9e3779b9u ^ ((uint32_t)i * 0x85ebca6bu);
        args[i].vertices_walked = 0;
        args[i].target_vertices = target_for_thread(target_edges, thread_count, i);
        done[i] = args[i].target_vertices == 0;
        if(!done[i]){
            active_threads++;
        }

        threads[i] = minithread_init(
            NULL,
            stack_words,
            &args[i],
            &fOpt,
            NULL,
            0,
            budget
        );
        fOpt.compiled = 1;
    }

    uint64_t scheduler_cycles = 0;
    uint64_t thread_cycles = 0;
    uint64_t budget_units_consumed = 0;
    struct timespec run_start;
    struct timespec run_end;
    clock_gettime(CLOCK_MONOTONIC, &run_start);
    while(active_threads > 0){
        scheduler_cycles++;
        for(size_t i = 0; i < thread_count; i++){
            if(done[i]){
                continue;
            }

            minithread_run_cycle(threads[i]);
            thread_cycles++;
            budget_units_consumed += charged_budget_for_cycle(threads[i], budget);
            if(!summary_only){
                cycles_run[i]++;
            }
            if(threads[i]->state == MINITHREAD_RETURNED || args[i].vertices_walked >= args[i].target_vertices){
                done[i] = true;
                active_threads--;
            }else if(threads[i]->state == MINITHREAD_ERRORED){
                fprintf(stderr, "thread %zu errored\n", i);
                return 1;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &run_end);

    uint64_t total_edges = 0;
    for(size_t i = 0; i < thread_count; i++){
        total_edges += args[i].vertices_walked;
    }

    printf("threads: %zu\n", thread_count);
    printf("budget_per_cycle: %" PRId64 "\n", budget);
    printf("target_edges: %" PRIu64 "\n", target_edges);
    printf("total_edges: %" PRIu64 "\n", total_edges);
    printf("run_seconds: %.9f\n", elapsed_seconds(run_start, run_end));
    printf("scheduler_cycles: %" PRIu64 "\n", scheduler_cycles);
    printf("thread_cycles: %" PRIu64 "\n", thread_cycles);
    printf("budget_units_consumed: %" PRIu64 "\n", budget_units_consumed);
    printf("stack_words: %zu\n", stack_words);
    printf("stack_bytes: %zu\n", stack_words * sizeof(void*));
    printf("summary_only: %d\n", summary_only ? 1 : 0);
    printf("\n");

    if(summary_only){
        return 0;
    }

    printf("thread,budget_per_cycle,target_edges,vertices_walked,cycles_run\n");

    for(size_t i = 0; i < thread_count; i++){
        printf("%zu,%" PRId64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
            i,
            budget,
            args[i].target_vertices,
            args[i].vertices_walked,
            cycles_run[i]
        );
    }

    return 0;
}
