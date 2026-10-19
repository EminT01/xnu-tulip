/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>

extern "C" {
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
}

class TulipPlatformExpert : public IODTPlatformExpert
{
	OSDeclareDefaultStructors(TulipPlatformExpert);

public:
	virtual const char * deleteList(void) APPLE_KEXT_OVERRIDE;
	virtual const char * excludeList(void) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;

private:
	uint64_t bootUTC;
};

OSDefineMetaClassAndStructors(TulipPlatformExpert, IODTPlatformExpert);

extern void tulip_cpu_init(void);

static void
tulip_tick_thread(void *arg __unused, wait_result_t wr __unused)
{
	for (unsigned int n = 0;; n++) {
		IODelay(1000000);
		kprintf("tick %u\n", n);
		tulip_gic_report();
	}
}

long
TulipPlatformExpert::getGMTTimeOfDay(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (long)(bootUTC + ns / NSEC_PER_SEC);
}

const char *
TulipPlatformExpert::deleteList(void)
{
	return "('tulip-delete-nothing')";
}

const char *
TulipPlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'memory-map', 'options')";
}

bool
TulipPlatformExpert::start(IOService * provider)
{
	tulip_cpu_init();
	if (!IODTPlatformExpert::start(provider)) {
		return false;
	}
	ml_set_max_cpus(ml_get_max_cpu_number() + 1);

	IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	if (chosen) {
		OSData *utc = OSDynamicCast(OSData, chosen->getProperty("tulip-utc"));
		if (utc && utc->getLength() == sizeof(bootUTC)) {
			bootUTC = *(const uint64_t *)utc->getBytesNoCopy();
		}
		chosen->release();
	}
	publishResource("IORTC", this);
	tulip_gic_enable_vtimer();

	uint32_t timertest = 0;
	if (PE_parse_boot_argn("tulip_timertest", &timertest, sizeof(timertest)) && timertest) {
		tulip_timer_selftest();
	}

	uint32_t tick = 0;
	if (PE_parse_boot_argn("tulip_tick", &tick, sizeof(tick)) && tick) {
		thread_t thread;
		if (kernel_thread_start(tulip_tick_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	registerService();
	return true;
}
