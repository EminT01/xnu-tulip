/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/hid/IOHIDEvent.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include "hid/include/IOHIDEventService.h"
#include "TulipI2C.h"
#include "TulipTLMM.h"

extern "C" {
#include <kern/clock.h>
}

/* Novatek NT36672A at I2C address 0x01 (Linux drivers/input/touchscreen/novatek-nvt-ts.c). */
#define NVT_ADDR                0x01
#define NVT_TOUCH_START         0x00
#define NVT_TOUCH_SIZE          6
#define NVT_MAX_TOUCHES         10
#define NVT_PARAMS_START        0x78
#define NVT_PARAMS_SIZE         0x0f
#define NVT_TOUCH_INVALID       0xff
#define NVT_TOUCH_NEW           1
#define NVT_TOUCH_UPDATE        2

#define POLL_MS                 8
/* A finger that sees no interrupt for this long is read again, in case its release was lost. */
#define RELEASE_CHECK_MS        100

class TulipTouch : public IOHIDEventService
{
	OSDeclareDefaultStructors(TulipTouch);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Digitizer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Dig_TouchScreen; }

protected:
	virtual const char *getProduct() APPLE_KEXT_OVERRIDE { return "Touch Screen"; }

private:
	TulipI2C *fI2C;
	TulipTLMM *fTLMM;
	uint32_t fAddr, fResetPin, fIrqPin;
	IOTimerEventSource *fTimer;
	IOInterruptEventSource *fInterrupt;
	uint64_t fInterrupts;
	uint32_t fWidth, fHeight;
	uint32_t fCanvasWidth, fCanvasHeight;
	int32_t fOutX, fOutY, fOutWidth, fOutHeight;
	bool fDown[NVT_MAX_TOUCHES];
	IOFixed fX[NVT_MAX_TOUCHES], fY[NVT_MAX_TOUCHES];
	UInt32 fSwipe[NVT_MAX_TOUCHES];

	bool anyDown() const;
	bool readTouches();
	void arm();
	void report(const uint8_t *buf);
	static void poll(OSObject *owner, IOTimerEventSource *sender);
	static void interrupt(OSObject *owner, IOInterruptEventSource *sender, int count);
};

extern "C" bool tulip_display_on;

OSDefineMetaClassAndStructors(TulipTouch, IOHIDEventService);

/*
 * Apple's touch firmware marks a touch that starts at the left, right or bottom edge, then marks its moves as a
 * locked swipe away from that edge. UIKit's system edge gestures (the home gesture) need both marks.
 */
static UInt32
edgeSwipe(IOFixed x, IOFixed y)
{
	const IOFixed edge = 0x10000 * 3 / 100;
	if (y > 0x10000 - edge) {
		return kIOHIDDigitizerEventSwipeUp;
	}
	if (x < edge) {
		return kIOHIDDigitizerEventSwipeRight;
	}
	if (x > 0x10000 - edge) {
		return kIOHIDDigitizerEventSwipeLeft;
	}
	return 0;
}

/* tulip's touch panel reports at most y = 2248 of 2280 rows; stretch the last rows so a swipe from the bottom edge starts at the edge. */
#define TOUCH_STRETCH_FROM 2200
#define TOUCH_MAX_Y        2248

static int32_t
bottomStretch(int32_t y, int32_t height)
{
	if (y <= TOUCH_STRETCH_FROM || height <= TOUCH_MAX_Y) {
		return y;
	}
	int32_t stretched = TOUCH_STRETCH_FROM + (y - TOUCH_STRETCH_FROM) * (height - 1 - TOUCH_STRETCH_FROM) /
	    (TOUCH_MAX_Y - TOUCH_STRETCH_FROM);
	return stretched < height ? stretched : height - 1;
}

/* pos / size as a 16.16 fraction, clamped to [0, 1). */
static IOFixed
unitFixed(int32_t pos, int32_t size)
{
	int64_t v = ((int64_t)pos << 16) / size;
	return (IOFixed)(v < 0 ? 0 : (v > 0xffff ? 0xffff : v));
}

static bool
propertyU32(IOService *service, const char *key, uint32_t *value)
{
	OSData *data = OSDynamicCast(OSData, service->getProperty(key));
	if (!data || data->getLength() != sizeof(uint32_t)) {
		return false;
	}
	*value = *(const uint32_t *)data->getBytesNoCopy();
	return true;
}

bool
TulipTouch::anyDown() const
{
	for (int i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (fDown[i]) {
			return true;
		}
	}
	return false;
}

bool
TulipTouch::readTouches()
{
	uint8_t buf[NVT_MAX_TOUCHES * NVT_TOUCH_SIZE];
	if (!fI2C->readRegister((uint8_t)fAddr, NVT_TOUCH_START, buf, sizeof(buf))) {
		return false;
	}
	report(buf);
	return true;
}

void
TulipTouch::arm()
{
	if (!fInterrupt || !fTLMM->interruptsReady()) {
		fTimer->setTimeoutMS(POLL_MS);
	} else if (anyDown()) {
		fTimer->setTimeoutMS(RELEASE_CHECK_MS);
	} else {
		fTimer->cancelTimeout();
	}
}

bool
TulipTouch::start(IOService *provider)
{
	if (!propertyU32(provider, "i2c-address", &fAddr) || !propertyU32(provider, "reset-gpio", &fResetPin) ||
	    !propertyU32(provider, "irq-gpio", &fIrqPin)) {
		IOLog("TulipTouch: missing properties\n");
		return false;
	}
	fI2C = TulipI2C::forDevice(provider);
	fTLMM = TulipTLMM::wait();
	if (!fI2C || !fTLMM) {
		IOLog("TulipTouch: no I2C bus or TLMM\n");
		return false;
	}

	/* Reset (high = run) and interrupt pins as in Android's ts_active. */
	fTLMM->set(fResetPin, true);
	fTLMM->configure(fResetPin, 0, kTulipPullUp, 16, true);
	fTLMM->configure(fIrqPin, 0, kTulipPullUp, 16, false);

	uint8_t params[NVT_PARAMS_SIZE];
	bool found = fI2C->readRegister((uint8_t)fAddr, NVT_PARAMS_START, params, sizeof(params));
	if (!found) {
		fTLMM->set(fResetPin, false);
		IOSleep(10);
		fTLMM->set(fResetPin, true);
		IOSleep(200);
		found = fI2C->readRegister((uint8_t)fAddr, NVT_PARAMS_START, params, sizeof(params));
	}
	if (!found) {
		IOLog("TulipTouch: no answer from the touch controller\n");
		return false;
	}
	fWidth = (uint32_t)params[4] << 8 | params[5];
	fHeight = (uint32_t)params[6] << 8 | params[7];
	IOLog("TulipTouch: %ux%u, %u touches, chip 0x%02x\n", fWidth, fHeight, params[9], params[14]);
	if (fWidth == 0 || fWidth > 4096 || fHeight == 0 || fHeight > 4096) {
		return false;
	}

	/* Touch coordinates are panel pixels; iOS wants the canvas that AppleCLCD shows the center of. */
	fCanvasWidth = fWidth;
	fCanvasHeight = fHeight;
	IORegistryEntry *disp = IORegistryEntry::fromPath("/arm-io/disp0", gIODTPlane);
	if (disp) {
		OSData *w = OSDynamicCast(OSData, disp->getProperty("canvas-width"));
		OSData *h = OSDynamicCast(OSData, disp->getProperty("canvas-height"));
		if (w && h && w->getLength() == 4 && h->getLength() == 4) {
			fCanvasWidth = *(const uint32_t *)w->getBytesNoCopy();
			fCanvasHeight = *(const uint32_t *)h->getBytesNoCopy();
		}
		disp->release();
	}
	/* AppleCLCD shows the canvas scaled to fit the panel and centered; touches map back through the same fit. */
	uint64_t scale = ((uint64_t)fWidth << 16) / fCanvasWidth;
	if (((uint64_t)fHeight << 16) / fCanvasHeight < scale) {
		scale = ((uint64_t)fHeight << 16) / fCanvasHeight;
	}
	if (scale > 1ULL << 16) {
		scale = 1ULL << 16;
	}
	fOutWidth = (int32_t)((fCanvasWidth * scale) >> 16);
	fOutHeight = (int32_t)((fCanvasHeight * scale) >> 16);
	fOutX = ((int32_t)fWidth - fOutWidth) / 2;
	fOutY = ((int32_t)fHeight - fOutHeight) / 2;

	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	setProperty("DisplayIntegrated", kOSBooleanTrue);
	/* 68x144 mm, 16.16 fixed point. */
	setProperty("SurfaceWidth", 68ULL << 16, 32);
	setProperty("SurfaceHeight", 144ULL << 16, 32);

	IOWorkLoop *wl = getWorkLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &TulipTouch::poll);
	if (!wl || !fTimer || wl->addEventSource(fTimer) != kIOReturnSuccess) {
		return false;
	}
	if (fTLMM->interruptsReady()) {
		fInterrupt = IOInterruptEventSource::interruptEventSource(this, &TulipTouch::interrupt, provider, 0);
		if (fInterrupt && wl->addEventSource(fInterrupt) != kIOReturnSuccess) {
			OSSafeReleaseNULL(fInterrupt);
		}
	}
	if (fInterrupt) {
		fInterrupt->enable();
	} else {
		fTLMM->latch(fIrqPin, kTulipTriggerFalling);
	}
	IOLog("TulipTouch: %s\n", fInterrupt ? "interrupts" : "polling");
	registerService();
	readTouches();
	arm();
	return true;
}

void
TulipTouch::report(const uint8_t *buf)
{
	bool down[NVT_MAX_TOUCHES] = {};
	IOFixed x[NVT_MAX_TOUCHES], y[NVT_MAX_TOUCHES];

	memcpy(x, fX, sizeof(x));
	memcpy(y, fY, sizeof(y));
	for (int i = 0; i < NVT_MAX_TOUCHES; i++) {
		const uint8_t *t = buf + i * NVT_TOUCH_SIZE;
		int type = t[0] & 7, slot = (t[0] >> 3) - 1;
		if (t[0] == NVT_TOUCH_INVALID || slot < 0 || slot >= NVT_MAX_TOUCHES ||
		    (type != NVT_TOUCH_NEW && type != NVT_TOUCH_UPDATE)) {
			continue;
		}
		int32_t px = (int32_t)((uint32_t)t[1] << 4 | t[3] >> 4) - fOutX;
		int32_t py = bottomStretch((int32_t)((uint32_t)t[2] << 4 | (t[3] & 0xf)), (int32_t)fHeight) - fOutY;
		down[slot] = true;
		x[slot] = unitFixed(px, fOutWidth);
		y[slot] = unitFixed(py, fOutHeight);
	}

	uint64_t now = mach_absolute_time();
	IOHIDEvent *collection = IOHIDEvent::digitizerEvent(now, 0, kIOHIDDigitizerTransducerTypeHand, false, 0, 0, 0);
	if (!collection) {
		return;
	}
	collection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, TRUE);
	UInt32 collectionMask = 0;
	bool any = false, changed = false;
	IOFixed cx = 0, cy = 0;
	for (int i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (!down[i] && !fDown[i]) {
			continue;
		}
		UInt32 mask = 0;
		if (down[i] != fDown[i]) {
			mask |= kIOHIDDigitizerEventTouch | kIOHIDDigitizerEventRange;
		}
		if (down[i] && !fDown[i]) {
			mask |= kIOHIDDigitizerEventIdentity;
			fSwipe[i] = edgeSwipe(x[i], y[i]);
			if (fSwipe[i]) {
				mask |= kIOHIDDigitizerEventFromEdgeTip;
			}
		}
		if (down[i] && fDown[i] && (x[i] != fX[i] || y[i] != fY[i])) {
			mask |= kIOHIDDigitizerEventPosition;
			if (fSwipe[i]) {
				mask |= kIOHIDDigitizerEventSwipeLocked | fSwipe[i];
			}
		}
		IOHIDEvent *finger = IOHIDEvent::digitizerEvent(now, (UInt32)i + 2, kIOHIDDigitizerTransducerTypeFinger,
		    down[i], down[i] ? 1 : 0, x[i], y[i]);
		if (finger) {
			finger->setIntegerValue(kIOHIDEventFieldDigitizerIndex, i);
			finger->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, i + 2);
			finger->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, TRUE);
			finger->setIntegerValue(kIOHIDEventFieldDigitizerTouch, down[i]);
			finger->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, mask);
			collection->appendChild(finger);
			finger->release();
		}
		if (down[i] != fDown[i]) {
			kprintf("TulipTouch: finger %d %s\n", i, down[i] ? "down" : "up");
		}
		if (down[i] && !any) {
			cx = x[i];
			cy = y[i];
		}
		collectionMask |= mask;
		any |= down[i];
		changed |= mask != 0;
		fDown[i] = down[i];
		fX[i] = x[i];
		fY[i] = y[i];
	}
	if (changed && tulip_display_on) {
		collection->setFixedValue(kIOHIDEventFieldDigitizerX, cx);
		collection->setFixedValue(kIOHIDEventFieldDigitizerY, cy);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerRange, any);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerTouch, any);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, collectionMask);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerButtonMask, any ? 1 : 0);
		dispatchEvent(collection);
	}
	collection->release();
}

void
TulipTouch::interrupt(OSObject *owner, IOInterruptEventSource *sender __unused, int count __unused)
{
	TulipTouch *self = (TulipTouch *)owner;
	if (self->fInterrupts++ == 0) {
		IOLog("TulipTouch: first interrupt\n");
	}
	self->readTouches();
	self->arm();
}

void
TulipTouch::poll(OSObject *owner, IOTimerEventSource *sender __unused)
{
	TulipTouch *self = (TulipTouch *)owner;
	bool line = !self->fTLMM->get(self->fIrqPin);
	bool interrupts = self->fInterrupt && self->fTLMM->interruptsReady();
	bool latched = !interrupts && self->fTLMM->takeLatched(self->fIrqPin);
	bool down = self->anyDown();

	if (latched || line || down) {
		self->readTouches();
	}
	self->arm();
}
