/*
 * Instrumented example workload that traverses the graph module with
 * breadth-first search from inside a Tally minithread.
 */
#include <stdio.h>
#include <stdint.h>

#include "graph_api.h"
#include "graph_func.h"

extern int64_t counter;

int bfs(int start, int dest){

    int n = get_n_nodes();
    
    int visited[n];
    for(int i = 0; i < n; i++) visited[i] = 0;
    
    int queue[n];
    int p0 = 0, p1 = 0;

    queue[p0++] = start;
    visited[start] = 1;

    while(p1 < p0){
        int u = queue[p1++];
        add_hop();
        printf("going: %d\n", u);

        if(u == dest){
            printf("dest found-------\n");
            return 1;
        }

        n = get_n_neighbours(u);
        for(int i = 0; i < n; i++){
            int v = get_x_neighbour(u, i);
            if(visited[v] == 0){
                queue[p0++] = v;
                visited[v] = 1;
            }
        }
    }

    return 0;
}

void run_bfs(void* args){

    struct graph_args *input = (struct graph_args*) args;

    int start = input->start;
    int dest = input->dest;


    bfs(start, dest);
}
