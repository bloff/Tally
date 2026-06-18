/*
 * Unit test for the clean graph module loader using the repository's checked-in
 * graph fixture.
 */
#include <stdio.h>

#include "graph.h"

#ifndef TALLY_SOURCE_DIR
#define TALLY_SOURCE_DIR "."
#endif

int main(void) {
    load_graph(TALLY_SOURCE_DIR "/data/graph.txt");

    if (N_G != 1000) {
        fprintf(stderr, "expected 1000 graph nodes, got %d\n", N_G);
        return 1;
    }

    if (Glen == NULL || G == NULL) {
        fprintf(stderr, "graph arrays were not initialized\n");
        return 1;
    }

    if (Glen[0] != 8) {
        fprintf(stderr, "expected node 0 to have 8 neighbours, got %d\n", Glen[0]);
        return 1;
    }

    if (G[0][0] != 947 || G[0][7] != 591) {
        fprintf(stderr, "unexpected node 0 adjacency data\n");
        return 1;
    }

    return 0;
}
