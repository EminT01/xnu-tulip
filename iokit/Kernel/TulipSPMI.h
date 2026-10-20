/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_TULIPSPMI_H
#define _IOKIT_TULIPSPMI_H

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>

/* Reads and writes PMIC registers through a Qualcomm SPMI arbiter v2/v3 (Linux drivers/spmi/spmi-pmic-arb.c). */
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

	/* Maps the write channels and the channel owner table; writes stay off without them. */
	bool
	initWrites(IOService *provider, unsigned int channelsIndex, unsigned int configIndex)
	{
		fChannels = provider->mapDeviceMemoryWithIndex(channelsIndex);
		fConfig = provider->mapDeviceMemoryWithIndex(configIndex);
		return fChannels && fConfig;
	}

	/* The execution environment that owns a peripheral's channel, or -1. Only the owner may write. */
	int
	owner(uint8_t sid, uint16_t addr)
	{
		uint16_t apid;
		if (!fConfig || !findApid((uint16_t)(sid << 8 | addr >> 8), &apid)) {
			return -1;
		}
		return (int)(*(volatile uint32_t *)(fConfig->getVirtualAddress() + kOwnerTable + 4 * apid) & kOwnerMask);
	}

	/* Writes up to 4 consecutive registers; refuses peripherals that another execution environment owns. */
	bool
	write(uint8_t sid, uint16_t addr, const uint8_t *buf, unsigned int len)
	{
		uint16_t apid;
		if (len < 1 || len > 4 || !fChannels || owner(sid, addr) != (int)kEE ||
		    !findApid((uint16_t)(sid << 8 | addr >> 8), &apid)) {
			kprintf("TulipSPMI: write sid %u 0x%04x refused\n", sid, addr);
			return false;
		}
		volatile uint8_t *chan = (volatile uint8_t *)fChannels->getVirtualAddress() + 0x1000 * kEE + 0x8000 * apid;
		uint32_t data = 0;
		for (unsigned int i = 0; i < len; i++) {
			data |= (uint32_t)buf[i] << (8 * i);
		}
		uint32_t status = 0;
		for (int attempt = 0; attempt < kAttempts && !(status & kStatusDone && !(status & kStatusErrors)); attempt++) {
			*(volatile uint32_t *)(chan + kWriteData) = data;
			*(volatile uint32_t *)(chan + kCmd) = kOpExtWriteL << 27 | (addr & 0xff) << 4 | (len - 1);
			status = wait(chan);
		}
		if (!(status & kStatusDone) || (status & kStatusErrors)) {
			kprintf("TulipSPMI: write sid %u 0x%04x: status 0x%x\n", sid, addr, status);
			return false;
		}
		return true;
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
		for (int attempt = 0; attempt < kAttempts && !(status & kStatusDone && !(status & kStatusErrors)); attempt++) {
			*(volatile uint32_t *)(obs + kCmd) = kOpExtReadL << 27 | (addr & 0xff) << 4 | (len - 1);
			status = wait(obs);
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
		kWriteData = 0x10,
		kReadData = 0x18,
		kOpExtWriteL = 0,
		kOpExtReadL = 1,
		kOwnerTable = 0x700,
		kOwnerMask = 0x7,
		kStatusDone = 1u << 0,
		kStatusErrors = 0xeu,
		kApidValid = 0x8000,
		kAttempts = 3,
	};

	/* The arbiter sometimes drops a transfer (status bit 3); Linux treats that as transient too. */
	static uint32_t
	wait(volatile uint8_t *chan)
	{
		uint32_t status = 0;
		for (int i = 0; i < 1000 && !(status & kStatusDone); i++) {
			IODelay(1);
			status = *(volatile uint32_t *)(chan + kStatus);
		}
		return status;
	}

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
	IOMemoryMap *fChannels = NULL;
	IOMemoryMap *fConfig = NULL;
	uint32_t fVersion = 0;
	uint16_t fApid[4096] = {};
};

#endif /* _IOKIT_TULIPSPMI_H */
