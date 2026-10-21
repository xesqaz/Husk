/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-gc.h"

#include <libproc.h>
#include <os/proc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define GC_NO_THREAD_REDIRECTS
#define GC_THREADS
#include <gc.h>

static _Atomic bool g_active;
static pthread_key_t g_roots_key;
static pthread_once_t g_roots_once = PTHREAD_ONCE_INIT;

typedef struct tl_gc_thread_roots {
    void *pending;
    void *current_thread;
} tl_gc_thread_roots;

typedef struct tl_gc_global_ref {
    void *object;
    struct tl_gc_global_ref *next;
} tl_gc_global_ref;

static pthread_mutex_t g_global_refs_lock = PTHREAD_MUTEX_INITIALIZER;
static tl_gc_global_ref *g_global_refs;

static void roots_destroy(void *value)
{
    tl_gc_thread_roots *roots = value;
    if (!roots) return;
    roots->pending = NULL;
    roots->current_thread = NULL;
    GC_free(roots);
}

static void roots_key_init(void)
{
    (void)pthread_key_create(&g_roots_key, roots_destroy);
}

static tl_gc_thread_roots *thread_roots(void)
{
    if (!tl_gc_active()) return NULL;
    pthread_once(&g_roots_once, roots_key_init);
    tl_gc_thread_roots *roots = pthread_getspecific(g_roots_key);
    if (!roots) {
        roots = GC_malloc_uncollectable(sizeof(*roots));
        if (roots && pthread_setspecific(g_roots_key, roots) != 0) {
            roots->pending = NULL;
            roots->current_thread = NULL;
            GC_free(roots);
            roots = NULL;
        }
    }
    return roots;
}

void tl_gc_init(void)
{
    if (tl_gc_active()) return;
    GC_INIT();
    atomic_store_explicit(&g_active, true, memory_order_release);
    (void)thread_roots();
}

bool tl_gc_active(void)
{
    return atomic_load_explicit(&g_active, memory_order_acquire);
}

void *tl_gc_alloc(size_t size)
{
    return tl_gc_active() ? GC_malloc(size ? size : 1) : calloc(1, size ? size : 1);
}

void *tl_gc_alloc_atomic(size_t size)
{
    size_t alloc_size = size ? size : 1;
    void *ptr = tl_gc_active() ? GC_malloc_atomic(alloc_size) : calloc(1, alloc_size);
    /* Java primitive arrays are zero-initialized even though the collector's
     * atomic allocator deliberately leaves pointer-free memory uncleared. */
    if (ptr && tl_gc_active()) memset(ptr, 0, alloc_size);
    return ptr;
}

void *tl_gc_alloc_uncollectable(size_t size)
{
    return tl_gc_active() ? GC_malloc_uncollectable(size ? size : 1) : calloc(1, size ? size : 1);
}

void *tl_gc_realloc(void *ptr, size_t size)
{
    if (!size) {
        tl_gc_free(ptr);
        return NULL;
    }
    if (tl_gc_active() && ptr && GC_base(ptr)) return GC_realloc(ptr, size);
    if (tl_gc_active() && !ptr) return GC_malloc(size);
    return realloc(ptr, size);
}

void *tl_gc_realloc_atomic(void *ptr, size_t size)
{
    if (!size) {
        tl_gc_free(ptr);
        return NULL;
    }
    if (tl_gc_active() && ptr && GC_base(ptr)) return GC_realloc(ptr, size);
    if (tl_gc_active() && !ptr) return tl_gc_alloc_atomic(size);
    return realloc(ptr, size);
}

void tl_gc_free(void *ptr)
{
    if (!ptr) return;
    if (tl_gc_active() && GC_base(ptr)) GC_free(ptr);
    else free(ptr);
}

char *tl_gc_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *copy = tl_gc_active() ? GC_malloc_atomic(n) : malloc(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

void tl_gc_collect(void)
{
    if (tl_gc_active()) GC_gcollect_and_unmap();
}

int64_t tl_gc_free_bytes(void)
{
    return tl_gc_active() ? (int64_t)GC_get_free_bytes() : 0;
}

int64_t tl_gc_total_bytes(void)
{
    return tl_gc_active() ? (int64_t)GC_get_heap_size() : 0;
}

int64_t tl_gc_process_limit(void)
{
    rusage_info_current usage;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_CURRENT, &usage) != 0) return 0;
    size_t available = os_proc_available_memory();
    uint64_t footprint = usage.ri_phys_footprint;
    if (available > UINT64_MAX - footprint) return INT64_MAX;
    uint64_t limit = footprint + available;
    return limit > INT64_MAX ? INT64_MAX : (int64_t)limit;
}

void tl_gc_set_pending(void *object)
{
    tl_gc_thread_roots *roots = thread_roots();
    if (roots) roots->pending = object;
}

void tl_gc_set_current_thread(void *object)
{
    tl_gc_thread_roots *roots = thread_roots();
    if (roots) roots->current_thread = object;
}

void tl_gc_add_roots(void *start, void *end)
{
    if (tl_gc_active() && start && end && (uintptr_t)start < (uintptr_t)end) GC_add_roots(start, end);
}

void tl_gc_global_ref_add(void *object)
{
    if (!object || !tl_gc_active()) return;
    tl_gc_global_ref *ref = GC_malloc_uncollectable(sizeof(*ref));
    if (!ref) return;
    ref->object = object;
    pthread_mutex_lock(&g_global_refs_lock);
    ref->next = g_global_refs;
    g_global_refs = ref;
    pthread_mutex_unlock(&g_global_refs_lock);
}

void tl_gc_global_ref_remove(void *object)
{
    if (!object || !tl_gc_active()) return;
    pthread_mutex_lock(&g_global_refs_lock);
    tl_gc_global_ref **link = &g_global_refs;
    while (*link && (*link)->object != object) link = &(*link)->next;
    tl_gc_global_ref *ref = *link;
    if (ref) {
        *link = ref->next;
        ref->object = NULL;
        ref->next = NULL;
    }
    pthread_mutex_unlock(&g_global_refs_lock);
    if (ref) GC_free(ref);
}

bool tl_gc_weak_ref_register(void **slot, void *object)
{
    return slot && object && tl_gc_active()
        && GC_general_register_disappearing_link(slot, object) == GC_SUCCESS;
}

void tl_gc_weak_ref_remove(void **slot)
{
    if (slot && tl_gc_active()) (void)GC_unregister_disappearing_link(slot);
}
