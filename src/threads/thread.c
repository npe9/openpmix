/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/threads/pmix_threads.h"
#include "src/threads/pmix_tsd.h"

#include <stdlib.h>

#ifdef HAVE_LITHE
#include <lithe/lithe.h>
#include <lithe/fork_join_sched.h>
#include <stdio.h>

/* pmix_lithe_register_fork_join_sched / pmix_lithe_get_fork_join_sched live in
 * pmix_lithe_fork_join_api.c so they are never dropped by per-TU dead-code
 * elimination (thread.c references get from pmix_thread_start; without a strong
 * definition in the archive, the link leaves U and libomp's weak ref fails). */

static void pmix_lithe_thread_fn(void *arg)
{
    pmix_thread_t *t = (pmix_thread_t *)arg;
    t->t_lithe_ret = t->t_run((pmix_object_t *)t);
    __sync_synchronize();
    t->t_lithe_done = 1;
}
#endif

bool pmix_debug_threads = false;

static void pmix_thread_construct(pmix_thread_t *t);

#ifdef HAVE_LITHE
static uintptr_t pmix_main_ctx_id;
#else
static pthread_t pmix_main_thread;
#endif

struct pmix_tsd_key_value {
    pmix_tsd_key_t key;
    pmix_tsd_destructor_t destructor;
};

static struct pmix_tsd_key_value *pmix_tsd_key_values = NULL;
static int pmix_tsd_key_values_count = 0;

PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_thread_t, pmix_object_t, pmix_thread_construct, NULL);

/*
 * Constructor
 */
static void pmix_thread_construct(pmix_thread_t *t)
{
    t->t_run = 0;
    t->t_handle = (pthread_t) -1;
#ifdef HAVE_LITHE
    t->t_lithe_done = 0;
    t->t_lithe_ret = NULL;
#endif
}

int pmix_thread_start(pmix_thread_t *t)
{
    if (PMIX_ENABLE_DEBUG) {
        if (NULL == t->t_run || t->t_handle != (pthread_t) -1) {
            return PMIX_ERR_BAD_PARAM;
        }
    }

#ifdef HAVE_LITHE
    {
        lithe_fork_join_sched_t *sched = pmix_lithe_get_fork_join_sched();
        if (!sched) {
            fprintf(stderr,
                    "[PMIx-Lithe] FATAL: pmix_thread_start with no scheduler "
                    "(host must call pmix_lithe_register_fork_join_sched before PMIx progress)\n");
            abort();
        }
        if (getenv("LITHE_DEBUG"))
            fprintf(stderr,
                    "[PMIx-Lithe] thread_start: sched=%p t_run=%p\n",
                    (void *)sched, (void *)(uintptr_t)t->t_run);
        t->t_lithe_done = 0;
        t->t_lithe_ret = NULL;
        lithe_fork_join_context_t *ctx =
            lithe_fork_join_context_create(sched, 262144,
                                           pmix_lithe_thread_fn, t);
        if (!ctx)
            return PMIX_ERROR;
        t->t_handle = (pthread_t)(uintptr_t)ctx;
        lithe_context_yield();
        return PMIX_SUCCESS;
    }
#else
    {
        int rc = pthread_create(&t->t_handle, NULL,
                                (void *(*)(void *))t->t_run, t);
        return (rc == 0) ? PMIX_SUCCESS : PMIX_ERROR;
    }
#endif
}

int pmix_thread_join(pmix_thread_t *t, void **thr_return)
{
#ifdef HAVE_LITHE
    {
        lithe_fork_join_sched_t *sched = pmix_lithe_get_fork_join_sched();
        if (!sched) {
            fprintf(stderr,
                    "[PMIx-Lithe] FATAL: pmix_thread_join with no scheduler "
                    "(host must call pmix_lithe_register_fork_join_sched)\n");
            abort();
        }
        while (!t->t_lithe_done) {
            lithe_context_yield();
        }
        t->t_handle = (pthread_t)-1;
        if (thr_return)
            *thr_return = t->t_lithe_ret;
        return PMIX_SUCCESS;
    }
#else
    {
        int rc = pthread_join(t->t_handle, thr_return);
        t->t_handle = (pthread_t)-1;
        return (rc == 0) ? PMIX_SUCCESS : PMIX_ERROR;
    }
#endif
}

bool pmix_thread_self_compare(pmix_thread_t *t)
{
#ifdef HAVE_LITHE
    lithe_context_t *self = lithe_context_self();
    return t->t_handle == (pthread_t)(uintptr_t)self;
#else
    return t->t_handle == pthread_self();
#endif
}

pmix_thread_t *pmix_thread_get_self(void)
{
    pmix_thread_t *t = PMIX_NEW(pmix_thread_t);
#ifdef HAVE_LITHE
    t->t_handle = (pthread_t)(uintptr_t)lithe_context_self();
#else
    t->t_handle = pthread_self();
#endif
    return t;
}

void pmix_thread_kill(pmix_thread_t *t, int sig)
{
#ifdef HAVE_LITHE
    (void)t; (void)sig;
#else
    pthread_kill(t->t_handle, sig);
#endif
}

#ifdef HAVE_LITHE

/* ================================================================
 * PMIx Lithe-native TSD (mirrors OPAL's approach: no pthread keys).
 * Keys are integers; values stored per-Lithe-context in a hash table.
 * ================================================================ */

static volatile int pmix_tsd_spinlock = 0;

static inline void pmix_tsd_spin_lock(void)
{
    while (__sync_lock_test_and_set(&pmix_tsd_spinlock, 1))
        while (pmix_tsd_spinlock)
            __asm__ volatile("pause" ::: "memory");
}

static inline void pmix_tsd_spin_unlock(void)
{
    __sync_lock_release(&pmix_tsd_spinlock);
}

static pmix_tsd_key_t pmix_tsd_next_key = 1;

struct pmix_tsd_entry {
    pmix_tsd_key_t        key;
    void                 *value;
    struct pmix_tsd_entry *next;
};

struct pmix_tsd_ctx_bucket {
    uintptr_t                   ctx_id;
    struct pmix_tsd_entry      *entries;
    struct pmix_tsd_ctx_bucket *next;
};

#define PMIX_TSD_HASH_SIZE 64
static struct pmix_tsd_ctx_bucket *pmix_tsd_hash[PMIX_TSD_HASH_SIZE];

static inline size_t pmix_tsd_hash_fn(uintptr_t cid)
{
    return (cid >> 4) % PMIX_TSD_HASH_SIZE;
}

static struct pmix_tsd_entry **pmix_tsd_get_list(uintptr_t cid)
{
    size_t h = pmix_tsd_hash_fn(cid);
    struct pmix_tsd_ctx_bucket *b = pmix_tsd_hash[h];
    while (b) {
        if (b->ctx_id == cid) return &b->entries;
        b = b->next;
    }
    b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->ctx_id = cid;
    b->next = pmix_tsd_hash[h];
    pmix_tsd_hash[h] = b;
    return &b->entries;
}

static inline uintptr_t pmix_tsd_current_ctx_id(void)
{
    lithe_context_t *ctx = lithe_context_self();
    return ctx ? (uintptr_t)ctx : 0;
}

int pmix_tsd_key_create(pmix_tsd_key_t *key, pmix_tsd_destructor_t destructor)
{
    if (!key) return PMIX_ERR_BAD_PARAM;

    pmix_tsd_spin_lock();
    pmix_tsd_key_t k = pmix_tsd_next_key++;

    pmix_tsd_key_values = realloc(pmix_tsd_key_values,
        (pmix_tsd_key_values_count + 1) * sizeof(struct pmix_tsd_key_value));
    pmix_tsd_key_values[pmix_tsd_key_values_count].key = k;
    pmix_tsd_key_values[pmix_tsd_key_values_count].destructor = destructor;
    pmix_tsd_key_values_count++;

    pmix_tsd_spin_unlock();

    *key = k;
    return PMIX_SUCCESS;
}

int pmix_lithe_tsd_key_delete(pmix_tsd_key_t key)
{
    if (key == 0) return PMIX_ERR_BAD_PARAM;
    pmix_tsd_spin_lock();
    for (int i = 0; i < pmix_tsd_key_values_count; i++) {
        if (pmix_tsd_key_values[i].key == key) {
            pmix_tsd_key_values[i] = pmix_tsd_key_values[--pmix_tsd_key_values_count];
            break;
        }
    }
    pmix_tsd_spin_unlock();
    return PMIX_SUCCESS;
}

int pmix_lithe_tsd_setspecific(pmix_tsd_key_t key, void *value)
{
    if (key == 0) return PMIX_ERR_BAD_PARAM;
    uintptr_t cid = pmix_tsd_current_ctx_id();

    pmix_tsd_spin_lock();
    struct pmix_tsd_entry **list = pmix_tsd_get_list(cid);
    if (!list) { pmix_tsd_spin_unlock(); return PMIX_ERROR; }

    struct pmix_tsd_entry *e = *list;
    while (e) {
        if (e->key == key) { e->value = value; pmix_tsd_spin_unlock(); return PMIX_SUCCESS; }
        e = e->next;
    }
    e = malloc(sizeof(*e));
    if (!e) { pmix_tsd_spin_unlock(); return PMIX_ERROR; }
    e->key = key;
    e->value = value;
    e->next = *list;
    *list = e;

    pmix_tsd_spin_unlock();
    return PMIX_SUCCESS;
}

int pmix_lithe_tsd_getspecific(pmix_tsd_key_t key, void **valuep)
{
    if (key == 0 || !valuep) return PMIX_ERR_BAD_PARAM;
    uintptr_t cid = pmix_tsd_current_ctx_id();

    pmix_tsd_spin_lock();
    struct pmix_tsd_entry **list = pmix_tsd_get_list(cid);
    if (!list) { *valuep = NULL; pmix_tsd_spin_unlock(); return PMIX_SUCCESS; }

    struct pmix_tsd_entry *e = *list;
    while (e) {
        if (e->key == key) { *valuep = e->value; pmix_tsd_spin_unlock(); return PMIX_SUCCESS; }
        e = e->next;
    }
    *valuep = NULL;
    pmix_tsd_spin_unlock();
    return PMIX_SUCCESS;
}

int pmix_tsd_keys_destruct(void)
{
    pmix_tsd_spin_lock();
    uintptr_t cid = pmix_tsd_current_ctx_id();
    struct pmix_tsd_entry **list = pmix_tsd_get_list(cid);
    if (list) {
        for (int i = 0; i < pmix_tsd_key_values_count; i++) {
            struct pmix_tsd_entry *e = *list;
            while (e) {
                if (e->key == pmix_tsd_key_values[i].key && e->value &&
                    pmix_tsd_key_values[i].destructor) {
                    pmix_tsd_key_values[i].destructor(e->value);
                    e->value = NULL;
                }
                e = e->next;
            }
        }
    }
    if (pmix_tsd_key_values_count > 0) {
        free(pmix_tsd_key_values);
        pmix_tsd_key_values = NULL;
        pmix_tsd_key_values_count = 0;
    }
    pmix_tsd_spin_unlock();
    return PMIX_SUCCESS;
}

void pmix_thread_set_main(void)
{
    pmix_main_ctx_id = pmix_tsd_current_ctx_id();
}

#else /* !HAVE_LITHE */

int pmix_tsd_key_create(pmix_tsd_key_t *key, pmix_tsd_destructor_t destructor)
{
    int rc;
    rc = pthread_key_create(key, destructor);
    if ((0 == rc) && (pthread_self() == pmix_main_thread)) {
        pmix_tsd_key_values = (struct pmix_tsd_key_value *)
            realloc(pmix_tsd_key_values,
                    (pmix_tsd_key_values_count + 1) * sizeof(struct pmix_tsd_key_value));
        pmix_tsd_key_values[pmix_tsd_key_values_count].key = *key;
        pmix_tsd_key_values[pmix_tsd_key_values_count].destructor = destructor;
        pmix_tsd_key_values_count++;
    }
    return rc;
}

int pmix_tsd_keys_destruct(void)
{
    int i;
    void *ptr;
    for (i = 0; i < pmix_tsd_key_values_count; i++) {
        if (PMIX_SUCCESS == pmix_tsd_getspecific(pmix_tsd_key_values[i].key, &ptr)) {
            if (NULL != pmix_tsd_key_values[i].destructor) {
                pmix_tsd_key_values[i].destructor(ptr);
                pmix_tsd_setspecific(pmix_tsd_key_values[i].key, NULL);
            }
        }
    }
    if (0 < pmix_tsd_key_values_count) {
        free(pmix_tsd_key_values);
        pmix_tsd_key_values_count = 0;
    }
    return PMIX_SUCCESS;
}

void pmix_thread_set_main(void)
{
    pmix_main_thread = pthread_self();
}

#endif /* HAVE_LITHE */
