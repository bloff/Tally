#include "./../../../include/graph_api.h"
#include "./../../../include/minithread_api.h"
#include <stdio.h>

const int GRAPH_HASH = HASH_S("graph");

void add_hop(){
	struct graph_struct* g = minithread_find(GRAPH_HASH);
	if(g != NULL){
		g->hops++;
	}
}

int get_n_nodes(){
	return N_G;
}

int get_n_neighbours(int node){
	if(node >= N_G){
		printf("graph_module instrumented: return with error 1 %d\n", node);
		return -1;
	}
	return Glen[node];
}

int get_x_neighbour(int node, int index){
	if(node >= N_G){
		printf("graph_module instrumented: return with error 2 %d %d\n", node, N_G);
		return -1;
	}
	if(index >= Glen[node]){
		printf("graph_module instrumented: return with error 3 %d %d\n", index, Glen[node]);
		return -1;
	}

	//printf("node: %d index: %d\n", node, index);
	return G[node][index];
}
