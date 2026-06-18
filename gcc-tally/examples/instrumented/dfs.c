/*
 * Instrumented example workload that traverses the graph module with
 * depth-first search from inside a Tally minithread.
 */
#include <stdio.h>
#include <stdint.h>

#include "graph_api.h"
#include "graph_func.h"

extern int64_t counter;

int dfs(int u, int end, int* visited){
    //printf("going to %d\n", u);
    visited[u] = 1;
    if(u == end){
            printf("found destination %d %d --------------------------------------\n", u, end);
            return 1;
    }

    int n = get_n_neighbours(u);
    for(int i = 0; i < n; i++){
        int v = get_x_neighbour(u, i);
        //printf("neighbour %d %d\n", v, visited[v]);
        if(visited[v] == 0)
            if(dfs(v, end, visited)) return 1;
    }

    return 0;
}

void run_dfs(void* args){
    struct graph_args *input = (struct graph_args*) args;

    int start = input->start;
    int dest = input->dest;

    int n = get_n_nodes();
    //printf("nodes: %d %d %d %d\n", n, get_n_nodes(), N_G, dest);
    int visited[n];
    //int a;

    printf("filling visited %d\n", visited[9]);
    
    for(int i = 0; i < n; i++){
            //printf("i: %d\n", i);
            visited[i] = 0;
            //a = i;
    }

    printf("calling dfs\n");

    dfs(start, dest, visited);
    return; 
}
