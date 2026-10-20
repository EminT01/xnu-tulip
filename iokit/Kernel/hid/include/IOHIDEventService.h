/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_HID_IOHIDEVENTSERVICE_H
#define _IOKIT_HID_IOHIDEVENTSERVICE_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/hid/IOHIDEventTypes.h>

class IOHIDEvent;

enum {
	kHIDDispatchOptionDeliveryNotificationForce    = (1 << 12),
	kHIDDispatchOptionDeliveryNotificationSuppress = (1 << 13),
};

/* Base of our input drivers: the part of IOHIDFamily's class that iOS opens through IOHIDEventServiceUserClient. */
class IOHIDEventService : public IOService
{
	OSDeclareAbstractStructors(IOHIDEventService);

public:
	typedef void (*Action)(OSObject *target, OSObject *sender, void *context, OSObject *event, IOOptionBits options);

	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	virtual bool open(IOService *client, IOOptionBits options, void *context, Action action);
	virtual IOHIDEvent *copyEvent(IOHIDEventType type, IOHIDEvent *matching = NULL, IOOptionBits options = 0);
	virtual IOHIDEvent *copyMatchingEvent(OSDictionary *matching);
	virtual IOReturn setElementValue(UInt32 usagePage, UInt32 usage, UInt32 value);
	virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	virtual UInt32 getPrimaryUsagePage() = 0;
	virtual UInt32 getPrimaryUsage() = 0;

protected:
	virtual bool handleOpen(IOService *client, IOOptionBits options, void *argument) APPLE_KEXT_OVERRIDE;
	virtual void handleClose(IOService *client, IOOptionBits options) APPLE_KEXT_OVERRIDE;
	virtual bool handleIsOpen(const IOService *client) const APPLE_KEXT_OVERRIDE;

	void dispatchEvent(IOHIDEvent *event, IOOptionBits options = 0);
	virtual const char *getProduct() = 0;

private:
	enum { kMaxClients = 4 };
	struct Client {
		IOService *client;
		void *context;
		Action action;
	};

	IOLock *fClientLock;
	Client fClients[kMaxClients];
	unsigned int fClientCount;
};

#endif /* _IOKIT_HID_IOHIDEVENTSERVICE_H */
