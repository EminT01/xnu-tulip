/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include "TulipSPMI.h"

/* PM660L (SID 3) WLED, Linux drivers/leds/leds-qpnp-wled.c. Strings 0 and 1 drive the panel. */
#define WLED_SID                3
#define WLED_CTRL               0xd800
#define WLED_SINK               0xd900
#define WLED_MODULE_EN          0xd846
#define WLED_MODULE_EN_BIT      (1u << 7)
#define WLED_SYNC               0xd947
#define WLED_SYNC_ALL           0x0f
#define WLED_BRIGHT(n)          (0xd957 + 0x10 * (n))
#define WLED_STRINGS            2
#define WLED_MAX_LEVEL          4095
#define WLED_MIN_LEVEL          4
#define WLED_SYNC_DELAY_US      1000
#define WLED_SOFT_START_MS      10
#define LCDB_BASE               0xec00

#define DEFAULT_MAX_NITS        750
#define DEFAULT_MID_NITS        155
#define DEFAULT_MIN_NITS        2
#define CALIBRATION_MA          20

/* Display power from IOMobileFramebuffer selector 12; TulipTouch drops touches while it is off. */
extern "C" bool tulip_display_on;
bool tulip_display_on = true;

class AppleARMBacklight;
static AppleARMBacklight *gBacklight;

/* The panel backlight: CoreBrightness sets brightness-nits, and the display power switches the WLED module. */
class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;

	void setPower(bool on);

private:
	TulipSPMI fSPMI;
	IOService *fArbiter;
	IOLock *fLock;
	uint32_t fLevel;
	uint64_t fMaxNits;
	bool fWrites;

	bool readLevel(uint32_t *level);
	bool writeLevel(uint32_t level);
	bool setModule(bool on);
	void logState();
	void publishLevel();
	void publishCalibration(IOService *provider);
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

void
tulip_display_power(bool on)
{
	tulip_display_on = on;
	if (gBacklight) {
		gBacklight->setPower(on);
	}
}

bool
AppleARMBacklight::readLevel(uint32_t *level)
{
	uint8_t b[2];
	if (!fSPMI.read(WLED_SID, WLED_BRIGHT(0), b, 2)) {
		return false;
	}
	*level = (uint32_t)(b[1] & 0x0f) << 8 | b[0];
	return true;
}

bool
AppleARMBacklight::writeLevel(uint32_t level)
{
	const uint8_t value[2] = { (uint8_t)level, (uint8_t)(level >> 8 & 0x0f) };
	const uint8_t sync = WLED_SYNC_ALL, idle = 0;
	for (int n = 0; n < WLED_STRINGS; n++) {
		if (!fSPMI.write(WLED_SID, WLED_BRIGHT(n), value, 2)) {
			return false;
		}
	}
	if (!fSPMI.write(WLED_SID, WLED_SYNC, &sync, 1)) {
		return false;
	}
	IODelay(WLED_SYNC_DELAY_US);
	return fSPMI.write(WLED_SID, WLED_SYNC, &idle, 1);
}

bool
AppleARMBacklight::setModule(bool on)
{
	uint8_t en;
	if (!fSPMI.read(WLED_SID, WLED_MODULE_EN, &en, 1)) {
		return false;
	}
	en = on ? (en | WLED_MODULE_EN_BIT) : (en & ~WLED_MODULE_EN_BIT);
	return fSPMI.write(WLED_SID, WLED_MODULE_EN, &en, 1);
}

void
AppleARMBacklight::setPower(bool on)
{
	IOLockLock(fLock);
	bool ok = fWrites && (on ? writeLevel(fLevel) && setModule(true) : setModule(false));
	IOLockUnlock(fLock);
	if (on && ok) {
		IOSleep(WLED_SOFT_START_MS);
	}
	IOLog("AppleARMBacklight: display %s%s\n", on ? "on" : "off", ok ? "" : ", WLED write failed");
}

void
AppleARMBacklight::publishLevel()
{
	OSDictionary *params = OSDictionary::withCapacity(1);
	OSDictionary *brightness = OSDictionary::withCapacity(3);
	OSNumber *min = OSNumber::withNumber(0ULL, 32);
	OSNumber *max = OSNumber::withNumber(WLED_MAX_LEVEL, 32);
	OSNumber *value = OSNumber::withNumber(fLevel, 32);
	if (params && brightness && min && max && value) {
		brightness->setObject("min", min);
		brightness->setObject("max", max);
		brightness->setObject("value", value);
		params->setObject("brightness", brightness);
		setProperty("IODisplayParameters", params);
	}
	OSSafeReleaseNULL(value);
	OSSafeReleaseNULL(max);
	OSSafeReleaseNULL(min);
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(params);
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	if (!dict) {
		return kIOReturnBadArgument;
	}
	dict->iterateObjects(^bool (const OSSymbol *key, OSObject *value) {
		OSNumber *number = OSDynamicCast(OSNumber, value);
		if (key->isEqualTo("brightness-nits") && number) {
			uint64_t level = number->unsigned64BitValue() * WLED_MAX_LEVEL / fMaxNits;
			level = level < WLED_MIN_LEVEL ? WLED_MIN_LEVEL : level > WLED_MAX_LEVEL ? WLED_MAX_LEVEL : level;
			IOLockLock(fLock);
			fLevel = (uint32_t)level;
			bool ok = fWrites && (!tulip_display_on || writeLevel(fLevel));
			IOLockUnlock(fLock);
			publishLevel();
			IOLog("AppleARMBacklight: brightness-nits 0x%llx -> level %u%s\n", number->unsigned64BitValue(), fLevel,
			    ok ? "" : ", WLED write failed");
		} else {
			IOLog("AppleARMBacklight: ignoring %s = %lld\n", key->getCStringNoCopy(),
			    number ? (long long)number->unsigned64BitValue() : -1LL);
		}
		return false;
	});
	return kIOReturnSuccess;
}

void
AppleARMBacklight::logState()
{
	static const uint16_t wled[] = {
		0xd804, 0xd805, 0xd808, 0xd810, 0xd845, 0xd846, 0xd848, 0xd84c, 0xd84d, 0xd84e, 0xd904, 0xd905, 0xd946,
		0xd94a, 0xd94b, 0xd950, 0xd952, 0xd956, 0xd957, 0xd958, 0xd960, 0xd962, 0xd966, 0xd967, 0xd968,
	};
	static const uint16_t lcdb[] = { 0xec04, 0xec05, 0xec08, 0xec0a, 0xec0b, 0xec10, 0xec41, 0xec46, 0xec71, 0xec81 };
	static const struct {
		const char *name;
		const uint16_t *regs;
		unsigned int count;
	} groups[] = {
		{ "wled", wled, sizeof(wled) / sizeof(wled[0]) },
		{ "lcdb", lcdb, sizeof(lcdb) / sizeof(lcdb[0]) },
	};

	IOLog("AppleARMBacklight: owners wled ctrl %d, wled sink %d, lcdb %d\n", fSPMI.owner(WLED_SID, WLED_CTRL),
	    fSPMI.owner(WLED_SID, WLED_SINK), fSPMI.owner(WLED_SID, LCDB_BASE));
	for (unsigned int g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
		char line[256];
		size_t len = (size_t)snprintf(line, sizeof(line), "AppleARMBacklight: %s", groups[g].name);
		for (unsigned int i = 0; i < groups[g].count && len < sizeof(line); i++) {
			uint8_t v = 0;
			bool ok = fSPMI.read(WLED_SID, groups[g].regs[i], &v);
			len += (size_t)snprintf(line + len, sizeof(line) - len, ok ? " %x=%02x" : " %x=--", groups[g].regs[i], v);
		}
		IOLog("%s\n", line);
	}
}

static uint32_t
providerU32(IOService *provider, const char *key, uint32_t fallback)
{
	OSData *data = OSDynamicCast(OSData, provider->getProperty(key));
	if (!data || data->getLength() < sizeof(uint32_t) || !*(const uint32_t *)data->getBytesNoCopy()) {
		return fallback;
	}
	return *(const uint32_t *)data->getBytesNoCopy();
}

/* CoreBrightness reads these from the service itself, not from its device tree provider. */
void
AppleARMBacklight::publishCalibration(IOService *provider)
{
	OSObject *table = provider->getProperty("backlight-marketing-table");
	if (table) {
		setProperty("backlight-marketing-table", table);
	}

	const uint32_t max = (uint32_t)fMaxNits;
	const uint32_t mid = providerU32(provider, "LmidProduct", DEFAULT_MID_NITS << 16);
	const uint32_t min = providerU32(provider, "LminProduct", DEFAULT_MIN_NITS << 16);
	const struct {
		const char *key;
		uint32_t value;
	} params[] = {
		{ "lMaxProduct", max }, { "lMidProduct", mid }, { "lMinProduct", min },
		{ "lMaxPanel", max }, { "lMidPanel", mid }, { "lMinPanel", min },
		{ "current-for-max-backlight", CALIBRATION_MA << 16 },
		{ "current-for-mid-backlight", CALIBRATION_MA << 16 },
		{ "hardware-max-current-limit", CALIBRATION_MA << 16 },
	};
	OSDictionary *dict = OSDictionary::withCapacity(sizeof(params) / sizeof(params[0]));
	if (!dict) {
		return;
	}
	for (unsigned int i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		OSNumber *number = OSNumber::withNumber(params[i].value, 32);
		if (number) {
			dict->setObject(params[i].key, number);
			number->release();
		}
	}
	setProperty("backlight-calibration-parameters", dict);
	dict->release();
}

bool
AppleARMBacklight::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	OSDictionary *match = IOService::nameMatching("qcom,spmi-pmic-arb");
	fArbiter = match ? IOService::waitForMatchingService(match, 5 * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(match);
	if (!fLock || !fArbiter || !fSPMI.init(fArbiter, 0, 1)) {
		IOLog("AppleARMBacklight: no SPMI arbiter\n");
		return false;
	}

	fMaxNits = providerU32(provider, "LmaxProduct", DEFAULT_MAX_NITS << 16);

	fWrites = fSPMI.initWrites(fArbiter, 2, 3) && fSPMI.owner(WLED_SID, WLED_CTRL) == 0 &&
	    fSPMI.owner(WLED_SID, WLED_SINK) == 0;
	logState();
	if (!readLevel(&fLevel)) {
		fLevel = WLED_MAX_LEVEL;
	}
	IOLog("AppleARMBacklight: level %u, max %llu nits, writes %s\n", fLevel, fMaxNits >> 16, fWrites ? "on" : "off");

	setProperty("backlight-control", kOSBooleanTrue);
	setProperty("new-backlight-architecture", kOSBooleanTrue);
	publishLevel();
	publishCalibration(provider);
	gBacklight = this;
	registerService();
	return true;
}
