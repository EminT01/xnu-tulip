/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

/* AppleCLCD: the boot loader's framebuffer as an iOS 16 IOMobileFramebuffer, fed by CPU copies. */

#include "TulipSurface.h"

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>

extern "C" {
#include <machine/atomic.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
}

enum {
	kSelGetLayerDefaultSurface = 3,  /* -> [IOSurface port name] */
	kSelSwapBegin = 4,               /* -> [swap id] */
	kSelSwapEnd = 5,                 /* struct in: swap record */
	kSelSwapWait = 6,
	kSelGetID = 7,
	kSelGetDisplaySize = 8,          /* -> [width, height] */
	kSelIsMainDisplay = 18,
	kSelGetDotPitch = 28,
	kSelNotifications = 72,          /* [callback, refcon, type, 0] */
};

/* Swap record: +0x50 swap id, +0x54 surface id per layer, +0x64 source and +0xc4 destination rects, +0x104 layer mask. */
#define SWAP_RECORD_SIZE   0x46c
#define SWAP_SURFACES      0x54
#define SWAP_SOURCE_RECTS  0x64
#define SWAP_DEST_RECTS    0xc4
#define SWAP_LAYER_MASK    0x104
#define LAYERS             4
#define NOTIFY_TYPES       8
#define NOTIFY_VSYNC       5
#define VSYNC_US           16667
#define DOT_PITCH          403
#define FORMAT_BGRA        0x42475241

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer
{
	OSDeclareDefaultStructors(AppleCLCD);

public:
	uint32_t fWidth, fHeight, fRowBytes;
	TulipSurface *fPanel;
	IOLock *fLock;
	OSArray *fClients;
	uint32_t fSwapID;

	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *record);

private:
	IOWorkLoop *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t fVsyncCount;

	static void vsync(OSObject *owner, IOTimerEventSource *sender);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

	/* Caller holds the framebuffer lock. */
	void notify(uint32_t type, io_user_reference_t *values, uint32_t count);

private:
	AppleCLCD *fFB;
	task_t fTask;
	mach_port_t fPort;
	io_user_reference_t fCallback[NOTIFY_TYPES];
	io_user_reference_t fRefcon[NOTIFY_TYPES];
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (video.v_baseAddr == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("TulipFB: no boot framebuffer\n");
		return false;
	}
	OSDictionary *match = IOService::nameMatching("IOCoreSurfaceRoot");
	IOService *surfaces = match ? IOService::waitForMatchingService(match, 10 * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(match);
	if (!surfaces) {
		IOLog("TulipFB: no IOCoreSurfaceRoot\n");
		return false;
	}
	surfaces->release();
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fPanel = tulip_surface_create_physical(video.v_baseAddr, fWidth, fHeight, fRowBytes, FORMAT_BGRA);
	fWorkLoop = IOWorkLoop::workLoop();
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsync);
	if (!fLock || !fClients || !fPanel || !fWorkLoop || !fVsync ||
	    fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("TulipFB: setup failed\n");
		return false;
	}
	fVsync->setTimeoutUS(VSYNC_US);
	setProperty("IOUserClientClass", "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("TulipFB: %ux%u, %u bytes per row, panel surface %u\n", fWidth, fHeight, fRowBytes, fPanel->id);
	return true;
}

void
AppleCLCD::vsync(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	/* 64-bit count and timestamp, sent as 32-bit halves. */
	io_user_reference_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32), (uint32_t)now, (uint32_t)(now >> 32), 0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		((IOMobileFramebufferUserClient *)fb->fClients->getObject(i))->notify(NOTIFY_VSYNC, values, 6);
	}
	IOLockUnlock(fb->fLock);
	sender->setTimeoutUS(VSYNC_US);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static int32_t
get32(const uint8_t *p, uint32_t off)
{
	int32_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static uint32_t gSwapsLogged;

/* Copy each enabled 32-bit layer into the panel, clipped, without scaling. */
void
AppleCLCD::swap(const uint8_t *record)
{
	uint32_t mask = (uint32_t)get32(record, SWAP_LAYER_MASK);
	uint8_t *panel = fPanel->pixels();

	for (uint32_t layer = 0; layer < LAYERS; layer++) {
		uint32_t id = (uint32_t)get32(record, SWAP_SURFACES + 4 * layer);
		const uint8_t *src = record + SWAP_SOURCE_RECTS + 16 * layer;
		const uint8_t *dst = record + SWAP_DEST_RECTS + 16 * layer;
		if (!(mask & (1u << layer)) || id == 0 || id == fPanel->id) {
			continue;
		}
		TulipSurface *s = tulip_surface_copy(id);
		if (!s) {
			continue;
		}
		int32_t sx = MAX(get32(src, 0), 0), sy = MAX(get32(src, 4), 0);
		int32_t dx = MAX(get32(dst, 0), 0), dy = MAX(get32(dst, 4), 0);
		int32_t w = get32(dst, 8), h = get32(dst, 12);
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		w = MIN(w, MIN((int32_t)s->width - sx, (int32_t)fWidth - dx));
		h = MIN(h, MIN((int32_t)s->height - sy, (int32_t)fHeight - dy));
		if (gSwapsLogged < 8) {
			gSwapsLogged++;
			kprintf("TulipFB: swap %d layer %u surface %u %ux%u format 0x%x -> %d,%d %dx%d\n",
			    get32(record, 0x50), layer, id, s->width, s->height, s->pixelFormat, dx, dy, w, h);
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow + (uint64_t)(sx + w) * 4;
		uint8_t *pixels = s->pixels();
		if (w > 0 && h > 0 && s->bytesPerElement == 4 && s->elementWidth == 1 && s->elementHeight == 1 &&
		    pixels && last <= s->data->getLength()) {
			for (int32_t y = 0; y < h; y++) {
				memcpy(panel + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4,
				    pixels + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4, (size_t)w * 4);
			}
		}
		s->release();
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (!fFB || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type __unused,
    io_user_reference_t refCon __unused)
{
	IOLockLock(fFB->fLock);
	mach_port_t old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::notify(uint32_t type, io_user_reference_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= NOTIFY_TYPES || fCallback[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fCallback[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, values, count, kIOUserNotifyOptionCanDrop);
}

static uint64_t gLoggedSelectors;

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused, void *reference __unused)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = {};
	uint32_t outCount = 0;

	if (selector < 64 && !(os_atomic_or_orig(&gLoggedSelectors, 1ULL << selector, relaxed) & (1ULL << selector))) {
		char name[32];
		proc_selfname(name, sizeof(name));
		kprintf("TulipFB: %s selector %u in %u/%u out %u/%u\n", name, selector, args->scalarInputCount,
		    args->structureInputSize, args->scalarOutputCount, args->structureOutputSize);
	}

	switch (selector) {
	case kSelGetLayerDefaultSurface:
		out[0] = tulip_surface_port(fFB->fPanel, fTask);
		outCount = 1;
		break;
	case kSelSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSelSwapEnd:
		if (args->structureInputDescriptor || args->structureInputSize < SWAP_RECORD_SIZE) {
			ret = kIOReturnBadArgument;
		} else {
			uint8_t record[SWAP_RECORD_SIZE];
			memcpy(record, args->structureInput, sizeof(record));
			fFB->swap(record);
		}
		break;
	case kSelGetID:
		outCount = 1;
		break;
	case kSelGetDisplaySize:
		out[0] = fFB->fWidth;
		out[1] = fFB->fHeight;
		outCount = 2;
		break;
	case kSelIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSelGetDotPitch:
		out[0] = DOT_PITCH;
		outCount = 1;
		break;
	case kSelNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < NOTIFY_TYPES) {
			IOLockLock(fFB->fLock);
			fCallback[args->scalarInput[2]] = args->scalarInput[0];
			fRefcon[args->scalarInput[2]] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
		}
		break;
	case kSelSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (!args->structureOutputDescriptor && args->structureOutput) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return ret;
}
