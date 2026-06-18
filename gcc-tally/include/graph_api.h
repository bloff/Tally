/*
 * Instrumented graph module API callable from minithread workloads.
 */
#ifndef GRAPH_API
#define GRAPH_API

#include "minithread_api.h"
#include "graph.h"

void add_hop();
int get_n_nodes();
int get_n_neighbours(int node);
int get_x_neighbour(int node, int index);

#endif //GRAPH_API
