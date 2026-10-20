/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/hid/IOHIDEvent.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include "hid/include/IOHIDEventService.h"
#include "TulipSPMIInterrupts.h"

extern "C" {
#include <kern/clock.h>
#include <pexpert/pexpert.h>
}

/* PM660 power-on block (SID 0): real-time status of the power key (KPDPWR) and volume down (RESIN). */
#define PON_RT_STS         0x810
#define PON_KPDPWR         (1u << 0)
#define PON_RESIN          (1u << 1)
/* PM660L GPIO 7 (SID 2): volume up, active low. Linux reads an input's level from RT_STS. */
#define VOLUP_RT_STS       0xc610
#define GPIO_LEVEL         (1u << 0)
#define PON_BASE           0x800
#define VOLUP_BASE         0xc600

#define POLL_MS            20

enum {
	kKeyPower,
	kKeyVolumeUp,
	kKeyVolumeDown,
	kKeys,
};

static const UInt32 gKeyUsage[kKeys] = {
	kHIDUsage_Csmr_Power, kHIDUsage_Csmr_VolumeIncrement, kHIDUsage_Csmr_VolumeDecrement,
};
static const char *const gKeyName[kKeys] = { "power", "volume up", "volume down" };

class TulipButtons : public IOHIDEventService
{
	OSDeclareDefaultStructors(TulipButtons);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Consumer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Csmr_ConsumerControl; }

protected:
	virtual const char *getProduct() APPLE_KEXT_OVERRIDE { return "Buttons"; }

private:
	TulipSPMI fSPMI;
	IOTimerEventSource *fTimer;
	IOInterruptEventSource *fInterrupt;
	IONotifier *fNotifier;
	volatile bool fInterrupts;
	bool fDown[kKeys];

	void update();
	static void poll(OSObject *owner, IOTimerEventSource *sender);
	static void interrupt(OSObject *owner, IOInterruptEventSource *sender, int count);
	static bool published(void *target, void *refCon, IOService *service, IONotifier *notifier);
};

OSDefineMetaClassAndStructors(TulipButtons, IOHIDEventService);

bool
TulipButtons::start(IOService *provider)
{
	if (!fSPMI.init(provider, 0, 1)) {
		IOLog("TulipButtons: no SPMI arbiter v2 or v3 (version 0x%08x)\n", fSPMI.version());
		return false;
	}
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	IOWorkLoop *wl = getWorkLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &TulipButtons::poll);
	fInterrupt = IOInterruptEventSource::interruptEventSource(this, &TulipButtons::interrupt);
	if (!wl || !fTimer || !fInterrupt || wl->addEventSource(fTimer) != kIOReturnSuccess ||
	    wl->addEventSource(fInterrupt) != kIOReturnSuccess) {
		return false;
	}
	IOLog("TulipButtons: SPMI arbiter 0x%08x\n", fSPMI.version());
	registerService();
	fTimer->setTimeoutMS(POLL_MS);
	fNotifier = addMatchingNotification(gIOFirstPublishNotification, serviceMatching("TulipSPMIInterrupts"),
	    &TulipButtons::published, this);
	return true;
}

bool
TulipButtons::published(void *target, void *refCon __unused, IOService *service, IONotifier *notifier)
{
	TulipButtons *self = (TulipButtons *)target;
	TulipSPMIInterrupts *pmic = OSDynamicCast(TulipSPMIInterrupts, service);
	notifier->remove();
	if (pmic && pmic->enable(0, PON_BASE, 0, kTulipPMICBoth, self->fInterrupt) &&
	    pmic->enable(0, PON_BASE, 1, kTulipPMICBoth, self->fInterrupt) &&
	    pmic->enable(2, VOLUP_BASE, 0, kTulipPMICBoth, self->fInterrupt)) {
		self->fInterrupts = true;
		self->fInterrupt->interruptOccurred(NULL, NULL, 0);
		IOLog("TulipButtons: interrupts\n");
	}
	return true;
}

void
TulipButtons::update()
{
	uint8_t pon, gpio;

	if (fSPMI.read(0, PON_RT_STS, &pon) && fSPMI.read(2, VOLUP_RT_STS, &gpio)) {
		bool down[kKeys] = {
			(pon & PON_KPDPWR) != 0, !(gpio & GPIO_LEVEL), (pon & PON_RESIN) != 0,
		};
		for (int k = 0; k < kKeys; k++) {
			if (down[k] == fDown[k]) {
				continue;
			}
			fDown[k] = down[k];
			kprintf("TulipButtons: %s %s\n", gKeyName[k], down[k] ? "down" : "up");
			IOHIDEvent *event = IOHIDEvent::keyboardEvent(mach_absolute_time(), kHIDPage_Consumer, gKeyUsage[k], down[k]);
			if (event) {
				dispatchEvent(event);
				event->release();
			}
		}
	}
}

void
TulipButtons::interrupt(OSObject *owner, IOInterruptEventSource *sender __unused, int count __unused)
{
	((TulipButtons *)owner)->update();
}

void
TulipButtons::poll(OSObject *owner, IOTimerEventSource *sender)
{
	TulipButtons *self = (TulipButtons *)owner;
	self->update();
	if (!self->fInterrupts) {
		sender->setTimeoutMS(POLL_MS);
	}
}
