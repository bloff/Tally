/*
 * Clean-side graph module implementation: loads graph data and initializes
 * per-minithread graph counters.
 */
#include "graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int N_G;
int **G;
int *Glen;

void init_graph(void* module_struct, void* args_ptr){
	struct graph_struct* g = (struct graph_struct*) module_struct;

	//no memory needed in the stack of the thread
	g->module.size = 0; 
	g->hops = 0;
}

void load_graph(char* path){
	printf("path %s\n", path);

	FILE* fp;
	char* line;
	fp = fopen(path, "r");

	if(fp == NULL){
		printf("cannot open file\n");
		return;
	}

	//number of nodes in graph
	fscanf(fp, "%d", &N_G);
	printf("nodes: %d\n", N_G);

	G = malloc(sizeof(int*) * N_G);
	Glen = malloc(sizeof(int) * N_G);

	for(int i = 0; i < N_G; i++){
		int k;
		fscanf(fp, "%d", &k);

		G[i] = malloc(sizeof(int) * k);
		Glen[i] = k;

		for(int j = 0; j < k; j++) fscanf(fp, "%d", &G[i][j]);
	}
	
	/*
	for(int i = 0; i < N_G; i++){
		printf("node %d:\n", i);
		for(int j = 0; j < Glen[i]; j++){
			printf("%d ", G[i][j]);
		}
		printf("\n");
	}
	*/
	fclose(fp);
}

