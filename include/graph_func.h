/*
 * Small graph traversal argument types used by the example workloads.
 */
#ifndef GRAPH_FUNC_
#define GRAPH_FUNC_

#include "graph.h"

typedef struct graph_struct* graph_t;
void _add_hop(graph_t g);

struct graph_args{
	int start;
	int dest;
};

#endif //GRAPH_FUNC_
