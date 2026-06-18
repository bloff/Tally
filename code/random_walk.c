#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "./../include/graph_api.h"
#include "./../include/graph_func.h"

#include "./../include/minithread_api.h"

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
