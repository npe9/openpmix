/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*-
 * Standalone PMIx / prte daemons wire libevent to lithe mutex/condvar when OPAL is absent.
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#if defined(HAVE_LITHE) && !PMIX_HAVE_LIBEV

#    define LITHE_EVTHREAD_WIRE_FN pmix_lithe_evthread_wire_libevent
#    define LITHE_EVTHREAD_EXPORT  PMIX_EXPORT
#    include "../../../../opal/util/lithe_evthread_libevent_impl.inc.c"

#endif
