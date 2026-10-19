/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

/* Open-source stand-ins for routines that ship only in Apple's per-SoC objects. */

#include <arm/caches_internal.h>
#include <arm/machine_routines.h>
#include <arm/pmap.h>

void
ml_enable_monitor(void)
{
}

#if SCHED_HYGIENE_DEBUG && (DEBUG || DEVELOPMENT)
bool
pmap_pending_preemption(void)
{
	return _pmap_pending_preemption_real();
}
#endif

void
CleanPoC_DcacheRegion_Force_nopreempt_nohid(vm_offset_t va, size_t length)
{
	CleanPoC_DcacheRegion_Force(va, length);
}
