/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include "TulipSPMI.h"

extern "C" {
#include <pexpert/pexpert.h>
}

extern uint64_t tulip_cpu_mhz(void);

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
#define GOLD_RAIL               0x1a00
#define SMPS_VALID_MV           0x42

#define FUSE_SPEED_BIN          0x130
#define OSM_PERF_ENABLE         0x3004
#define CPRH_LOOP_ENABLE        0x4004
#define CPRH_CTL                0x7aa0
#define APM_PERF_STATUS         0x18
#define APM_STATUS_MASK         0x1f

#define PLL_MODE                0x0
#define PLL_L                   0x4
#define PLL_STATUS              0x2c
#define PLL_MODE_OUTCTRL        (1u << 0)
#define PLL_MODE_BYPASSNL       (1u << 1)
#define PLL_MODE_RESET_N        (1u << 2)
#define PLL_STATUS_LOCK         (1u << 16)

#define MUX_CMD                 0x50
#define MUX_CFG                 0x54
#define MUX_CMD_UPDATE          (1u << 0)
#define MUX_CMD_ROOT_EN         (1u << 1)
#define MUX_CFG_SRC_MASK        0x71fu
#define MUX_SRC_GPLL0           0x400u
#define MUX_SRC_PLL_MAIN        0x300u

/* Highest PLL L value (x 19.2 MHz) for each Gold rail voltage, from the bin 3 perfcl table's ceilings. */
static const struct {
	uint32_t l;
	uint32_t mv;
} gGoldSteps[] = {
	{ 0x3a, 724 },  /* 1113.6 MHz */
	{ 0x49, 788 },  /* 1401.6 MHz */
	{ 0x5b, 868 },  /* 1747.2 MHz */
};

/* Gold cluster clock: logs the state that the bootloader leaves and runs the Gold PLL at tulip_gold_l (default 0x5b). */
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

	void
	setReg(int block, uint32_t offset, uint32_t value)
	{
		*(volatile uint32_t *)(fMap[block]->getVirtualAddress() + offset) = value;
	}

	bool setMux(uint32_t source);
	bool setGoldL(uint32_t l);

	void logBlock(const char *name, int block, const uint32_t *offsets, unsigned int count);
};

OSDefineMetaClassAndStructors(TulipCPUClock, IOService);

bool
TulipCPUClock::setMux(uint32_t source)
{
	setReg(kRegPerfMux, MUX_CFG, (reg(kRegPerfMux, MUX_CFG) & ~MUX_CFG_SRC_MASK) | source);
	setReg(kRegPerfMux, MUX_CMD, MUX_CMD_UPDATE | MUX_CMD_ROOT_EN);
	for (int i = 0; i < 100; i++) {
		if (!(reg(kRegPerfMux, MUX_CMD) & MUX_CMD_UPDATE)) {
			return true;
		}
		IODelay(1);
	}
	return false;
}

/* Runs the Gold PLL at l x 19.2 MHz and the Gold cluster from it. OSM and CPRh stay off; the rail must already hold the voltage. */
bool
TulipCPUClock::setGoldL(uint32_t l)
{
	uint32_t needMv = 0;
	for (unsigned int i = 0; i < sizeof(gGoldSteps) / sizeof(gGoldSteps[0]) && !needMv; i++) {
		if (l <= gGoldSteps[i].l) {
			needMv = gGoldSteps[i].mv;
		}
	}
	uint8_t lo = 0, hi = 0;
	bool rail = fSPMI.read(1, GOLD_RAIL + SMPS_VALID_MV, &lo) && fSPMI.read(1, GOLD_RAIL + SMPS_VALID_MV + 1, &hi);
	uint32_t railMv = (uint32_t)hi << 8 | lo;
	if (!needMv || !rail || railMv < needMv || (reg(kRegFuses, FUSE_SPEED_BIN) >> 29 & 7) != 3 ||
	    (reg(kRegOSM, OSM_PERF_ENABLE) & 1) || (reg(kRegOSM, CPRH_LOOP_ENABLE) & 1) || (reg(kRegOSM, CPRH_CTL) & 1) ||
	    (reg(kRegAPM, APM_PERF_STATUS) & APM_STATUS_MASK)) {
		IOLog("TulipCPUClock: not setting L 0x%x: needs %u mV, rail %u mV\n", l, needMv, railMv);
		return false;
	}

	boolean_t istate = ml_set_interrupts_enabled(FALSE);
	bool ok = setMux(MUX_SRC_GPLL0);
	if (ok) {
		setReg(kRegPerfPLL, PLL_MODE, 0);
		setReg(kRegPerfPLL, PLL_L, l);
		setReg(kRegPerfPLL, PLL_MODE, PLL_MODE_BYPASSNL);
		IODelay(10);
		setReg(kRegPerfPLL, PLL_MODE, PLL_MODE_BYPASSNL | PLL_MODE_RESET_N);
		IODelay(50);
		ok = false;
		for (int i = 0; i < 200 && !ok; i++) {
			ok = reg(kRegPerfPLL, PLL_STATUS) & PLL_STATUS_LOCK;
			IODelay(1);
		}
		if (ok) {
			setReg(kRegPerfPLL, PLL_MODE, PLL_MODE_BYPASSNL | PLL_MODE_RESET_N | PLL_MODE_OUTCTRL);
			ok = setMux(MUX_SRC_PLL_MAIN);
		}
	}
	ml_set_interrupts_enabled(istate);
	IOLog("TulipCPUClock: L 0x%x (%u kHz) at %u mV: %s, mux 0x%08x\n", l, l * 19200, railMv, ok ? "ok" : "failed",
	    reg(kRegPerfMux, MUX_CFG));
	return ok;
}

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

	/* 1747.2 MHz unless tulip_gold_l says otherwise (0: keep ABL's 600 MHz). */
	uint32_t l = 0x5b;
	PE_parse_boot_argn("tulip_gold_l", &l, sizeof(l));
	if (l) {
		IOLog("TulipCPUClock: %llu MHz before\n", tulip_cpu_mhz());
		setGoldL(l);
		IOLog("TulipCPUClock: %llu MHz after\n", tulip_cpu_mhz());
	}
	return true;
}
