/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_GC_H
#define HUSK_TL_GC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void tl_gc_init(void);
bool tl_gc_active(void);
void *tl_gc_alloc(size_t size);
void *tl_gc_alloc_atomic(size_t size);
void *tl_gc_alloc_uncollectable(size_t size);
void *tl_gc_realloc(void *ptr, size_t size);
void *tl_gc_realloc_atomic(void *ptr, size_t size);
void tl_gc_free(void *ptr);
char *tl_gc_strdup(const char *s);
void tl_gc_collect(void);
int64_t tl_gc_free_bytes(void);
int64_t tl_gc_total_bytes(void);
int64_t tl_gc_process_limit(void);
void tl_gc_set_pending(void *object);
void tl_gc_set_current_thread(void *object);
void tl_gc_add_roots(void *start, void *end);
void tl_gc_global_ref_add(void *object);
void tl_gc_global_ref_remove(void *object);
bool tl_gc_weak_ref_register(void **slot, void *object);
void tl_gc_weak_ref_remove(void **slot);

#endif
