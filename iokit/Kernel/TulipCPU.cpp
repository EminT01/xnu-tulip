/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>

extern "C" {
#include <kern/processor.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
}

#define PSCI_CPU_ON_64 0xc4000003u

extern "C" unsigned int LowResetVectorBase;

/* One CPU core: registers it with the scheduler, starts it with PSCI and signals it with SGIs. */
class TulipCPU : public IOCPU
{
	OSDeclareDefaultStructors(TulipCPU);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	virtual void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	virtual void haltCPU(void) APPLE_KEXT_OVERRIDE;
	virtual void signalCPU(IOCPU *target) APPLE_KEXT_OVERRIDE;
	virtual const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;

private:
	uint32_t physID;
	OSSharedPtr<const OSSymbol> cpuName;
};

OSDefineMetaClassAndStructors(TulipCPU, IOCPU);

static IOLock *gTulipCPULock;
static bool gTulipBootCPURegistered;
static unsigned int gTulipCPUsStarted;
static bool gTulipPSCIHVC;

void
tulip_cpu_init(void)
{
	gTulipCPULock = IOLockAlloc();

	IORegistryEntry *cpus = IORegistryEntry::fromPath("/cpus", gIODTPlane);
	if (cpus) {
		OSData *method = OSDynamicCast(OSData, cpus->getProperty("psci-method"));
		gTulipPSCIHVC = method && strncmp((const char *)method->getBytesNoCopy(), "hvc", method->getLength()) == 0;
		cpus->release();
	}
}

int64_t tulip_psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);

int64_t
tulip_psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 asm("x0") = fn;
	register uint64_t x1 asm("x1") = a1;
	register uint64_t x2 asm("x2") = a2;
	register uint64_t x3 asm("x3") = a3;

	if (gTulipPSCIHVC) {
		asm volatile ("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	} else {
		asm volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	}
	return (int64_t)(int32_t)x0;
}

bool
TulipCPU::start(IOService *provider)
{
	OSNumber *logical = OSDynamicCast(OSNumber, provider->getProperty("logical-cpu-id"));
	OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
	if (!logical || !reg || reg->getLength() < sizeof(uint32_t)) {
		return false;
	}
	if (!IOCPU::start(provider)) {
		return false;
	}

	const ml_topology_info_t *topology = ml_get_topology_info();
	const ml_topology_cpu_t *topo_cpu = &topology->cpus[logical->unsigned32BitValue()];
	bool boot = topo_cpu == topology->boot_cpu;
	physID = *(const uint32_t *)reg->getBytesNoCopy();

	char name[8];
	snprintf(name, sizeof(name), "cpu%u", topo_cpu->cpu_id);
	cpuName.reset(OSSymbol::withCString(name), OSNoRetain);

	ml_processor_info_t info = {};
	info.cpu_id = (cpu_id_t)this;
	info.phys_id = topo_cpu->phys_id;
	info.log_id = topo_cpu->cpu_id;
	info.cluster_id = topo_cpu->cluster_id;
	info.cluster_type = topo_cpu->cluster_type;
	info.l2_cache_size = topo_cpu->l2_cache_size;
	info.l2_cache_id = topo_cpu->l2_cache_id;
	info.l3_cache_size = topo_cpu->l3_cache_size;
	info.l3_cache_id = topo_cpu->l3_cache_id;

	/* The boot CPU must register first: the others need its cpu_id to receive IPIs. */
	IOLockLock(gTulipCPULock);
	while (!boot && !gTulipBootCPURegistered) {
		IOLockSleep(gTulipCPULock, &gTulipBootCPURegistered, THREAD_UNINT);
	}
	perfmon_interrupt_handler_func pmi_handler;
	if (ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler) != KERN_SUCCESS) {
		panic("TulipCPU: ml_processor_register failed for %s", name);
	}
	if (boot) {
		gTulipBootCPURegistered = true;
		IOLockWakeup(gTulipCPULock, &gTulipBootCPURegistered, false);
	}
	IOLockUnlock(gTulipCPULock);

	setCPUNumber(topo_cpu->cpu_id);
	setCPUState(kIOCPUStateUninitalized);
	registerService();

	if (processor_start(machProcessor) != KERN_SUCCESS) {
		panic("TulipCPU: processor_start failed for %s", name);
	}

	IOLockLock(gTulipCPULock);
	bool all = ++gTulipCPUsStarted == topology->num_cpus;
	IOLockUnlock(gTulipCPULock);
	if (all) {
		ml_cpu_init_completed();
		publishResource(gIOAllCPUInitializedKey, kOSBooleanTrue);
	}
	return true;
}

void
TulipCPU::initCPU(bool boot __unused)
{
	setCPUState(kIOCPUStateRunning);

	/* The first IPI clears SIGPdisabled, which every CPU starts with. */
	tulip_gic_send_ipi(physID);
	while (ml_get_interrupts_enabled() && !ml_cpu_signal_is_enabled()) {
		OSMemoryBarrier();
	}
}

void
TulipCPU::quiesceCPU(void)
{
}

kern_return_t
TulipCPU::startCPU(vm_offset_t start_paddr __unused, vm_offset_t arg_paddr __unused)
{
	int64_t ret = tulip_psci_call(PSCI_CPU_ON_64, physID, ml_vtophys((vm_offset_t)&LowResetVectorBase), 0);
	if (ret != 0) {
		kprintf("TulipCPU: PSCI CPU_ON 0x%x returned %lld\n", physID, ret);
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}

void
TulipCPU::haltCPU(void)
{
	setCPUState(kIOCPUStateStopped);
	processor_exit(machProcessor);
}

void
TulipCPU::signalCPU(IOCPU *target)
{
	tulip_gic_send_ipi(static_cast<TulipCPU *>(target)->physID);
}

const OSSymbol *
TulipCPU::getCPUName(void)
{
	return cpuName.get();
}
