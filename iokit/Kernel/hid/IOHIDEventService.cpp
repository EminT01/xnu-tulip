/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDPrivateKeys.h>
#include <IOKit/hid/IOHIDDebug.h>
#include <IOKit/hid/IOHIDEvent.h>
#include <IOKit/hid/IOHIDEventServiceUserClient.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include "IOHIDEventService.h"

extern "C" {
#include <sys/proc.h>
}

os_log_t
_HIDLogCategory(HIDLogCategory category __unused)
{
	return OS_LOG_DEFAULT;
}

OSDefineMetaClassAndAbstractStructors(IOHIDEventService, IOService);

static void
setNumber(IOService *service, const char *key, UInt32 value)
{
	service->setProperty(key, value, 32);
}

bool
IOHIDEventService::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	fClientLock = IOLockAlloc();
	if (!fClientLock) {
		return false;
	}
	setProperty(kIOHIDTransportKey, "SPU");
	setProperty(kIOHIDProductKey, getProduct());
	setProperty(kIOHIDManufacturerKey, "Apple Inc.");
	setNumber(this, kIOHIDVendorIDKey, 0x05ac);
	setNumber(this, kIOHIDProductIDKey, getPrimaryUsage());
	setNumber(this, kIOHIDLocationIDKey, (UInt32)getRegistryEntryID());
	setNumber(this, kIOHIDReportIntervalKey, 10000);
	setNumber(this, kIOHIDPrimaryUsagePageKey, getPrimaryUsagePage());
	setNumber(this, kIOHIDPrimaryUsageKey, getPrimaryUsage());
	setProperty(kIOHIDBuiltInKey, kOSBooleanTrue);

	OSDictionary *pair = OSDictionary::withCapacity(2);
	OSArray *pairs = OSArray::withCapacity(1);
	OSNumber *page = OSNumber::withNumber(getPrimaryUsagePage(), 32);
	OSNumber *usage = OSNumber::withNumber(getPrimaryUsage(), 32);
	if (pair && pairs && page && usage) {
		pair->setObject(kIOHIDDeviceUsagePageKey, page);
		pair->setObject(kIOHIDDeviceUsageKey, usage);
		pairs->setObject(pair);
		setProperty(kIOHIDDeviceUsagePairsKey, pairs);
	}
	OSSafeReleaseNULL(pair);
	OSSafeReleaseNULL(pairs);
	OSSafeReleaseNULL(page);
	OSSafeReleaseNULL(usage);

	/* Apple's HID personalities (IOHIDFamily Info.plist) put this key on every event service. */
	setProperty("HIDServiceSupport", kOSBooleanTrue);
	setProperty("IOUserClientClass", "IOHIDEventServiceUserClient");
	return true;
}

void
IOHIDEventService::free(void)
{
	if (fClientLock) {
		IOLockFree(fClientLock);
		fClientLock = NULL;
	}
	IOService::free();
}

bool
IOHIDEventService::open(IOService *client, IOOptionBits options, void *context, Action action)
{
	Client c = { client, context, action };
	return IOService::open(client, options, &c);
}

bool
IOHIDEventService::handleOpen(IOService *client, IOOptionBits options __unused, void *argument)
{
	const Client *c = (const Client *)argument;
	bool ok = false;

	if (!c) {
		return false;
	}
	IOLockLock(fClientLock);
	if (fClientCount < kMaxClients) {
		bool present = false;
		for (unsigned int i = 0; i < fClientCount; i++) {
			present |= fClients[i].client == client;
		}
		if (!present) {
			client->retain();
			fClients[fClientCount++] = *c;
			ok = true;
			char name[32];
			proc_selfname(name, sizeof(name));
			kprintf("%s: opened by %s (%s), %u clients\n", getName(), name, client->getName(), fClientCount);
		}
	}
	IOLockUnlock(fClientLock);
	return ok;
}

void
IOHIDEventService::handleClose(IOService *client, IOOptionBits options __unused)
{
	IOLockLock(fClientLock);
	for (unsigned int i = 0; i < fClientCount; i++) {
		if (fClients[i].client == client) {
			fClients[i] = fClients[--fClientCount];
			client->release();
			break;
		}
	}
	IOLockUnlock(fClientLock);
}

bool
IOHIDEventService::handleIsOpen(const IOService *client) const
{
	bool open = false;

	IOLockLock(fClientLock);
	for (unsigned int i = 0; i < fClientCount; i++) {
		open |= client == NULL || fClients[i].client == client;
	}
	IOLockUnlock(fClientLock);
	return open;
}

void
IOHIDEventService::dispatchEvent(IOHIDEvent *event, IOOptionBits options)
{
	Client clients[kMaxClients];
	unsigned int count;

	event->setSenderID(getRegistryEntryID());
	IOLockLock(fClientLock);
	count = fClientCount;
	for (unsigned int i = 0; i < count; i++) {
		clients[i] = fClients[i];
		clients[i].client->retain();
	}
	IOLockUnlock(fClientLock);

	for (unsigned int i = 0; i < count; i++) {
		if (clients[i].action) {
			clients[i].action(clients[i].client, this, clients[i].context, event, options);
		}
		clients[i].client->release();
	}
}

IOHIDEvent *
IOHIDEventService::copyEvent(IOHIDEventType type __unused, IOHIDEvent *matching __unused, IOOptionBits options __unused)
{
	return NULL;
}

IOHIDEvent *
IOHIDEventService::copyMatchingEvent(OSDictionary *matching __unused)
{
	return NULL;
}

IOReturn
IOHIDEventService::setElementValue(UInt32 usagePage __unused, UInt32 usage __unused, UInt32 value __unused)
{
	return kIOReturnUnsupported;
}

IOReturn
IOHIDEventService::setProperties(OSObject *properties)
{
	return OSDynamicCast(OSDictionary, properties) ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn
IOHIDEventService::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	if (type != kIOHIDEventServiceUserClientType) {
		return IOService::newUserClient(owningTask, securityID, type, properties, handler);
	}
	IOUserClient *client = OSTypeAlloc(IOHIDEventServiceUserClient);
	if (!client) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(this)) {
		client->release();
		return kIOReturnBadArgument;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnUnsupported;
	}
	*handler = client;
	return kIOReturnSuccess;
}
