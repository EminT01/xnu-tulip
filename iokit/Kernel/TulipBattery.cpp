/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>

#define CAPACITY_MAH 4000

/* A battery that is always full and on external power, until we read the PM660 fuel gauge. */
class TulipBattery : public IOPMPowerSource
{
	OSDeclareDefaultStructors(TulipBattery);

public:
	virtual bool init(OSDictionary *dictionary) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
	void setNumber(const char *key, UInt32 value);
	void setFlag(const char *key, bool value);
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
TulipBattery::start(IOService *provider)
{
	if (!IOPMPowerSource::start(provider)) {
		return false;
	}
	setBatteryInstalled(true);
	setMaxCapacity(CAPACITY_MAH);
	setCurrentCapacity(CAPACITY_MAH);
	setExternalConnected(true);
	setExternalChargeCapable(true);
	setIsCharging(false);
	setAtWarnLevel(false);
	setAtCriticalLevel(false);
	setVoltage(4350);
	setAmperage(0);
	setTimeRemaining(-1);
	setCycleCount(0);
	setNumber("DesignCapacity", CAPACITY_MAH);
	setNumber("AppleRawCurrentCapacity", CAPACITY_MAH);
	setNumber("AppleRawMaxCapacity", CAPACITY_MAH);
	setNumber("Temperature", 2500);
	setFlag("FullyCharged", true);
	setFlag("AppleRawExternalConnected", true);
	updateStatus();
	registerService();
	return true;
}
