/*
 * Namespace-local ABI bridge for std-using LLVM Tally workloads. Workloads link
 * against this tiny shared object inside their dlmopen namespace; the manager
 * installs function pointers that forward charges and allocation hooks back to
 * the base-namespace runtime.
 */
#include <stddef.h>
#include <stdint.h>

typedef void (*tally_charge_fn)(uint64_t cost);
typedef void *(*tally_alloc_fn)(uint64_t size, uint64_t align);
typedef void (*tally_dealloc_fn)(void *ptr, uint64_t size, uint64_t align);
typedef void *(*tally_realloc_fn)(
    void *ptr,
    uint64_t old_size,
    uint64_t align,
    uint64_t new_size);

typedef struct tally_abi_hooks {
    tally_charge_fn charge;
    tally_alloc_fn alloc;
    tally_dealloc_fn dealloc;
    tally_realloc_fn realloc;
} tally_abi_hooks;

static tally_abi_hooks hooks;

__attribute__((visibility("default"))) void tally_abi_set_hooks(
    const tally_abi_hooks *new_hooks)
{
    if (new_hooks == NULL) {
        hooks = (tally_abi_hooks){0};
        return;
    }

    hooks = *new_hooks;
}

__attribute__((visibility("default"))) void __tally_charge(uint64_t cost)
{
    if (hooks.charge != NULL) {
        hooks.charge(cost);
    }
}

__attribute__((visibility("default"))) void *__tally_alloc(
    uint64_t size,
    uint64_t align)
{
    if (hooks.alloc == NULL) {
        return NULL;
    }

    return hooks.alloc(size, align);
}

__attribute__((visibility("default"))) void __tally_dealloc(
    void *ptr,
    uint64_t size,
    uint64_t align)
{
    if (hooks.dealloc != NULL) {
        hooks.dealloc(ptr, size, align);
    }
}

__attribute__((visibility("default"))) void *__tally_realloc(
    void *ptr,
    uint64_t old_size,
    uint64_t align,
    uint64_t new_size)
{
    if (hooks.realloc == NULL) {
        return NULL;
    }

    return hooks.realloc(ptr, old_size, align, new_size);
}
