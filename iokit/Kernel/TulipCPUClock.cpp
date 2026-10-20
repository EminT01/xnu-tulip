/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include "TulipSPMI.h"

/* Register blocks of the /arm-io/cpu-clock node, in its "reg" order. */
enum {
	kRegFuses,
	kRegOSM,
	kRegPerfPLL,
	kRegPerfMux,
	kRegGCC,
	kRegAPM,
	kRegPerfSAW,
	kRegSPMICore,
	kRegSPMIObserver,
	kRegs,
};

/* APC1 (Gold rail) candidates: PM660 (SID 1) S1, S2, S3. */
static const uint16_t gSMPS[] = { 0x1400, 0x1700, 0x1a00 };

/* Gold cluster clock and voltage. Read-only for now: it logs the state that the bootloader leaves. */
class TulipCPUClock : public IOService
{
	OSDeclareDefaultStructors(TulipCPUClock);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
	IOMemoryMap *fMap[kRegs];
	TulipSPMI fSPMI;

	uint32_t
	reg(int block, uint32_t offset)
	{
		return *(volatile uint32_t *)(fMap[block]->getVirtualAddress() + offset);
	}

	void logBlock(const char *name, int block, const uint32_t *offsets, unsigned int count);
};

OSDefineMetaClassAndStructors(TulipCPUClock, IOService);

void
TulipCPUClock::logBlock(const char *name, int block, const uint32_t *offsets, unsigned int count)
{
	char line[256];
	size_t len = (size_t)snprintf(line, sizeof(line), "TulipCPUClock: %s", name);

	for (unsigned int i = 0; i < count && len < sizeof(line); i++) {
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %x=%08x", offsets[i], reg(block, offsets[i]));
	}
	IOLog("%s\n", line);
}

bool
TulipCPUClock::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	for (int i = 0; i < kRegSPMICore; i++) {
		fMap[i] = provider->mapDeviceMemoryWithIndex(i);
		if (!fMap[i]) {
			IOLog("TulipCPUClock: no register block %d\n", i);
			return false;
		}
	}
	if (!fSPMI.init(provider, kRegSPMICore, kRegSPMIObserver)) {
		IOLog("TulipCPUClock: no SPMI arbiter\n");
		return false;
	}

	uint32_t bin = reg(kRegFuses, 0x130) >> 29 & 7;
	IOLog("TulipCPUClock: speed bin %u\n", bin);
	static const uint32_t fuses[] = { 0x130, 0x228, 0x22c, 0x230, 0x238 };
	static const uint32_t osm[] = { 0x3004, 0x3f10, 0x3c00, 0x3150, 0x3154, 0x3158, 0x315c, 0x3160, 0x3164,
		                        0x4004, 0x7aa0, 0x7aa4 };
	static const uint32_t pll[] = { 0x0, 0x4, 0xc, 0x10, 0x2c };
	static const uint32_t mux[] = { 0x50, 0x54 };
	static const uint32_t gcc[] = { 0x5c, 0x60 };
	static const uint32_t apm[] = { 0x10, 0x18 };
	static const uint32_t saw[] = { 0xc04, 0xc18, 0x900, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54 };
	logBlock("fuses", kRegFuses, fuses, sizeof(fuses) / sizeof(fuses[0]));
	logBlock("osm", kRegOSM, osm, sizeof(osm) / sizeof(osm[0]));
	logBlock("pll", kRegPerfPLL, pll, sizeof(pll) / sizeof(pll[0]));
	logBlock("mux", kRegPerfMux, mux, sizeof(mux) / sizeof(mux[0]));
	logBlock("hmss_gpll0", kRegGCC, gcc, sizeof(gcc) / sizeof(gcc[0]));
	logBlock("apm", kRegAPM, apm, sizeof(apm) / sizeof(apm[0]));
	logBlock("saw", kRegPerfSAW, saw, sizeof(saw) / sizeof(saw[0]));

	static const uint8_t smpsRegs[] = { 0x04, 0x05, 0x40, 0x41, 0x42, 0x43, 0x45, 0x68, 0x69 };
	for (unsigned int s = 0; s < sizeof(gSMPS) / sizeof(gSMPS[0]); s++) {
		char line[128];
		size_t len = (size_t)snprintf(line, sizeof(line), "TulipCPUClock: pm660 s%u", s + 1);
		for (unsigned int i = 0; i < sizeof(smpsRegs) && len < sizeof(line); i++) {
			uint8_t v = 0;
			bool ok = fSPMI.read(1, (uint16_t)(gSMPS[s] + smpsRegs[i]), &v);
			len += (size_t)snprintf(line + len, sizeof(line) - len, ok ? " %02x=%02x" : " %02x=--", smpsRegs[i], v);
		}
		IOLog("%s\n", line);
	}
	return true;
}
