/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOInterrupts.h>
#include "TulipSPMIInterrupts.h"

/* Interrupt registers of a PMIC peripheral (Linux drivers/spmi/spmi-pmic-arb.c, QPNPINT_REG_*). */
#define INT_SET_TYPE        0x11
#define INT_POLARITY_HIGH   0x12
#define INT_POLARITY_LOW    0x13
#define INT_LATCHED_CLR     0x14
#define INT_EN_SET          0x15
#define INT_EN_CLR          0x16

/* Arbiter v3 interrupt block, "intr" in Linux: per-APID accumulator enable, status and clear, and per-EE summary. */
#define ACC_ENABLE(apid)    (0x1000 * (apid))
#define IRQ_STATUS(apid)    (0x1000 * (apid) + 0x4)
#define IRQ_CLEAR(apid)     (0x1000 * (apid) + 0x8)
#define OWNER_ACC_STATUS(n) (0x200000 + 0x4 * (n))
#define ACC_ENABLE_BIT      1u
#define APIDS               512
#define EE                  0

/* Register blocks of the /arm-io/spmi node, in its "reg" order. */
enum {
	kRegCore,
	kRegObserver,
	kRegChannels,
	kRegConfig,
	kRegIntr,
};

OSDefineMetaClassAndStructors(TulipSPMIInterrupts, IOService);

TulipSPMIInterrupts *
TulipSPMIInterrupts::wait(void)
{
	OSDictionary *matching = serviceMatching("TulipSPMIInterrupts");
	IOService *service = matching ? waitForMatchingService(matching, 5 * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(matching);
	TulipSPMIInterrupts *spmi = OSDynamicCast(TulipSPMIInterrupts, service);
	if (!spmi) {
		OSSafeReleaseNULL(service);
	}
	return spmi;
}

volatile uint32_t *
TulipSPMIInterrupts::intr(uint32_t offset)
{
	return (volatile uint32_t *)(fIntr->getVirtualAddress() + offset);
}

bool
TulipSPMIInterrupts::enable(uint8_t sid, uint16_t base, uint8_t bit, uint32_t trigger, IOInterruptEventSource *client)
{
	uint16_t apid;
	int owner = fSPMI.owner(sid, base);
	if (bit > 7 || !client || !fSPMI.apid(sid, base, &apid) || owner != EE) {
		IOLog("TulipSPMIInterrupts: no interrupt %u of %u:0x%04x (owner %d)\n", bit, sid, base, owner);
		return false;
	}
	uint8_t mask = (uint8_t)(1u << bit), type = 0, high = 0, low = 0;
	if (!fSPMI.read(sid, base + INT_SET_TYPE, &type, 1) || !fSPMI.read(sid, base + INT_POLARITY_HIGH, &high, 1) ||
	    !fSPMI.read(sid, base + INT_POLARITY_LOW, &low, 1)) {
		return false;
	}
	type |= mask;
	high = (trigger & kTulipPMICRising) ? (high | mask) : (high & ~mask);
	low = (trigger & kTulipPMICFalling) ? (low | mask) : (low & ~mask);
	uint8_t typeRegs[3] = { type, high, low };

	IOLockLock(fLock);
	bool ok = fClientCount < kMaxClients;
	if (ok) {
		fClients[fClientCount] = { apid, sid, base, bit, client };
		OSMemoryBarrier();
		fClientCount++;
	}
	IOLockUnlock(fLock);
	ok = ok && fSPMI.write(sid, base + INT_SET_TYPE, typeRegs, 3) && fSPMI.write(sid, base + INT_LATCHED_CLR, &mask, 1);
	if (ok) {
		*intr(ACC_ENABLE(apid)) = ACC_ENABLE_BIT;
		ok = fSPMI.write(sid, base + INT_EN_SET, &mask, 1);
	}
	IOLog("TulipSPMIInterrupts: %u:0x%04x bit %u (APID %u) %s\n", sid, base, bit, apid, ok ? "on" : "failed");
	return ok;
}

bool
TulipSPMIInterrupts::dispatch(uint16_t apid)
{
	uint32_t status = *intr(IRQ_STATUS(apid)) & 0xff;
	if (!status) {
		return false;
	}
	*intr(IRQ_CLEAR(apid)) = status;
	uint32_t claimed = 0;
	for (uint32_t i = 0; i < fClientCount; i++) {
		Client *c = &fClients[i];
		if (c->apid != apid || !(status & (1u << c->bit))) {
			continue;
		}
		uint8_t mask = (uint8_t)(1u << c->bit);
		fSPMI.write(c->sid, c->base + INT_LATCHED_CLR, &mask, 1);
		claimed |= mask;
		c->source->interruptOccurred(NULL, NULL, 0);
	}
	if (status & ~claimed) {
		uint8_t stray = (uint8_t)(status & ~claimed);
		bool known = false;
		for (uint32_t i = 0; i < fClientCount && !known; i++) {
			if (fClients[i].apid == apid) {
				fSPMI.write(fClients[i].sid, fClients[i].base + INT_EN_CLR, &stray, 1);
				fSPMI.write(fClients[i].sid, fClients[i].base + INT_LATCHED_CLR, &stray, 1);
				known = true;
			}
		}
		if (!known) {
			*intr(ACC_ENABLE(apid)) = 0;
		}
		kprintf("TulipSPMIInterrupts: APID %u stray bits 0x%x, turned off\n", apid, stray);
	}
	return true;
}

void
TulipSPMIInterrupts::summary(IOInterruptEventSource *sender __unused, int count __unused)
{
	bool any = false;
	for (uint32_t n = 0; n < APIDS / 32; n++) {
		uint32_t acc = *intr(OWNER_ACC_STATUS(n));
		while (acc) {
			uint32_t bit = __builtin_ctz(acc);
			acc &= acc - 1;
			uint16_t apid = (uint16_t)(n * 32 + bit);
			if (*intr(ACC_ENABLE(apid)) & ACC_ENABLE_BIT) {
				any |= dispatch(apid);
			}
		}
	}
	/* As in Linux: when the summary is empty, look at the APIDs that have clients. */
	if (!any) {
		for (uint32_t i = 0; i < fClientCount; i++) {
			dispatch(fClients[i].apid);
		}
	}
}

bool
TulipSPMIInterrupts::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fIntr = provider->mapDeviceMemoryWithIndex(kRegIntr);
	if (!fLock || !fIntr || !fSPMI.init(provider, kRegCore, kRegObserver) ||
	    !fSPMI.initWrites(provider, kRegChannels, kRegConfig)) {
		IOLog("TulipSPMIInterrupts: no arbiter blocks\n");
		return false;
	}

	/* Registering waits for the GIC driver, so only try once it runs. */
	OSDictionary *matching = serviceMatching("TulipGIC");
	IOService *gic = matching ? waitForMatchingService(matching, 5 * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(matching);
	fWorkLoop = IOWorkLoop::workLoop();
	if (gic && fWorkLoop && provider->getProperty(gIOInterruptControllersKey)) {
		fSummary = IOInterruptEventSource::interruptEventSource(this,
		    OSMemberFunctionCast(IOInterruptEventAction, this, &TulipSPMIInterrupts::summary), provider, 0);
	}
	OSSafeReleaseNULL(gic);
	if (!fSummary || fWorkLoop->addEventSource(fSummary) != kIOReturnSuccess) {
		IOLog("TulipSPMIInterrupts: no summary interrupt\n");
		return false;
	}
	fSummary->enable();
	IOLog("TulipSPMIInterrupts: arbiter 0x%08x\n", fSPMI.version());
	registerService();
	return true;
}
