/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/hid/IOHIDEvent.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include "hid/include/IOHIDEventService.h"

extern "C" {
#include <kern/clock.h>
#include <pexpert/pexpert.h>
}

/* Qualcomm SPMI PMIC arbiter v2/v3 (Linux drivers/spmi/spmi-pmic-arb.c), read-only through the observer channel. */
#define ARB_VERSION        0x0
#define ARB_APID_MAP(n)    (0x800 + 4 * (n))
#define ARB_APIDS          512
#define ARB_OBS(ee, apid)  (0x1000 * (ee) + 0x8000 * (apid))
#define ARB_CMD            0x00
#define ARB_STATUS         0x08
#define ARB_RDATA0         0x18
#define ARB_OP_EXT_READL   1
#define ARB_STATUS_DONE    (1u << 0)
#define ARB_STATUS_ERRORS  0xeu
#define ARB_EE             0

/* PM660 power-on block (SID 0): real-time status of the power key (KPDPWR) and volume down (RESIN). */
#define PON_RT_STS         0x810
#define PON_KPDPWR         (1u << 0)
#define PON_RESIN          (1u << 1)
/* PM660L GPIO 7 (SID 2): volume up, active low. Linux reads an input's level from RT_STS. */
#define VOLUP_STATUS1      0xc608
#define VOLUP_RT_STS       0xc610
#define GPIO_LEVEL         (1u << 0)

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
	IOMemoryMap *fCore;
	IOMemoryMap *fObserver;
	IOTimerEventSource *fTimer;
	uint16_t fApid[2];
	bool fDown[kKeys];

	bool findApid(uint8_t sid, uint16_t addr, uint16_t *apid);
	bool read(unsigned int periph, uint8_t sid, uint16_t addr, uint8_t *value);
	static void poll(OSObject *owner, IOTimerEventSource *sender);
};

OSDefineMetaClassAndStructors(TulipButtons, IOHIDEventService);

bool
TulipButtons::findApid(uint8_t sid, uint16_t addr, uint16_t *apid)
{
	volatile uint32_t *core = (volatile uint32_t *)fCore->getVirtualAddress();
	uint16_t ppid = (uint16_t)(sid << 8 | (addr >> 8));

	for (uint16_t i = 0; i < ARB_APIDS; i++) {
		uint32_t map = core[ARB_APID_MAP(i) / 4];
		if (map && ((map >> 8) & 0xfff) == ppid) {
			*apid = i;
			return true;
		}
	}
	return false;
}

bool
TulipButtons::read(unsigned int periph, uint8_t sid, uint16_t addr, uint8_t *value)
{
	volatile uint8_t *obs = (volatile uint8_t *)fObserver->getVirtualAddress() + ARB_OBS(ARB_EE, fApid[periph]);
	uint32_t status = 0;

	*(volatile uint32_t *)(obs + ARB_CMD) = ARB_OP_EXT_READL << 27 | (addr & 0xff) << 4;
	for (int i = 0; i < 1000 && !(status & ARB_STATUS_DONE); i++) {
		IODelay(1);
		status = *(volatile uint32_t *)(obs + ARB_STATUS);
	}
	if (!(status & ARB_STATUS_DONE) || (status & ARB_STATUS_ERRORS)) {
		kprintf("TulipButtons: read sid %u 0x%04x: status 0x%x\n", sid, addr, status);
		return false;
	}
	*value = (uint8_t)*(volatile uint32_t *)(obs + ARB_RDATA0);
	return true;
}

bool
TulipButtons::start(IOService *provider)
{
	fCore = provider->mapDeviceMemoryWithIndex(0);
	fObserver = provider->mapDeviceMemoryWithIndex(1);
	if (!fCore || !fObserver) {
		IOLog("TulipButtons: no SPMI registers\n");
		return false;
	}
	uint32_t version = *(volatile uint32_t *)(fCore->getVirtualAddress() + ARB_VERSION);
	if (version < 0x20010000 || version >= 0x50000000) {
		IOLog("TulipButtons: SPMI arbiter version 0x%08x is not v2 or v3\n", version);
		return false;
	}
	if (!findApid(0, PON_RT_STS, &fApid[0]) || !findApid(2, VOLUP_RT_STS, &fApid[1])) {
		IOLog("TulipButtons: no SPMI channel for the keys\n");
		return false;
	}
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	IOWorkLoop *wl = getWorkLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &TulipButtons::poll);
	if (!wl || !fTimer || wl->addEventSource(fTimer) != kIOReturnSuccess) {
		return false;
	}
	uint8_t pon = 0, status1 = 0, rt = 0;
	read(0, 0, PON_RT_STS, &pon);
	read(1, 2, VOLUP_STATUS1, &status1);
	read(1, 2, VOLUP_RT_STS, &rt);
	IOLog("TulipButtons: SPMI arbiter 0x%08x, channels %u and %u, PON 0x%02x, volume up status 0x%02x rt 0x%02x\n",
	    version, fApid[0], fApid[1], pon, status1, rt);
	registerService();
	fTimer->setTimeoutMS(POLL_MS);
	return true;
}

void
TulipButtons::poll(OSObject *owner, IOTimerEventSource *sender)
{
	TulipButtons *self = (TulipButtons *)owner;
	uint8_t pon, gpio;

	if (self->read(0, 0, PON_RT_STS, &pon) && self->read(1, 2, VOLUP_RT_STS, &gpio)) {
		bool down[kKeys] = {
			(pon & PON_KPDPWR) != 0, !(gpio & GPIO_LEVEL), (pon & PON_RESIN) != 0,
		};
		for (int k = 0; k < kKeys; k++) {
			if (down[k] == self->fDown[k]) {
				continue;
			}
			self->fDown[k] = down[k];
			kprintf("TulipButtons: %s %s\n", gKeyName[k], down[k] ? "down" : "up");
			IOHIDEvent *event = IOHIDEvent::keyboardEvent(mach_absolute_time(), kHIDPage_Consumer, gKeyUsage[k], down[k]);
			if (event) {
				self->dispatchEvent(event);
				event->release();
			}
		}
	}
	sender->setTimeoutMS(POLL_MS);
}
