/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_TULIPSPMI_H
#define _IOKIT_TULIPSPMI_H

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>

/* Reads PMIC registers through a Qualcomm SPMI arbiter v2/v3 observer channel (Linux drivers/spmi/spmi-pmic-arb.c). */
class TulipSPMI
{
public:
	/* Maps the arbiter's core and observer blocks from the provider's "reg" entries; false for other versions. */
	bool
	init(IOService *provider, unsigned int coreIndex, unsigned int observerIndex)
	{
		fCore = provider->mapDeviceMemoryWithIndex(coreIndex);
		fObserver = provider->mapDeviceMemoryWithIndex(observerIndex);
		if (!fCore || !fObserver) {
			return false;
		}
		fVersion = *(volatile uint32_t *)fCore->getVirtualAddress();
		return fVersion >= 0x20010000 && fVersion < 0x50000000;
	}

	uint32_t
	version() const
	{
		return fVersion;
	}

	bool
	read(uint8_t sid, uint16_t addr, uint8_t *value)
	{
		return read(sid, addr, value, 1);
	}

	/* Reads up to 4 consecutive registers in one SPMI transfer, so multi-byte values do not tear. */
	bool
	read(uint8_t sid, uint16_t addr, uint8_t *buf, unsigned int len)
	{
		uint16_t apid;
		if (len < 1 || len > 4 || !findApid((uint16_t)(sid << 8 | addr >> 8), &apid)) {
			return false;
		}
		volatile uint8_t *obs = (volatile uint8_t *)fObserver->getVirtualAddress() + 0x1000 * kEE + 0x8000 * apid;
		uint32_t status = 0;
		*(volatile uint32_t *)(obs + kCmd) = kOpExtReadL << 27 | (addr & 0xff) << 4 | (len - 1);
		for (int i = 0; i < 1000 && !(status & kStatusDone); i++) {
			IODelay(1);
			status = *(volatile uint32_t *)(obs + kStatus);
		}
		if (!(status & kStatusDone) || (status & kStatusErrors)) {
			kprintf("TulipSPMI: read sid %u 0x%04x: status 0x%x\n", sid, addr, status);
			return false;
		}
		uint32_t data = *(volatile uint32_t *)(obs + kReadData);
		for (unsigned int i = 0; i < len; i++) {
			buf[i] = (uint8_t)(data >> (8 * i));
		}
		return true;
	}

private:
	enum : uint32_t {
		kEE = 0,
		kApids = 512,
		kCmd = 0x00,
		kStatus = 0x08,
		kReadData = 0x18,
		kOpExtReadL = 1,
		kStatusDone = 1u << 0,
		kStatusErrors = 0xeu,
		kApidValid = 0x8000,
	};

	bool
	findApid(uint16_t ppid, uint16_t *apid)
	{
		if (!(fApid[ppid & 0xfff] & kApidValid)) {
			volatile uint32_t *core = (volatile uint32_t *)fCore->getVirtualAddress();
			for (uint16_t i = 0; i < kApids; i++) {
				uint32_t map = core[(0x800 + 4 * i) / 4];
				if (map && ((map >> 8) & 0xfff) == ppid) {
					fApid[ppid & 0xfff] = (uint16_t)(i | kApidValid);
					break;
				}
			}
		}
		*apid = fApid[ppid & 0xfff] & ~kApidValid;
		return fApid[ppid & 0xfff] & kApidValid;
	}

	IOMemoryMap *fCore = NULL;
	IOMemoryMap *fObserver = NULL;
	uint32_t fVersion = 0;
	uint16_t fApid[4096] = {};
};

#endif /* _IOKIT_TULIPSPMI_H */
