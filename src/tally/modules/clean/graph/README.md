# Graph Module Fixtures

This folder contains the clean, host-side graph module implementation and a
small graph generator retained from the original project. The runtime examples
and tests use the repository-level `data/graph.txt` fixture by default.

`gen_graph.py` can still generate additional graph fixtures, but its output
format may need adaptation before it is consumed by `load_graph`, which expects
one adjacency-list row per node.
