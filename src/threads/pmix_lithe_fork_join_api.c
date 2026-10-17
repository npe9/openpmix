/*
 * Copyright (c) 2024      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Fork-join scheduler pointer for PMIx Lithe threads: kept in this TU so the
 * symbols are always archived in libpmix (see thread.c comment).
 */

#include "src/include/pmix_config.h"

#ifdef HAVE_LITHE

#    include "pmix_common.h"
#    include "src/threads/pmix_threads.h"
#    include <lithe/fork_join_sched.h>
#    include <stdio.h>
#    include <stdlib.h>

static lithe_fork_join_sched_t *pmix_lithe_registered_fork_join_sched;

/* Weak definition: OPAL provides the strong symbol when MPI is linked; PMIx
 * tools (e.g. pevent) link only libpmix and get a null default. */
lithe_fork_join_sched_t *opal_lithe_pmix_export_sched __attribute__((weak)) = NULL;

PMIX_EXPORT void pmix_lithe_register_fork_join_sched(lithe_fork_join_sched_t *sched)
{
    pmix_lithe_registered_fork_join_sched = sched;
    __sync_synchronize();
}

PMIX_EXPORT lithe_fork_join_sched_t *pmix_lithe_get_fork_join_sched(void)
{
    return pmix_lithe_registered_fork_join_sched;
}

/* Weak: overridden by OPAL's strong symbol when Open MPI is linked (OPAL owns
 * the root sched and registers opal_sched). When libpmix is built STANDALONE and
 * consumed by a non-OMPI runtime (e.g. lithified MPICH), no OPAL export exists,
 * so bootstrap the process-wide root fork-join scheduler here and register it.
 * lithe_ensure_root_fork_join_sched() is idempotent and returns the SAME root
 * that MPICH's MPL-Lithe bootstrap uses, per
 * lithe-bootstrap-any-runtime-combination — this is composition, not a second
 * conflicting bootstrap. */
void __attribute__((weak)) opal_pmix_lithe_host_ensure_sched(void)
{
    if (NULL == pmix_lithe_get_fork_join_sched()) {
        lithe_fork_join_sched_t *root = lithe_ensure_root_fork_join_sched();
        if (NULL != root) {
            pmix_lithe_register_fork_join_sched(root);
        }
    }
}

#else

static void __attribute__((unused)) pmix_lithe_fork_join_api_unused(void) {}

#endif /* HAVE_LITHE */
