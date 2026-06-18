/*
 * Instrumented random-walk workload used by the demo executable and the
 * original graph benchmark experiments.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "graph_api.h"
#include "graph_func.h"

#include "minithread_api.h"

int walk(int u){
	while(1){
		int n = get_n_neighbours(u);
		int v_index = rand() % n;
		u = get_x_neighbour(u, v_index);
	}
}

void run_walk(void* args){
	time_t t;
	srand((unsigned) time(&t));
    walk(0);
}
