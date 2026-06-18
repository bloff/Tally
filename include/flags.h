/*
 * Compile-time feature toggles for the GCC tally plugin and minithread runtime.
 */
#ifdef GCCTALLY_DEBUG
#define gcctally_DEBUG
#endif

#ifdef GCCTALLY_DEBUG_RUNTIME
#define gcctally_DEBUG_RUNTIME
#endif

#define gcctally_branch_prediction
