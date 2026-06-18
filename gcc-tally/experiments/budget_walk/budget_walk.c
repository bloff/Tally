/*
 * Host-side experiment that runs ten Tally minithreads with linearly
 * increasing budgets and reports how much graph-walk work each one completes.
 */
#define _POSIX_C_SOURCE 199309L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "graph.h"
#include "graph_api.h"
#include "minithread.h"
#include "minithread_api.h"
#include "random_walk_budget.h"

#ifndef TALLY_SOURCE_DIR
#define TALLY_SOURCE_DIR "."
#endif

#define N_THREADS 10

static int64_t parse_positive_i64(const char *value, int64_t fallback){
    char *end = NULL;
    int64_t parsed = strtoll(value, &end, 10);
    if(end == value || *end != '\0' || parsed <= 0){
        return fallback;
    }
    return parsed;
}

int main(int argc, char* argv[]){
    int64_t metacycles = 1000;
    int64_t base_budget = 100;
    int64_t budget_step = 100;

    if(argc > 1) metacycles = parse_positive_i64(argv[1], metacycles);
    if(argc > 2) base_budget = parse_positive_i64(argv[2], base_budget);
    if(argc > 3) budget_step = parse_positive_i64(argv[3], budget_step);

    struct minithreadFuncOpt fOpt;
    fOpt.file_name = "experiments/budget_walk/instrumented/random_walk_budget";
    fOpt.func_name = "run_walk_budget";
    const char *assume_compiled = getenv("TALLY_ASSUME_COMPILED");
    fOpt.compiled = assume_compiled != NULL && assume_compiled[0] != '\0' && assume_compiled[0] != '0';

    struct minithreadModuleOpt graphModule;
    graphModule.module_name = "graph";
    graphModule.init = init_graph;
    graphModule.init_args = NULL;
    graphModule.clean = NULL;
    graphModule.unique_id = 0;
    graphModule.struct_size = sizeof(struct graph_struct);

    struct minithreadModuleOpt* modules[1];
    modules[0] = &graphModule;

    load_graph(TALLY_SOURCE_DIR "/data/graph.txt");

    Minithread threads[N_THREADS];
    struct random_walk_budget_args args[N_THREADS];
    int64_t budgets[N_THREADS];

    int n_nodes = get_n_nodes();
    if(n_nodes <= 0){
        fprintf(stderr, "graph did not load\n");
        return 1;
    }

    for(int i = 0; i < N_THREADS; i++){
        budgets[i] = base_budget + (budget_step * i);
        args[i].current_node = i % n_nodes;
        args[i].seed = 0x9e3779b9u ^ (uint32_t)(i * 0x85ebca6bu);

        threads[i] = minithread_init(
            NULL,
            1 << 10,
            &args[i],
            &fOpt,
            modules,
            1,
            budgets[i]
        );
        fOpt.compiled = 1;
    }

    for(int64_t cycle = 0; cycle < metacycles; cycle++){
        for(int i = 0; i < N_THREADS; i++){
            minithread_run_cycle(threads[i]);
        }
    }

    int hops[N_THREADS];
    for(int i = 0; i < N_THREADS; i++){
        struct graph_struct *graph = (struct graph_struct*) minithread_m_find(threads[i], HASH("graph"));
        hops[i] = graph == NULL ? -1 : graph->hops;
    }

    int64_t baseline_budget = budgets[0];
    int baseline_hops = hops[0];

    printf("metacycles: %" PRId64 "\n", metacycles);
    printf("threads: %d\n", N_THREADS);
    printf("base_budget: %" PRId64 "\n", base_budget);
    printf("budget_step: %" PRId64 "\n", budget_step);
    printf("\n");
    printf("thread,budget_per_metacycle,total_budget,vertices_walked,vertices_per_metacycle,vertices_per_1000_cycles,budget_multiple,work_multiple,linearity_ratio\n");

    for(int i = 0; i < N_THREADS; i++){
        int64_t total_budget = budgets[i] * metacycles;
        double hops_per_metacycle = metacycles == 0 ? 0.0 : (double)hops[i] / (double)metacycles;
        double hops_per_1000_cycles = total_budget == 0 ? 0.0 : ((double)hops[i] * 1000.0) / (double)total_budget;
        double budget_multiple = baseline_budget == 0 ? 0.0 : (double)budgets[i] / (double)baseline_budget;
        double work_multiple = baseline_hops <= 0 ? 0.0 : (double)hops[i] / (double)baseline_hops;
        double linearity_ratio = budget_multiple == 0.0 ? 0.0 : work_multiple / budget_multiple;

        printf("%d,%" PRId64 ",%" PRId64 ",%d,%.6f,%.6f,%.6f,%.6f,%.6f\n",
            i,
            budgets[i],
            total_budget,
            hops[i],
            hops_per_metacycle,
            hops_per_1000_cycles,
            budget_multiple,
            work_multiple,
            linearity_ratio
        );
    }

    return 0;
}
