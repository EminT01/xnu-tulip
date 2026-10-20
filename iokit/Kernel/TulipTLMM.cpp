/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterrupts.h>
#include "TulipTLMM.h"

/* Registers of one pin, 0x1000 apart in its tile (Linux drivers/pinctrl/qcom/pinctrl-sdm660.c). */
#define PIN_CFG                0x0
#define PIN_IO                 0x4
#define PIN_INTR_CFG           0x8
#define PIN_INTR_STATUS        0xc
#define PIN_STRIDE             0x1000
#define PIN_IO_IN              (1u << 0)
#define PIN_IO_OUT             (1u << 1)
#define CFG_FUNCTION_SHIFT     2
#define CFG_DRIVE_SHIFT        6
#define CFG_OUTPUT             (1u << 9)
#define INTR_ENABLE            (1u << 0)
#define INTR_POLARITY_HIGH     (1u << 1)
#define INTR_DETECT_SHIFT      2
#define INTR_DETECT_LEVEL      0
#define INTR_DETECT_RISING     1
#define INTR_DETECT_FALLING    2
#define INTR_DETECT_BOTH       3
#define INTR_RAW_STATUS        (1u << 4)
#define INTR_TARGET_APPS       (3u << 5)
#define INTR_TARGET_MASK       (7u << 5)
#define INTR_STATUS            (1u << 0)

/* Summary interrupts in a row that no registered pin claims before the summary is turned off. */
#define MAX_SPURIOUS           10000
#define MAX_USED               16

OSDefineMetaClassAndStructors(TulipTLMM, IOInterruptController);

static bool
isEdge(uint32_t trigger)
{
	return trigger & (kTulipTriggerRising | kTulipTriggerFalling);
}

static uint32_t
specTrigger(IOService *nub, int source)
{
	OSArray *specs = OSDynamicCast(OSArray, nub->getProperty(gIOInterruptSpecifiersKey));
	OSData *spec = specs ? OSDynamicCast(OSData, specs->getObject((unsigned int)source)) : NULL;
	if (spec && spec->getLength() >= 2 * sizeof(uint32_t)) {
		return ((const uint32_t *)spec->getBytesNoCopy())[1];
	}
	return kTulipTriggerHigh;
}

TulipTLMM *
TulipTLMM::wait(void)
{
	OSDictionary *matching = IOService::serviceMatching("TulipTLMM");
	IOService *service = matching ? IOService::waitForMatchingService(matching, 5 * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(matching);
	TulipTLMM *tlmm = OSDynamicCast(TulipTLMM, service);
	if (!tlmm) {
		OSSafeReleaseNULL(service);
	}
	return tlmm;
}

volatile uint32_t *
TulipTLMM::pin(uint32_t gpio, uint32_t reg)
{
	if (gpio >= fPins || (gpio >= fReservedFirst && gpio < fReservedFirst + fReservedCount)) {
		kprintf("TulipTLMM: no access to GPIO %u\n", gpio);
		return NULL;
	}
	uint8_t tile = ((const uint8_t *)fTiles->getBytesNoCopy())[gpio];
	if (tile >= 3 || !fTile[tile]) {
		return NULL;
	}
	return (volatile uint32_t *)(fTile[tile]->getVirtualAddress() + gpio * PIN_STRIDE + reg);
}

bool
TulipTLMM::configure(uint32_t gpio, uint32_t function, uint32_t pull, uint32_t driveMA, bool output)
{
	volatile uint32_t *cfg = pin(gpio, PIN_CFG);
	if (!cfg || driveMA < 2 || driveMA > 16) {
		return false;
	}
	*cfg = (pull & 3) | (function & 0xf) << CFG_FUNCTION_SHIFT | (driveMA / 2 - 1) << CFG_DRIVE_SHIFT |
	    (output ? CFG_OUTPUT : 0);
	return true;
}

bool
TulipTLMM::get(uint32_t gpio)
{
	volatile uint32_t *io = pin(gpio, PIN_IO);
	return io && (*io & PIN_IO_IN);
}

void
TulipTLMM::set(uint32_t gpio, bool high)
{
	volatile uint32_t *io = pin(gpio, PIN_IO);
	if (io) {
		*io = high ? PIN_IO_OUT : 0;
	}
}

void
TulipTLMM::setTrigger(uint32_t gpio, uint32_t trigger, bool enable)
{
	volatile uint32_t *cfg = pin(gpio, PIN_INTR_CFG);
	if (!cfg) {
		return;
	}
	uint32_t detect = INTR_DETECT_LEVEL, polarity = 0;
	switch (trigger) {
	case kTulipTriggerRising:
		detect = INTR_DETECT_RISING;
		polarity = INTR_POLARITY_HIGH;
		break;
	case kTulipTriggerFalling:
		detect = INTR_DETECT_FALLING;
		polarity = INTR_POLARITY_HIGH;
		break;
	case kTulipTriggerBoth:
		detect = INTR_DETECT_BOTH;
		polarity = INTR_POLARITY_HIGH;
		break;
	case kTulipTriggerHigh:
		polarity = INTR_POLARITY_HIGH;
		break;
	default:
		break;
	}
	*cfg = INTR_TARGET_APPS | INTR_RAW_STATUS | detect << INTR_DETECT_SHIFT | polarity | (enable ? INTR_ENABLE : 0);
}

bool
TulipTLMM::latch(uint32_t gpio, uint32_t trigger)
{
	volatile uint32_t *status = pin(gpio, PIN_INTR_STATUS);
	if (!status) {
		return false;
	}
	setTrigger(gpio, trigger, false);
	*status = 0;
	return true;
}

bool
TulipTLMM::takeLatched(uint32_t gpio)
{
	volatile uint32_t *status = pin(gpio, PIN_INTR_STATUS);
	if (!status || !(*status & INTR_STATUS)) {
		return false;
	}
	*status = 0;
	return true;
}

IOReturn
TulipTLMM::getInterruptType(IOService *nub, int source, int *interruptType)
{
	if (!interruptType) {
		return kIOReturnBadArgument;
	}
	*interruptType = isEdge(specTrigger(nub, source)) ? kIOInterruptTypeEdge : kIOInterruptTypeLevel;
	return kIOReturnSuccess;
}

void
TulipTLMM::initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	uint32_t gpio = (uint32_t)vectorNumber, trigger = specTrigger(vector->nub, vector->source);
	fType[gpio] = (uint8_t)trigger;
	latch(gpio, trigger);
	IOSimpleLockLock(controllerLock);
	bool known = false;
	for (uint32_t i = 0; i < fUsedCount; i++) {
		known |= fUsed[i] == gpio;
	}
	if (!known && fUsedCount < MAX_USED) {
		fUsed[fUsedCount] = gpio;
		OSMemoryBarrier();
		fUsedCount++;
	}
	IOSimpleLockUnlock(controllerLock);
}

int
TulipTLMM::getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector __unused)
{
	return isEdge(fType[vectorNumber]) ? kIOInterruptTypeEdge : kIOInterruptTypeLevel;
}

void
TulipTLMM::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector __unused)
{
	setTrigger((uint32_t)vectorNumber, fType[vectorNumber], false);
}

void
TulipTLMM::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector __unused)
{
	setTrigger((uint32_t)vectorNumber, fType[vectorNumber], true);
}

/* The summary SPI: runs the handler of every registered pin whose status is set. */
void
TulipTLMM::summary(void *refCon __unused, IOService *nub __unused, int source __unused)
{
	bool claimed = false;
	for (uint32_t i = 0; i < fUsedCount; i++) {
		uint32_t gpio = fUsed[i];
		IOInterruptVector *vector = &vectors[gpio];
		volatile uint32_t *status = pin(gpio, PIN_INTR_STATUS);
		if (!status || !(*status & INTR_STATUS)) {
			continue;
		}
		claimed = true;
		bool edge = isEdge(fType[gpio]);
		if (edge) {
			*status = 0;
		}
		vector->interruptActive = 1;
		OSMemoryBarrier();
		if (vector->interruptRegistered && !vector->interruptDisabledSoft) {
			vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
			if (vector->interruptDisabledSoft) {
				vector->interruptDisabledHard = 1;
				disableVectorHard(gpio, vector);
			}
		} else {
			vector->interruptDisabledHard = 1;
			disableVectorHard(gpio, vector);
		}
		if (!edge) {
			*status = 0;
		}
		vector->interruptActive = 0;
	}
	if (claimed) {
		fSpurious = 0;
	} else if (++fSpurious == MAX_SPURIOUS) {
		kprintf("TulipTLMM: %u summary interrupts that no pin claims, turning the summary off\n", fSpurious);
		fReady = false;
		getProvider()->disableInterrupt(0);
	}
}

bool
TulipTLMM::start(IOService *provider)
{
	if (!IOInterruptController::start(provider)) {
		return false;
	}
	fTiles = OSDynamicCast(OSData, provider->getProperty("tiles"));
	if (!fTiles || provider->getDeviceMemoryCount() < 3) {
		IOLog("TulipTLMM: no tiles\n");
		return false;
	}
	fPins = fTiles->getLength();
	for (int i = 0; i < 3; i++) {
		fTile[i] = provider->mapDeviceMemoryWithIndex(i);
		if (!fTile[i]) {
			IOLog("TulipTLMM: cannot map tile %d\n", i);
			return false;
		}
	}
	OSData *reserved = OSDynamicCast(OSData, provider->getProperty("reserved-gpios"));
	if (reserved && reserved->getLength() == 2 * sizeof(uint32_t)) {
		fReservedFirst = ((const uint32_t *)reserved->getBytesNoCopy())[0];
		fReservedCount = ((const uint32_t *)reserved->getBytesNoCopy())[1];
	}
	controllerLock = IOSimpleLockAlloc();
	vectors = IONewZero(IOInterruptVector, fPins);
	fType = IONewZero(uint8_t, fPins);
	fUsed = IONewZero(uint32_t, MAX_USED);
	if (!controllerLock || !vectors || !fType || !fUsed) {
		return false;
	}
	for (uint32_t i = 0; i < fPins; i++) {
		vectors[i].interruptLock = IOLockAlloc();
		vectors[i].interruptDisabledSoft = 1;
		vectors[i].interruptDisabledHard = 1;
		if (!vectors[i].interruptLock) {
			return false;
		}
	}

	/* Pins that the boot loader left routed to us would raise the summary with nobody to claim them. */
	for (uint32_t gpio = 0; gpio < fPins; gpio++) {
		if (gpio >= fReservedFirst && gpio < fReservedFirst + fReservedCount) {
			continue;
		}
		volatile uint32_t *cfg = pin(gpio, PIN_INTR_CFG);
		if (cfg && (*cfg & INTR_ENABLE) && (*cfg & INTR_TARGET_MASK) == INTR_TARGET_APPS) {
			IOLog("TulipTLMM: GPIO %u interrupt was on (0x%x), turning it off\n", gpio, *cfg);
			*cfg &= ~INTR_ENABLE;
		}
	}

	/* Without the GIC driver, registering the summary would wait for it forever. */
	OSDictionary *matching = serviceMatching("TulipGIC");
	IOService *gic = matching ? waitForMatchingService(matching, 5 * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(matching);
	const OSSymbol *name = IODTInterruptControllerName(provider);
	if (gic && name &&
	    provider->registerInterrupt(0, this, OSMemberFunctionCast(IOInterruptAction, this, &TulipTLMM::summary)) ==
	    kIOReturnSuccess && provider->enableInterrupt(0) == kIOReturnSuccess) {
		getPlatform()->registerInterruptController(const_cast<OSSymbol *>(name), this);
		fReady = true;
	}
	OSSafeReleaseNULL(gic);
	OSSafeReleaseNULL(name);
	IOLog("TulipTLMM: %u pins, interrupts %s\n", fPins, fReady ? "on" : "off");
	registerService();
	return true;
}
