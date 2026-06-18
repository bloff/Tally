/*
 * Internal linked-list helpers used by the instrumented SHMALL heap module.
 */
#ifndef LLIST_H
#define LLIST_H

#include "shmall.h"
#include <stdint.h>

void add_node(bin_t *bin, node_t *node);

void remove_node(bin_t *bin, node_t *node);

node_t *get_best_fit(bin_t *list, size_t size);
node_t *get_last_node(bin_t *list);

node_t *next(node_t *current);
node_t *prev(node_t *current);

#endif
