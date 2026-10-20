/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_TULIPTLMM_H
#define _IOKIT_TULIPTLMM_H

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOMemoryDescriptor.h>

/* Pull settings of a pin. */
enum {
	kTulipPullNone,
	kTulipPullDown,
	kTulipPullKeeper,
	kTulipPullUp,
};

/* Interrupt triggers, as Linux's IRQ_TYPE_* in interrupt specifiers. */
enum {
	kTulipTriggerRising = 1,
	kTulipTriggerFalling = 2,
	kTulipTriggerBoth = 3,
	kTulipTriggerHigh = 4,
	kTulipTriggerLow = 8,
};

/* Qualcomm TLMM: pin setup for other drivers, and the GPIO interrupts behind its one summary SPI. */
class TulipTLMM : public IOInterruptController
{
	OSDeclareDefaultStructors(TulipTLMM);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getInterruptType(IOService *nub, int source, int *interruptType) APPLE_KEXT_OVERRIDE;
	virtual void initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual int getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;

	/* Finds the published TLMM; false if it does not appear within a few seconds. */
	static TulipTLMM *wait(void);

	bool configure(uint32_t gpio, uint32_t function, uint32_t pull, uint32_t driveMA, bool output);
	bool get(uint32_t gpio);
	void set(uint32_t gpio, bool high);
	/* Latches a trigger in the pin's status without an interrupt, for drivers that poll. */
	bool latch(uint32_t gpio, uint32_t trigger);
	bool takeLatched(uint32_t gpio);
	/* True when GPIO interrupts reach the CPU. */
	bool interruptsReady() const { return fReady; }

private:
	IOMemoryMap *fTile[3];
	OSData *fTiles;
	uint32_t fPins;
	uint32_t fReservedFirst, fReservedCount;
	uint8_t *fType;
	uint32_t *fUsed;
	uint32_t fUsedCount;
	uint32_t fSpurious;
	bool fReady;

	volatile uint32_t *pin(uint32_t gpio, uint32_t reg);
	void setTrigger(uint32_t gpio, uint32_t trigger, bool enable);
	void summary(void *refCon, IOService *nub, int source);
};

#endif /* _IOKIT_TULIPTLMM_H */
