/*
 * Namespace-local ABI bridge for std-using LLVM Tally workloads. Workloads link
 * against this tiny shared object inside their dlmopen namespace; the manager
 * installs function pointers that forward charges and allocation hooks back to
 * the base-namespace runtime.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

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

enum {
    TALLY_DEFAULT_ALIGN = 16,
};

typedef struct tally_alloc_header {
    uint64_t magic;
    uint64_t requested_size;
    uint64_t total_size;
    uint64_t total_align;
    void *base;
} tally_alloc_header;

static const uint64_t TALLY_ALLOC_MAGIC = UINT64_C(0x74616c6c79686561);

typedef void *(*real_malloc_fn)(size_t size);
typedef void *(*real_calloc_fn)(size_t count, size_t size);
typedef void *(*real_realloc_fn)(void *ptr, size_t size);
typedef void (*real_free_fn)(void *ptr);
typedef int (*real_posix_memalign_fn)(void **ptr, size_t align, size_t size);

static void *real_symbol(const char *name)
{
    return dlsym(RTLD_NEXT, name);
}

static void *real_malloc(size_t size)
{
    real_malloc_fn fn = (real_malloc_fn)real_symbol("malloc");
    return fn == NULL ? NULL : fn(size);
}

static void *real_calloc(size_t count, size_t size)
{
    real_calloc_fn fn = (real_calloc_fn)real_symbol("calloc");
    return fn == NULL ? NULL : fn(count, size);
}

static void *real_realloc(void *ptr, size_t size)
{
    real_realloc_fn fn = (real_realloc_fn)real_symbol("realloc");
    return fn == NULL ? NULL : fn(ptr, size);
}

static void real_free(void *ptr)
{
    real_free_fn fn = (real_free_fn)real_symbol("free");
    if (fn != NULL) {
        fn(ptr);
    }
}

static int real_posix_memalign(void **ptr, size_t align, size_t size)
{
    real_posix_memalign_fn fn = (real_posix_memalign_fn)real_symbol("posix_memalign");
    return fn == NULL ? ENOMEM : fn(ptr, align, size);
}

static int is_power_of_two_alignment(size_t align)
{
    return align != 0 && (align & (align - 1)) == 0;
}

static int is_valid_posix_alignment(size_t align)
{
    return align >= sizeof(void *) && (align & (align - 1)) == 0;
}

static uintptr_t align_up_uintptr(uintptr_t value, size_t align)
{
    uintptr_t mask = (uintptr_t)align - 1;
    return (value + mask) & ~mask;
}

static void *tally_allocate_aligned(size_t size, size_t align)
{
    if (!is_power_of_two_alignment(align)) {
        errno = EINVAL;
        return NULL;
    }

    if (hooks.alloc == NULL) {
        if (align <= TALLY_DEFAULT_ALIGN) {
            return real_malloc(size);
        }

        void *ptr = NULL;
        if (real_posix_memalign(&ptr, align, size) != 0) {
            errno = ENOMEM;
            return NULL;
        }
        return ptr;
    }

    size_t total = 0;
    if (__builtin_add_overflow(size, align, &total) ||
        __builtin_add_overflow(total, sizeof(tally_alloc_header), &total)) {
        errno = ENOMEM;
        return NULL;
    }

    void *base = hooks.alloc((uint64_t)total, TALLY_DEFAULT_ALIGN);
    if (base == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    uintptr_t raw = (uintptr_t)base + sizeof(tally_alloc_header);
    void *user = (void *)align_up_uintptr(raw, align);
    tally_alloc_header *header = ((tally_alloc_header *)user) - 1;
    header->magic = TALLY_ALLOC_MAGIC;
    header->requested_size = (uint64_t)size;
    header->total_size = (uint64_t)total;
    header->total_align = TALLY_DEFAULT_ALIGN;
    header->base = base;
    return user;
}

static tally_alloc_header *tally_header_for(void *ptr)
{
    if (ptr == NULL) {
        return NULL;
    }

    tally_alloc_header *header = ((tally_alloc_header *)ptr) - 1;
    if (header->magic != TALLY_ALLOC_MAGIC) {
        return NULL;
    }
    return header;
}

static void tally_deallocate(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    tally_alloc_header *header = tally_header_for(ptr);
    if (header == NULL) {
        real_free(ptr);
        return;
    }

    if (hooks.dealloc != NULL) {
        hooks.dealloc(header->base, header->total_size, header->total_align);
    }
}

static void *tally_reallocate(void *ptr, size_t size, size_t align)
{
    if (ptr == NULL) {
        return tally_allocate_aligned(size, align);
    }
    if (size == 0) {
        tally_deallocate(ptr);
        return NULL;
    }

    tally_alloc_header *header = tally_header_for(ptr);
    if (header == NULL) {
        return real_realloc(ptr, size);
    }

    void *new_ptr = tally_allocate_aligned(size, align);
    if (new_ptr == NULL) {
        return NULL;
    }

    size_t copy_size = header->requested_size < size ? header->requested_size : size;
    memcpy(new_ptr, ptr, copy_size);
    tally_deallocate(ptr);
    return new_ptr;
}

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

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    return tally_allocate_aligned(size, TALLY_DEFAULT_ALIGN);
}

__attribute__((visibility("default"))) void *calloc(size_t count, size_t size)
{
    size_t total = 0;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }

    if (hooks.alloc == NULL) {
        return real_calloc(count, size);
    }

    void *ptr = tally_allocate_aligned(total, TALLY_DEFAULT_ALIGN);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

__attribute__((visibility("default"))) void *realloc(void *ptr, size_t size)
{
    if (hooks.alloc == NULL) {
        return real_realloc(ptr, size);
    }

    return tally_reallocate(ptr, size, TALLY_DEFAULT_ALIGN);
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    if (hooks.dealloc == NULL) {
        real_free(ptr);
        return;
    }

    tally_deallocate(ptr);
}

__attribute__((visibility("default"))) int posix_memalign(
    void **ptr,
    size_t align,
    size_t size)
{
    if (ptr == NULL) {
        return EINVAL;
    }
    *ptr = NULL;

    if (!is_valid_posix_alignment(align)) {
        return EINVAL;
    }

    if (hooks.alloc == NULL) {
        return real_posix_memalign(ptr, align, size);
    }

    void *allocated = tally_allocate_aligned(size, align);
    if (allocated == NULL) {
        return errno == EINVAL ? EINVAL : ENOMEM;
    }

    *ptr = allocated;
    return 0;
}

__attribute__((visibility("default"))) void *aligned_alloc(
    size_t align,
    size_t size)
{
    if (!is_power_of_two_alignment(align)) {
        errno = EINVAL;
        return NULL;
    }
    if (size % align != 0) {
        errno = EINVAL;
        return NULL;
    }

    return tally_allocate_aligned(size, align);
}

__attribute__((visibility("default"))) void *__rust_alloc(size_t size, size_t align)
{
    return tally_allocate_aligned(size, align);
}

__attribute__((visibility("default"))) void __rust_dealloc(
    void *ptr,
    size_t _size,
    size_t _align)
{
    tally_deallocate(ptr);
}

__attribute__((visibility("default"))) void *__rust_realloc(
    void *ptr,
    size_t _old_size,
    size_t align,
    size_t new_size)
{
    return tally_reallocate(ptr, new_size, align);
}

__attribute__((visibility("default"))) void *__rust_alloc_zeroed(
    size_t size,
    size_t align)
{
    void *ptr = tally_allocate_aligned(size, align);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}
