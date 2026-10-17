/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the GPU engines' files share */
#ifndef SNX_ENGINES_INT_H
#define SNX_ENGINES_INT_H
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/renderer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include "nx/gpu/gpu.h"
#include "nx/runtime/clock.h"

/* engines.c (gpu_ticks is in gpu.h) */
void query_get(GpuChannel* ch, u32* r, int base, u32 v);

/* dma.c: a block linear surface's bytes up to slice z as one host pointer, with their count in span, when they are
   contiguous in guest memory and 256 MiB at most; NULL otherwise */
u8* bl_span_ptr(u64 base, u32 wbytes, u32 h, u32 bh, u32 z, u32 bd, u64* span);

/* mme.c */
/* runs macro idx over the np parameters in ch->mme_params (it may stop at a wait, gpu.h's mme_resume) */
void mme_run(GpuChannel* ch, u32 idx, u32 np);

#endif
