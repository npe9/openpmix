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

PMIX_EXPORT void pmix_lithe_register_fork_join_sched(lithe_fork_join_sched_t *sched)
{
    pmix_lithe_registered_fork_join_sched = sched;
}

PMIX_EXPORT lithe_fork_join_sched_t *pmix_lithe_get_fork_join_sched(void)
{
    return pmix_lithe_registered_fork_join_sched;
}

#else

static void __attribute__((unused)) pmix_lithe_fork_join_api_unused(void) {}

#endif /* HAVE_LITHE */
