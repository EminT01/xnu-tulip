/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include "TulipSPMI.h"

#define CAPACITY_MAH            4000
#define CUTOFF_MV               3400
/* The gauge reads near 0 V for a moment when the charger is plugged in or out. */
#define MIN_VALID_MV            2500
#define WARN_PERCENT            10
#define POLL_MS                 2000
#define REPORT_POLLS            15

/* PM660 (SID 0) SMB2 charger and FG gen3 fuel gauge registers (Linux qpnp-smb2 and qpnp-fg-gen3). */
#define PM660_SID               0
#define CHGR_STATUS_1           0x1006
#define CHGR_STATUS_2           0x1007
#define CHGR_STATUS_5           0x100b
#define CHGR_STATUS_7           0x100d
#define BATIF_INT_RT_STS        0x1210
#define USBIN_APSD_RESULT       0x1308
#define USBIN_INT_RT_STS        0x1310
#define MISC_ICL_STATUS         0x1607
#define MISC_POWER_PATH_STATUS  0x160b
#define MISC_WDOG_STATUS        0x160c
#define MISC_WD_CFG             0x1651
#define MISC_SNARL_BARK_BITE    0x1653
#define FG_MSOC                 0x4009
#define FG_SOC_INT_RT_STS       0x4010
#define FG_SOC_EN_CTL           0x4046
#define FG_BATT_TEMP            0x4150
#define FG_INFO_INT_RT_STS      0x4110
#define FG_VBATT                0x41a0

#define CHGR_STATE_MASK         0x7
#define CHGR_STATE_TERMINATE    5
#define CHGR_STATE_INHIBIT      6
#define CHGR_ENABLED_MASK       0xf0
#define BATIF_MISSING           0x30
#define USBIN_PLUGIN            (1u << 4)
#define POWER_PATH_VALID        (1u << 0)
#define FG_MSOC_EMPTY           (1u << 5)
#define FG_ALGORITHM_EN         (1u << 7)
#define FG_BATT_MISSING         (1u << 3)

/* The PM660 battery: fuel gauge and charger state, read-only over SPMI. */
class TulipBattery : public IOPMPowerSource
{
	OSDeclareDefaultStructors(TulipBattery);

public:
	virtual bool init(OSDictionary *dictionary) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
	TulipSPMI fSPMI;
	IOTimerEventSource *fTimer;
	unsigned int fPolls;
	bool fReported;
	bool fLastOnline;
	uint8_t fLastState;
	uint32_t fLastPercent;

	void setNumber(const char *key, UInt32 value);
	void setFlag(const char *key, bool value);
	bool readU8(uint16_t addr, uint8_t *value);
	bool readU16(uint16_t addr, uint16_t *value);
	bool readPercent(uint32_t *percent);
	void logState();
	void update();

	static void poll(OSObject *owner, IOTimerEventSource *sender);
};

OSDefineMetaClassAndStructors(TulipBattery, IOPMPowerSource);

/* IOKit creates matched drivers with init(OSDictionary *), which skips IOPMPowerSource's own init(). */
bool
TulipBattery::init(OSDictionary *dictionary)
{
	if (!IOPMPowerSource::init()) {
		return false;
	}
	if (dictionary) {
		dictionary->iterateObjects(^bool (const OSSymbol *key, OSObject *value) {
			setProperty(key, value);
			return false;
		});
	}
	return true;
}

void
TulipBattery::setNumber(const char *key, UInt32 value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	OSNumber *number = OSNumber::withNumber(value, 32);
	if (symbol && number) {
		setPSProperty(symbol, number);
	}
	OSSafeReleaseNULL(number);
	OSSafeReleaseNULL(symbol);
}

void
TulipBattery::setFlag(const char *key, bool value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	if (symbol) {
		setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
		symbol->release();
	}
}

bool
TulipBattery::readU8(uint16_t addr, uint8_t *value)
{
	return fSPMI.read(PM660_SID, addr, value);
}

bool
TulipBattery::readU16(uint16_t addr, uint16_t *value)
{
	uint8_t b[2];
	if (!fSPMI.read(PM660_SID, addr, b, 2)) {
		return false;
	}
	*value = (uint16_t)(b[1] << 8 | b[0]);
	return true;
}

/* The monotonic SOC has a shadow copy; the two differ while the gauge updates them. */
bool
TulipBattery::readPercent(uint32_t *percent)
{
	for (int i = 0; i < 5; i++) {
		uint8_t b[2];
		if (fSPMI.read(PM660_SID, FG_MSOC, b, 2) && b[0] == b[1]) {
			uint32_t raw = b[0];
			*percent = raw == 255 ? 100 : raw == 0 ? 0 : ((raw - 1) * 98 + 126) / 253 + 1;
			return true;
		}
	}
	return false;
}

void
TulipBattery::logState()
{
	static const uint16_t regs[] = {
		CHGR_STATUS_1, CHGR_STATUS_2, CHGR_STATUS_5, CHGR_STATUS_7, BATIF_INT_RT_STS, USBIN_APSD_RESULT,
		USBIN_INT_RT_STS, MISC_ICL_STATUS, MISC_POWER_PATH_STATUS, MISC_WDOG_STATUS, MISC_WD_CFG,
		MISC_SNARL_BARK_BITE, FG_MSOC, FG_SOC_INT_RT_STS, FG_SOC_EN_CTL, FG_INFO_INT_RT_STS,
	};
	char line[256];
	size_t len = (size_t)snprintf(line, sizeof(line), "TulipBattery:");
	for (unsigned int i = 0; i < sizeof(regs) / sizeof(regs[0]) && len < sizeof(line); i++) {
		uint8_t v = 0;
		bool ok = readU8(regs[i], &v);
		len += (size_t)snprintf(line + len, sizeof(line) - len, ok ? " %x=%02x" : " %x=--", regs[i], v);
	}
	IOLog("%s\n", line);
}

void
TulipBattery::update()
{
	uint8_t usbin = 0, path = 0, chgr = 0, chgr7 = 0, socSts = 0, infoSts = 0, batif = 0;
	uint16_t vraw = 0, iraw = 0, traw = 0;
	uint32_t percent = 0;
	bool ok = readU8(USBIN_INT_RT_STS, &usbin) && readU8(MISC_POWER_PATH_STATUS, &path) &&
	    readU8(CHGR_STATUS_1, &chgr) && readU8(CHGR_STATUS_7, &chgr7) && readU8(FG_SOC_INT_RT_STS, &socSts) &&
	    readU8(FG_INFO_INT_RT_STS, &infoSts) && readU8(BATIF_INT_RT_STS, &batif) && readU16(FG_VBATT, &vraw) &&
	    readU16(FG_VBATT + 2, &iraw) && readU16(FG_BATT_TEMP, &traw) && readPercent(&percent);
	if (!ok) {
		return;
	}

	bool online = (usbin & USBIN_PLUGIN) || (path & POWER_PATH_VALID);
	uint8_t state = chgr & CHGR_STATE_MASK;
	bool done = state == CHGR_STATE_TERMINATE || state == CHGR_STATE_INHIBIT;
	bool charging = online && !done && state < 7 && (chgr7 & CHGR_ENABLED_MASK);
	bool present = !(batif & BATIF_MISSING) && !(infoSts & FG_BATT_MISSING);
	int32_t mv = (int32_t)((uint64_t)vraw * 122070 / 1000000);
	int32_t ma = (int32_t)(-(int64_t)(int16_t)iraw * 488281 / 1000000);
	int32_t centiC = (int32_t)(traw & 0x7ff) * 25 - 27315;
	if (mv < MIN_VALID_MV) {
		return;
	}
	bool empty = (socSts & FG_MSOC_EMPTY) && mv < CUTOFF_MV;
	if (empty) {
		percent = 0;
	}

	bool changed = !fReported || online != fLastOnline || state != fLastState || percent != fLastPercent;
	if (!changed && ++fPolls < REPORT_POLLS) {
		return;
	}
	fPolls = 0;
	if (changed) {
		IOLog("TulipBattery: %u%%, %d mV, %d mA, %d.%02d C, %s, charger state %u\n", percent, mv, ma,
		    centiC / 100, (centiC < 0 ? -centiC : centiC) % 100, online ? "online" : "offline", state);
	}
	if (online != fLastOnline || !fReported) {
		logState();
	}
	fReported = true;
	fLastOnline = online;
	fLastState = state;
	fLastPercent = percent;

	uint32_t mah = percent * CAPACITY_MAH / 100;
	setBatteryInstalled(present);
	setCurrentCapacity(mah);
	setExternalConnected(online);
	setIsCharging(charging);
	setAtWarnLevel(percent <= WARN_PERCENT);
	setAtCriticalLevel(empty);
	setVoltage((unsigned int)mv);
	setAmperage(ma);
	if (ma < 0) {
		setTimeRemaining((int)(mah * 60 / (uint32_t)-ma));
	} else if (charging && ma > 0) {
		setTimeRemaining((int)((CAPACITY_MAH - mah) * 60 / (uint32_t)ma));
	} else {
		setTimeRemaining(-1);
	}
	setNumber("AppleRawCurrentCapacity", mah);
	setNumber("InstantAmperage", (UInt32)ma);
	setNumber("Temperature", (UInt32)centiC);
	setFlag("FullyCharged", online && done);
	setFlag("AppleRawExternalConnected", online);
	updateStatus();
}

void
TulipBattery::poll(OSObject *owner, IOTimerEventSource *sender)
{
	((TulipBattery *)owner)->update();
	sender->setTimeoutMS(POLL_MS);
}

bool
TulipBattery::start(IOService *provider)
{
	if (!fSPMI.init(provider, 0, 1)) {
		IOLog("TulipBattery: no SPMI arbiter v2 or v3 (version 0x%08x)\n", fSPMI.version());
		return false;
	}
	if (!IOPMPowerSource::start(provider)) {
		return false;
	}
	uint8_t en = 0;
	if (readU8(FG_SOC_EN_CTL, &en) && !(en & FG_ALGORITHM_EN)) {
		IOLog("TulipBattery: fuel gauge algorithm is off, SOC may be stale\n");
	}
	setMaxCapacity(CAPACITY_MAH);
	setExternalChargeCapable(true);
	setCycleCount(0);
	setNumber("DesignCapacity", CAPACITY_MAH);
	setNumber("AppleRawMaxCapacity", CAPACITY_MAH);
	update();

	IOWorkLoop *wl = getWorkLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &TulipBattery::poll);
	if (!wl || !fTimer || wl->addEventSource(fTimer) != kIOReturnSuccess) {
		return false;
	}
	registerService();
	fTimer->setTimeoutMS(POLL_MS);
	return true;
}
