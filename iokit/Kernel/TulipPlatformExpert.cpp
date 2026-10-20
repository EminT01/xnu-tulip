/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLocks.h>

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
	virtual void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	virtual int haltRestart(unsigned int type) APPLE_KEXT_OVERRIDE;

private:
	uint64_t bootUTC;
};

OSDefineMetaClassAndStructors(TulipPlatformExpert, IODTPlatformExpert);

IOLock *gTulipSPMILock;

extern void tulip_cpu_init(void);
extern int64_t tulip_psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);

#define PSCI_SYSTEM_OFF   0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u
extern "C" uint64_t tulip_emmc_stats[4];

/* CPU clock in MHz: PMU cycles over 10 ms of timer time. */
uint64_t tulip_cpu_mhz(void);

uint64_t
tulip_cpu_mhz(void)
{
	uint64_t ten_ms, start, cycles;
	boolean_t istate = ml_set_interrupts_enabled(FALSE);

	__builtin_arm_wsr64("PMCR_EL0", __builtin_arm_rsr64("PMCR_EL0") | 1);
	__builtin_arm_wsr64("PMCNTENSET_EL0", 1ULL << 31);
	__builtin_arm_isb(15);
	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &ten_ms);
	start = mach_absolute_time();
	cycles = __builtin_arm_rsr64("PMCCNTR_EL0");
	while (mach_absolute_time() - start < ten_ms) {
	}
	cycles = __builtin_arm_rsr64("PMCCNTR_EL0") - cycles;
	ml_set_interrupts_enabled(istate);
	return cycles / 10000;
}

/* Services that keep IOKit from going quiet; configd's InterfaceNamer waits for quiet. */
static void
tulip_log_busy_services(void)
{
	IORegistryIterator *iter = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
	unsigned int count = 0;

	kprintf("stats: IOKit busy %u\n", IOService::getServiceRoot()->getBusyState());
	if (!iter) {
		return;
	}
	while (IORegistryEntry *entry = iter->getNextObject()) {
		IOService *service = OSDynamicCast(IOService, entry);
		if (service && service->getBusyState() && count++ < 6) {
			kprintf("stats: busy %s (%s) %u\n", service->getName(), service->getMetaClass()->getClassName(),
			    service->getBusyState());
		}
	}
	iter->release();
}

static void
tulip_stats_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t last[4] = {};

	for (;;) {
		IOSleep(10000);
		if (IOService::getServiceRoot()->getBusyState()) {
			tulip_log_busy_services();
		}
		uint64_t now[4], busy_ms;
		memcpy(now, tulip_emmc_stats, sizeof(now));
		absolutetime_to_nanoseconds(now[3] - last[3], &busy_ms);
		busy_ms /= NSEC_PER_MSEC;
		kprintf("stats: cpu %llu MHz (cpu %d) | emmc %llu reads %llu writes %llu KiB, busy %llu ms of 10000\n",
		    tulip_cpu_mhz(), cpu_number(), now[0] - last[0], now[1] - last[1], (now[2] - last[2]) >> 10, busy_ms);
		memcpy(last, now, sizeof(last));
	}
}

static void
tulip_tick_thread(void *arg __unused, wait_result_t wr __unused)
{
	for (unsigned int n = 0;; n++) {
		IODelay(1000000);
		kprintf("tick %u\n", n);
		tulip_gic_report();
	}
}

/* Restart and power off through PSCI; a panic restart keeps the panic on screen. */
int
TulipPlatformExpert::haltRestart(unsigned int type)
{
	switch (type) {
	case kPERestartCPU:
		tulip_psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
		break;
	case kPEHaltCPU:
		tulip_psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
		break;
	default:
		break;
	}
	return IODTPlatformExpert::haltRestart(type);
}

long
TulipPlatformExpert::getGMTTimeOfDay(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (long)(bootUTC + ns / NSEC_PER_SEC);
}

/* Nubs for /arm-io devices hang off the platform expert, so IOPlatformDevice name matching works. */
void
TulipPlatformExpert::processTopLevel(IORegistryEntry *root)
{
	IODTPlatformExpert::processTopLevel(root);

	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio) {
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}
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

	uint32_t timertest = 0;
	if (PE_parse_boot_argn("tulip_timertest", &timertest, sizeof(timertest)) && timertest) {
		tulip_timer_selftest();
	}

	uint32_t stats = 0;
	if (PE_parse_boot_argn("tulip_stats", &stats, sizeof(stats)) && stats) {
		thread_t thread;
		if (kernel_thread_start(tulip_stats_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
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
