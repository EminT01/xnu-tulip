/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_TULIPSPMIINTERRUPTS_H
#define _IOKIT_TULIPSPMIINTERRUPTS_H

#include <IOKit/IOService.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "TulipSPMI.h"

/* Triggers of a PMIC peripheral interrupt. */
enum {
	kTulipPMICRising = 1,
	kTulipPMICFalling = 2,
	kTulipPMICBoth = 3,
};

/* PMIC peripheral interrupts behind the SPMI arbiter's summary SPI, delivered to clients' event sources. */
class TulipSPMIInterrupts : public IOService
{
	OSDeclareDefaultStructors(TulipSPMIInterrupts);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	/* Finds the published service; NULL if it does not appear within a few seconds. */
	static TulipSPMIInterrupts *wait(void);

	/* Turns on interrupt bit of the peripheral at sid:base; each one runs client's action. */
	bool enable(uint8_t sid, uint16_t base, uint8_t bit, uint32_t trigger, IOInterruptEventSource *client);

private:
	enum { kMaxClients = 16 };
	struct Client {
		uint16_t apid;
		uint8_t sid;
		uint16_t base;
		uint8_t bit;
		IOInterruptEventSource *source;
	};

	TulipSPMI fSPMI;
	IOMemoryMap *fIntr;
	IOWorkLoop *fWorkLoop;
	IOInterruptEventSource *fSummary;
	IOLock *fLock;
	Client fClients[kMaxClients];
	uint32_t fClientCount;

	volatile uint32_t *intr(uint32_t offset);
	void summary(IOInterruptEventSource *sender, int count);
	bool dispatch(uint16_t apid);
};

#endif /* _IOKIT_TULIPSPMIINTERRUPTS_H */
