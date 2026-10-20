/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterrupts.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" {
#include <machine/machine_routines.h>
#include <pexpert/arm64/board_config.h>
}

#define GICD_CTLR          0x0000
#define GICD_TYPER         0x0004
#define GICD_IGROUPR       0x0080
#define GICD_ISENABLER     0x0100
#define GICD_ICENABLER     0x0180
#define GICD_ISPENDR       0x0200
#define GICD_ICPENDR       0x0280
#define GICD_IPRIORITYR    0x0400
#define GICD_ICFGR         0x0c00
#define GICD_IROUTER       0x6000
#define GICD_CTLR_RWP      (1u << 31)
#define GICD_TYPER_LINES   0x1f

#define FIRST_SPI          32
#define MAX_INTID          1020
#define SPI_PRIORITY       0xa0

/* Second cell of an interrupt specifier, as Linux's IRQ_TYPE_*. GIC SPIs are rising edge or level high. */
#define TRIGGER_EDGE_MASK  0x3

/* The shared peripheral interrupts (SPIs) of a GICv3. All of them go to the boot CPU as Group 1 IRQs. */
class TulipGIC : public IOInterruptController
{
	OSDeclareDefaultStructors(TulipGIC);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getInterruptType(IOService *nub, int source, int *interruptType) APPLE_KEXT_OVERRIDE;
	virtual IOReturn handleInterrupt(void *refCon, IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	virtual void initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual int getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;

private:
	IOMemoryMap *fMap;
	uint32_t fLines;
	uint64_t fRoute;
	uint8_t *fType;
	volatile uint32_t fTestHits;

	volatile uint32_t *reg(uint32_t offset);
	void setBit(uint32_t base, uint32_t intid);
	void configure(uint32_t intid, int type);
	void selfTest(uint32_t intid);
	static void dispatch(void *target, uint32_t intid);
	static void testHandler(void *target, void *refCon, void *nub, int source);
};

OSDefineMetaClassAndStructors(TulipGIC, IOInterruptController);

static int
triggerType(IOService *nub, int source)
{
	OSArray *specs = OSDynamicCast(OSArray, nub->getProperty(gIOInterruptSpecifiersKey));
	OSData *spec = specs ? OSDynamicCast(OSData, specs->getObject((unsigned int)source)) : NULL;
	if (spec && spec->getLength() >= 2 * sizeof(uint32_t) &&
	    (((const uint32_t *)spec->getBytesNoCopy())[1] & TRIGGER_EDGE_MASK)) {
		return kIOInterruptTypeEdge;
	}
	return kIOInterruptTypeLevel;
}

volatile uint32_t *
TulipGIC::reg(uint32_t offset)
{
	return (volatile uint32_t *)(fMap->getVirtualAddress() + offset);
}

void
TulipGIC::setBit(uint32_t base, uint32_t intid)
{
	*reg(base + (intid / 32) * 4) = 1u << (intid % 32);
}

void
TulipGIC::configure(uint32_t intid, int type)
{
	uint32_t word = (intid / 32) * 4, cfg = GICD_ICFGR + (intid / 16) * 4, prio = GICD_IPRIORITYR + (intid & ~3u);
	uint32_t cfgShift = (intid % 16) * 2 + 1, prioShift = (intid % 4) * 8;

	fType[intid] = (uint8_t)type;
	IOSimpleLockLock(controllerLock);
	setBit(GICD_ICENABLER, intid);
	*reg(GICD_IGROUPR + word) |= 1u << (intid % 32);
	*reg(prio) = (*reg(prio) & ~(0xffu << prioShift)) | (SPI_PRIORITY << prioShift);
	if (type == kIOInterruptTypeEdge) {
		*reg(cfg) |= 1u << cfgShift;
	} else {
		*reg(cfg) &= ~(1u << cfgShift);
	}
	*(volatile uint64_t *)(fMap->getVirtualAddress() + GICD_IROUTER + 8 * intid) = fRoute;
	setBit(GICD_ICPENDR, intid);
	IOSimpleLockUnlock(controllerLock);
}

IOReturn
TulipGIC::getInterruptType(IOService *nub, int source, int *interruptType)
{
	if (!interruptType) {
		return kIOReturnBadArgument;
	}
	*interruptType = triggerType(nub, source);
	return kIOReturnSuccess;
}

void
TulipGIC::initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	configure((uint32_t)vectorNumber, triggerType(vector->nub, vector->source));
}

int
TulipGIC::getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector __unused)
{
	return fType[vectorNumber];
}

void
TulipGIC::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector __unused)
{
	setBit(GICD_ICENABLER, (uint32_t)vectorNumber);
}

void
TulipGIC::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector __unused)
{
	setBit(GICD_ISENABLER, (uint32_t)vectorNumber);
}

void
TulipGIC::causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector __unused)
{
	setBit(GICD_ISPENDR, (uint32_t)vectorNumber);
}

void
TulipGIC::dispatch(void *target, uint32_t intid)
{
	((TulipGIC *)target)->handleInterrupt(NULL, NULL, (int)intid);
}

IOReturn
TulipGIC::handleInterrupt(void *refCon __unused, IOService *nub __unused, int source)
{
	if (source < FIRST_SPI || (uint32_t)source >= fLines) {
		return kIOReturnInvalid;
	}
	IOInterruptVector *vector = &vectors[source];
	vector->interruptActive = 1;
	OSMemoryBarrier();
	if (vector->interruptRegistered && !vector->interruptDisabledSoft) {
		vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
		if (vector->interruptDisabledSoft) {
			vector->interruptDisabledHard = 1;
			disableVectorHard(source, vector);
		}
	} else {
		vector->interruptDisabledHard = 1;
		disableVectorHard(source, vector);
	}
	vector->interruptActive = 0;
	return kIOReturnSuccess;
}

void
TulipGIC::testHandler(void *target, void *refCon __unused, void *nub __unused, int source __unused)
{
	((TulipGIC *)target)->fTestHits++;
}

/* Raises an SPI by software and checks that it arrives, so a missing route shows in the log. */
void
TulipGIC::selfTest(uint32_t intid)
{
	if (intid < FIRST_SPI || intid >= fLines) {
		return;
	}
	IOInterruptVector *vector = &vectors[intid];
	configure(intid, kIOInterruptTypeEdge);
	uint32_t group = (*reg(GICD_IGROUPR + (intid / 32) * 4) >> (intid % 32)) & 1;
	vector->handler = &TulipGIC::testHandler;
	vector->target = this;
	vector->interruptDisabledSoft = 0;
	vector->interruptDisabledHard = 0;
	vector->interruptRegistered = 1;
	OSSynchronizeIO();
	enableVector(intid, vector);
	causeVector(intid, vector);
	for (int i = 0; i < 100 && !fTestHits; i++) {
		IOSleep(1);
	}
	disableVectorHard(intid, vector);
	vector->interruptRegistered = 0;
	vector->interruptDisabledSoft = 1;
	vector->interruptDisabledHard = 1;
	vector->handler = NULL;
	IOLog("TulipGIC: self-test INTID %u: %s (%u hits, group %u)\n", intid, fTestHits ? "delivered" : "NOT delivered",
	    fTestHits, group);
}

bool
TulipGIC::start(IOService *provider)
{
	if (!IOInterruptController::start(provider)) {
		return false;
	}
	fMap = provider->mapDeviceMemoryWithIndex(0);
	if (!fMap) {
		IOLog("TulipGIC: no distributor\n");
		return false;
	}
	fLines = ((*reg(GICD_TYPER) & GICD_TYPER_LINES) + 1) * 32;
	if (fLines > MAX_INTID) {
		fLines = MAX_INTID;
	}
	fRoute = ml_get_topology_info()->boot_cpu->phys_id & 0xffffff;
	controllerLock = IOSimpleLockAlloc();
	vectors = IONewZero(IOInterruptVector, fLines);
	fType = IONewZero(uint8_t, fLines);
	if (!controllerLock || !vectors || !fType) {
		return false;
	}
	for (uint32_t i = 0; i < fLines; i++) {
		vectors[i].interruptLock = IOLockAlloc();
		vectors[i].interruptDisabledSoft = 1;
		vectors[i].interruptDisabledHard = 1;
		if (!vectors[i].interruptLock) {
			return false;
		}
	}

	tulip_irq_target = this;
	OSMemoryBarrier();
	tulip_irq_handler = &TulipGIC::dispatch;

	OSData *test = OSDynamicCast(OSData, provider->getProperty("tulip-test-intid"));
	if (test && test->getLength() == sizeof(uint32_t)) {
		selfTest(*(const uint32_t *)test->getBytesNoCopy());
	}

	const OSSymbol *name = IODTInterruptControllerName(provider);
	if (!name) {
		IOLog("TulipGIC: no phandle\n");
		return false;
	}
	getPlatform()->registerInterruptController(const_cast<OSSymbol *>(name), this);
	name->release();
	IOLog("TulipGIC: %u interrupt lines, SPIs to CPU 0x%llx\n", fLines, fRoute);
	registerService();
	return true;
}
