/* SPDX-License-Identifier: MIT */
#include "kosmickrisp-rt-dispatch.h"
#include "kosmickrisp/libkk/kk_argument_table.h"
#include "util/ralloc.h"
#include "kk_rt_dispatch_source.h"

char *
kk_rt_dispatcher_source(void *mem_ctx)
{
   return ralloc_asprintf(mem_ctx, "#define KK_ARGUMENT_ROOT %u\n%s",
                          KK_ARGUMENT_ROOT, kk_rt_dispatch_msl);
}
