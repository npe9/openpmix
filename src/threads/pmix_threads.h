/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_THREAD_H
#define PMIX_THREAD_H 1

#include "src/include/pmix_config.h"

#include <pthread.h>
#include <signal.h>

#include "src/class/pmix_object.h"
#include "src/include/pmix_atomic.h"
#if PMIX_ENABLE_DEBUG
#    include "src/util/pmix_output.h"
#endif

#ifdef HAVE_LITHE
#include <lithe/condvar.h>
#include <lithe/fork_join_sched.h>
#include <lithe/lithe.h>
#endif

#include "pmix_mutex.h"

BEGIN_C_DECLS

typedef void *(*pmix_thread_fn_t)(pmix_object_t *);

#define PMIX_THREAD_CANCELLED ((void *) 1);

struct pmix_thread_t {
    pmix_object_t super;
    pmix_thread_fn_t t_run;
    void *t_arg;
    pthread_t t_handle;
#ifdef HAVE_LITHE
    volatile int t_lithe_done;
    void *t_lithe_ret;
#endif
};

typedef struct pmix_thread_t pmix_thread_t;

#if PMIX_ENABLE_DEBUG
PMIX_EXPORT extern bool pmix_debug_threads;
#endif

PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_thread_t);

#ifdef HAVE_LITHE

/* Host (OPAL, MPICH MPL, …) must call this after entering its fork-join sched and before PMIx starts Lithe threads. */
PMIX_EXPORT void pmix_lithe_register_fork_join_sched(lithe_fork_join_sched_t *sched);
PMIX_EXPORT lithe_fork_join_sched_t *pmix_lithe_get_fork_join_sched(void);

typedef lithe_condvar_t pmix_condition_t;
#define pmix_condition_wait(a, b)    lithe_condvar_wait(a, &(b)->m_lock_lithe)
#define pmix_condition_broadcast(a)  lithe_condvar_broadcast(a)
#define pmix_condition_signal(a)     lithe_condvar_signal(a)
#define PMIX_CONDITION_STATIC_INIT   {{0}}

typedef struct {
    pmix_status_t status;
    pmix_mutex_t mutex;
    pmix_condition_t cond;
    volatile bool active;
    volatile int cond_initialized;
} pmix_lock_t;

#define PMIX_LOCK_STATIC_INIT               \
    {                                       \
        .status = PMIX_SUCCESS,             \
        .mutex = PMIX_MUTEX_STATIC_INIT,    \
        .cond = PMIX_CONDITION_STATIC_INIT, \
        .active = false,                    \
        .cond_initialized = 0               \
    }

#define PMIX_CONSTRUCT_LOCK(l)                     \
    do {                                           \
        PMIX_CONSTRUCT(&(l)->mutex, pmix_mutex_t); \
        lithe_condvar_init(&(l)->cond);            \
        (l)->cond_initialized = 1;                 \
        /* coverity[missing_lock : FALSE] */       \
        (l)->active = true;                        \
    } while (0)

#define PMIX_DESTRUCT_LOCK(l)             \
    do {                                  \
        PMIX_DESTRUCT(&(l)->mutex);       \
    } while (0)

#else /* !HAVE_LITHE */

typedef pthread_cond_t pmix_condition_t;
#define pmix_condition_wait(a, b) pthread_cond_wait(a, &(b)->m_lock_pthread)
#define pmix_condition_broadcast(a) pthread_cond_broadcast(a)
#define pmix_condition_signal(a)    pthread_cond_signal(a)
#define PMIX_CONDITION_STATIC_INIT  PTHREAD_COND_INITIALIZER

typedef struct {
    pmix_status_t status;
    pmix_mutex_t mutex;
    pmix_condition_t cond;
    volatile bool active;
} pmix_lock_t;

#define PMIX_LOCK_STATIC_INIT               \
    {                                       \
        .status = PMIX_SUCCESS,             \
        .mutex = PMIX_MUTEX_STATIC_INIT,    \
        .cond = PMIX_CONDITION_STATIC_INIT, \
        .active = false                     \
    }

#define PMIX_CONSTRUCT_LOCK(l)                     \
    do {                                           \
        PMIX_CONSTRUCT(&(l)->mutex, pmix_mutex_t); \
        pthread_cond_init(&(l)->cond, NULL);       \
        /* coverity[missing_lock : FALSE] */       \
        (l)->active = true;                        \
    } while (0)

#define PMIX_DESTRUCT_LOCK(l)             \
    do {                                  \
        PMIX_DESTRUCT(&(l)->mutex);       \
        pthread_cond_destroy(&(l)->cond); \
    } while (0)

#endif /* HAVE_LITHE */

#ifdef HAVE_LITHE
#    define PMIX_ENSURE_LOCK_COND_INIT(lck)                            \
        do {                                                           \
            if (__builtin_expect(!(lck)->cond_initialized, 0)) {       \
                lithe_condvar_init(&(lck)->cond);                      \
                (lck)->cond_initialized = 1;                           \
            }                                                          \
        } while (0)
#else
#    define PMIX_ENSURE_LOCK_COND_INIT(lck) do { } while(0)
#endif

#ifdef HAVE_LITHE
#    define PMIX_THREAD_COND_WAIT(lck)        \
        do {                                  \
            pmix_mutex_unlock(&(lck)->mutex); \
            lithe_context_yield();            \
            pmix_mutex_lock(&(lck)->mutex);   \
        } while (0)
#else
#    define PMIX_THREAD_COND_WAIT(lck) pmix_condition_wait(&(lck)->cond, &(lck)->mutex)
#endif

#if PMIX_ENABLE_DEBUG
#    define PMIX_ACQUIRE_THREAD(lck)                                            \
        do {                                                                    \
            PMIX_ENSURE_LOCK_COND_INIT(lck);                                    \
            pmix_mutex_lock(&(lck)->mutex);                                     \
            if (pmix_debug_threads) {                                           \
                pmix_output(0, "Waiting for thread %s:%d", __FILE__, __LINE__); \
            }                                                                   \
            while ((lck)->active) {                                             \
                PMIX_THREAD_COND_WAIT(lck);               \
            }                                                                   \
            if (pmix_debug_threads) {                                           \
                pmix_output(0, "Thread obtained %s:%d", __FILE__, __LINE__);    \
            }                                                                   \
            PMIX_ACQUIRE_OBJECT(lck);                                           \
            (lck)->active = true;                                               \
        } while (0)
#else
#    define PMIX_ACQUIRE_THREAD(lck)                              \
        do {                                                      \
            PMIX_ENSURE_LOCK_COND_INIT(lck);                      \
            pmix_mutex_lock(&(lck)->mutex);                       \
            while ((lck)->active) {                               \
                PMIX_THREAD_COND_WAIT(lck); \
            }                                                     \
            PMIX_ACQUIRE_OBJECT(lck);                             \
            (lck)->active = true;                                 \
        } while (0)
#endif

#if PMIX_ENABLE_DEBUG
#    define PMIX_WAIT_THREAD(lck)                                               \
        do {                                                                    \
            PMIX_ENSURE_LOCK_COND_INIT(lck);                                    \
            pmix_mutex_lock(&(lck)->mutex);                                     \
            if (pmix_debug_threads) {                                           \
                pmix_output(0, "Waiting for thread %s:%d", __FILE__, __LINE__); \
            }                                                                   \
            while ((lck)->active) {                                             \
                PMIX_THREAD_COND_WAIT(lck);               \
            }                                                                   \
            if (pmix_debug_threads) {                                           \
                pmix_output(0, "Thread obtained %s:%d", __FILE__, __LINE__);    \
            }                                                                   \
            PMIX_ACQUIRE_OBJECT(lck);                                           \
            pmix_mutex_unlock(&(lck)->mutex);                                   \
        } while (0)
#else
#    define PMIX_WAIT_THREAD(lck)                                 \
        do {                                                      \
            PMIX_ENSURE_LOCK_COND_INIT(lck);                      \
            pmix_mutex_lock(&(lck)->mutex);                       \
            while ((lck)->active) {                               \
                PMIX_THREAD_COND_WAIT(lck); \
            }                                                     \
            PMIX_ACQUIRE_OBJECT(lck);                             \
            pmix_mutex_unlock(&(lck)->mutex);                     \
        } while (0)
#endif

#if PMIX_ENABLE_DEBUG
#    define PMIX_RELEASE_THREAD(lck)                                          \
        do {                                                                  \
            PMIX_ENSURE_LOCK_COND_INIT(lck);                                  \
            if (pmix_debug_threads) {                                         \
                pmix_output(0, "Releasing thread %s:%d", __FILE__, __LINE__); \
            }                                                                 \
            (lck)->active = false;                                            \
            PMIX_POST_OBJECT(lck);                                            \
            pmix_condition_broadcast(&(lck)->cond);                           \
            pmix_mutex_unlock(&(lck)->mutex);                                 \
        } while (0)
#else
#    define PMIX_RELEASE_THREAD(lck)                \
        do {                                        \
            PMIX_ENSURE_LOCK_COND_INIT(lck);        \
            (lck)->active = false;                  \
            PMIX_POST_OBJECT(lck);                  \
            pmix_condition_broadcast(&(lck)->cond); \
            pmix_mutex_unlock(&(lck)->mutex);       \
        } while (0)
#endif

#define PMIX_WAKEUP_THREAD(lck)                 \
    do {                                        \
        PMIX_ENSURE_LOCK_COND_INIT(lck);        \
        pmix_mutex_lock(&(lck)->mutex);         \
        (lck)->active = false;                  \
        PMIX_POST_OBJECT(lck);                  \
        pmix_condition_broadcast(&(lck)->cond); \
        pmix_mutex_unlock(&(lck)->mutex);       \
    } while (0)

/* provide a macro for forward-proofing the shifting
 * of objects between threads - at some point, we
 * may revamp our threading model */

/* post an object to another thread - for now, we
 * only have a memory barrier */
#define PMIX_POST_OBJECT(o) pmix_atomic_wmb()

/* acquire an object from another thread - for now,
 * we only have a memory barrier */
#define PMIX_ACQUIRE_OBJECT(o) pmix_atomic_rmb()

PMIX_EXPORT int pmix_thread_start(pmix_thread_t *);
PMIX_EXPORT int pmix_thread_join(pmix_thread_t *, void **thread_return);
PMIX_EXPORT bool pmix_thread_self_compare(pmix_thread_t *);
PMIX_EXPORT pmix_thread_t *pmix_thread_get_self(void);
PMIX_EXPORT void pmix_thread_kill(pmix_thread_t *, int sig);
PMIX_EXPORT void pmix_thread_set_main(void);

END_C_DECLS

#endif /* PMIX_THREAD_H */
