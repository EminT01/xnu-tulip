/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>

extern "C" {
#include <machine/machine_routines.h>
#include <kern/clock.h>
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
	registerService();
	return true;
}
