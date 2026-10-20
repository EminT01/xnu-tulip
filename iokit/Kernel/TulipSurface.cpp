/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

/* IOCoreSurfaceRoot: replaces Apple's closed IOSurface kext for iOS 16 IOSurface.framework. */

#include "TulipSurface.h"

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitServer.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <pexpert/pexpert.h>

extern "C" {
#include <machine/atomic.h>
#include <sys/proc.h>
}

/* Surface description returned by create and lookup: +0 pixels, +8 state page, +0x10 timestamps. */
#define DESC_SIZE           0xa68
#define DESC_PLANES         0x68
#define DESC_PLANE_STRIDE   0x80
#define STATE_SIZE          PAGE_SIZE
#define STATE_SEED          0xc
#define STATE_VALUES_GEN    0x20
#define STATE_VALUES_SIZE   0x28
#define STATE_TIMESTAMPS    0x800
#define MAX_SURFACES        4096
#define MAX_DIMENSION       16384
#define MAX_SURFACE_BYTES   (256ULL << 20)
#define MAX_TOTAL_BYTES     (768ULL << 20)
#define MAX_PROPERTY_BYTES  (64U << 10)
#define ROW_ALIGN           64
#define LOCK_READ_ONLY      0x1
#define RETURN_TOO_SMALL    ((IOReturn)0xe00002db)

enum {
	kSelCreate = 0,            /* scalar [parent id], struct in = serialized properties */
	kSelLookup = 4,            /* scalar [id] */
	kSelSetValue = 9,          /* u32 id, u32 pad, serialized [value, key] or dictionary */
	kSelCopyValues = 0xa,      /* out: u32 reserved, u32 generation, serialized values */
	kSelRemoveValue = 0xb,     /* u32 id, u32 pad, key (empty: all) */
	kSelLimits = 13,           /* out 0x18 bytes; u16 at +0x16 is the ABI version, 1 */
	kSelLookupFromPort = 0x22, /* scalar [port name] */
	kSelCreatePort = 0x23,     /* scalars [id, options] -> scalar [port name] */
};

OSDefineMetaClassAndStructors(TulipSurface, OSObject);

static IOLock *gSurfaceLock;
bool tulip_surface_log;
static TulipSurface *gSurfaces[MAX_SURFACES];
static uint32_t gSurfaceUsers[MAX_SURFACES];
static uint32_t gNextSurfaceID = 1;
static uint64_t gSurfaceBytes;
static OSDictionary *gSurfaceValues[MAX_SURFACES];
static uint32_t gSurfaceValuesGen[MAX_SURFACES];

void
TulipSurface::free()
{
	if (data && wired) {
		IOLockLock(gSurfaceLock);
		gSurfaceBytes -= data->getLength();
		IOLockUnlock(gSurfaceLock);
	}
	OSSafeReleaseNULL(kernelMap);
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	OSObject::free();
}

uint8_t *
TulipSurface::pixels() const
{
	return kernelMap ? (uint8_t *)kernelMap->getVirtualAddress() : NULL;
}

TulipSurface *
tulip_surface_copy(uint32_t id)
{
	TulipSurface *s = NULL;

	if (id == 0 || id >= MAX_SURFACES || !gSurfaceLock) {
		return NULL;
	}
	IOLockLock(gSurfaceLock);
	s = gSurfaces[id];
	if (s) {
		s->retain();
	}
	IOLockUnlock(gSurfaceLock);
	return s;
}

static void
surface_use(TulipSurface *s)
{
	IOLockLock(gSurfaceLock);
	if (gSurfaces[s->id] == s) {
		gSurfaceUsers[s->id]++;
	}
	IOLockUnlock(gSurfaceLock);
}

/* The last user takes the surface out of the id table. */
static void
surface_unuse(TulipSurface *s)
{
	TulipSurface *gone = NULL;
	OSDictionary *values = NULL;

	IOLockLock(gSurfaceLock);
	if (gSurfaces[s->id] == s && --gSurfaceUsers[s->id] == 0) {
		gone = s;
		gSurfaces[s->id] = NULL;
		values = gSurfaceValues[s->id];
		gSurfaceValues[s->id] = NULL;
	}
	IOLockUnlock(gSurfaceLock);
	OSSafeReleaseNULL(values);
	OSSafeReleaseNULL(gone);
}

static uint32_t
dict_u32(OSDictionary *d, const char *key, uint32_t fallback)
{
	OSNumber *n = OSDynamicCast(OSNumber, d->getObject(key));
	return n ? n->unsigned32BitValue() : fallback;
}

static uint32_t
div_up(uint32_t a, uint32_t b)
{
	return b ? (a + b - 1) / b : a;
}

static uint32_t
row_bytes(uint32_t width, uint8_t elementWidth, uint16_t bytesPerElement)
{
	return (div_up(width, elementWidth) * bytesPerElement + ROW_ALIGN - 1) & ~(ROW_ALIGN - 1);
}

static bool surface_publish(TulipSurface *s);

/* New surface from IOSurfaceCreate properties, in the id table, retained; NULL if it does not fit. */
static TulipSurface *
surface_create(OSDictionary *props, uint32_t parentID)
{
	TulipSurface *s = OSTypeAlloc(TulipSurface);
	if (!s || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->parentID = parentID;
	s->width = dict_u32(props, "IOSurfaceWidth", 0);
	s->height = dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = dict_u32(props, "IOSurfacePixelFormat", 0);
	s->offset = dict_u32(props, "IOSurfaceOffset", 0);
	s->cacheMode = dict_u32(props, "IOSurfaceCacheMode", 0);
	s->bytesPerRow = dict_u32(props, "IOSurfaceBytesPerRow", row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->width > MAX_DIMENSION || s->height > MAX_DIMENSION) {
		s->release();
		return NULL;
	}

	uint64_t end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * div_up(s->height, s->elementHeight);
	OSArray *planes = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	if (planes) {
		uint64_t next = s->offset;
		s->planeCount = MIN(planes->getCount(), (unsigned int)TULIP_SURFACE_MAX_PLANES);
		for (uint32_t i = 0; i < s->planeCount; i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			TulipSurfacePlane *p = &s->planes[i];
			if (!pd) {
				continue;
			}
			p->width = dict_u32(pd, "IOSurfacePlaneWidth", 0);
			p->height = dict_u32(pd, "IOSurfacePlaneHeight", 0);
			p->bytesPerElement = (uint16_t)dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			p->bytesPerRow = dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = dict_u32(pd, "IOSurfacePlaneSize", p->bytesPerRow * div_up(p->height, p->elementHeight));
			next = round_page((uint64_t)p->offset + p->size);
			end = MAX(end, (uint64_t)p->offset + p->size);
		}
	}
	end = MAX(end, (uint64_t)dict_u32(props, "IOSurfaceAllocSize", 0));
	end = round_page(MAX(end, (uint64_t)1));
	if (end > MAX_SURFACE_BYTES) {
		s->release();
		return NULL;
	}
	s->allocSize = (uint32_t)end;

	IOLockLock(gSurfaceLock);
	bool fits = gSurfaceBytes + end <= MAX_TOTAL_BYTES;
	if (fits) {
		gSurfaceBytes += end;
	}
	IOLockUnlock(gSurfaceLock);
	if (!fits) {
		s->release();
		return NULL;
	}
	IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, end, PAGE_SIZE);
	if (!buf) {
		IOLockLock(gSurfaceLock);
		gSurfaceBytes -= end;
		IOLockUnlock(gSurfaceLock);
		s->release();
		return NULL;
	}
	bzero(buf->getBytesNoCopy(), end);
	s->data = buf;
	s->wired = true;
	s->kernelMap = buf->map();
	if (!s->kernelMap || !surface_publish(s)) {
		s->release();
		return NULL;
	}
	if (tulip_surface_log) {
		char name[32], keys[256] = "";
		size_t len = 0;
		proc_selfname(name, sizeof(name));
		OSCollectionIterator *it = OSCollectionIterator::withCollection(props);
		for (OSSymbol *key; it && (key = OSDynamicCast(OSSymbol, it->getNextObject())) && len < sizeof(keys);) {
			len += (size_t)snprintf(keys + len, sizeof(keys) - len, " %s", key->getCStringNoCopy());
		}
		OSSafeReleaseNULL(it);
		IOLog("TulipSurface: %s create %u parent %u %ux%u format 0x%x bpe %u element %ux%u bpr %u offset %u planes %u "
		    "size %u:%s\n", name, s->id, parentID, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->elementWidth, s->elementHeight, s->bytesPerRow, s->offset, s->planeCount, s->allocSize, keys);
	}
	return s;
}

/* Give s a state page and an id in the table; the table keeps a reference. */
static bool
surface_publish(TulipSurface *s)
{
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, STATE_SIZE, PAGE_SIZE);
	if (!s->state) {
		return false;
	}
	bzero(s->state->getBytesNoCopy(), STATE_SIZE);

	IOLockLock(gSurfaceLock);
	uint32_t id = 0;
	for (uint32_t tries = 0; tries < MAX_SURFACES - 1; tries++) {
		uint32_t candidate = gNextSurfaceID;
		gNextSurfaceID = gNextSurfaceID + 1 < MAX_SURFACES ? gNextSurfaceID + 1 : 1;
		if (!gSurfaces[candidate]) {
			id = candidate;
			break;
		}
	}
	if (id) {
		s->id = id;
		gSurfaces[id] = s;
		gSurfaceUsers[id] = 0;
		gSurfaceValuesGen[id] = 1;
		s->retain();
	}
	IOLockUnlock(gSurfaceLock);
	return id != 0;
}

TulipSurface *
tulip_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height, uint32_t bytesPerRow,
    uint32_t pixelFormat)
{
	if (!gSurfaceLock) {
		return NULL;
	}
	TulipSurface *s = OSTypeAlloc(TulipSurface);
	if (!s || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = 1;
	s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page((uint64_t)bytesPerRow * height);
	s->userCache = kIOMapWriteCombineCache;
	s->data = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	if (s->data) {
		s->kernelMap = s->data->map(kIOMapWriteCombineCache);
	}
	if (!s->kernelMap || !surface_publish(s)) {
		s->release();
		return NULL;
	}
	/* Never released: one extra user keeps it in the table. */
	surface_use(s);
	return s;
}

mach_port_name_t
tulip_surface_port(TulipSurface *surface, task_t task)
{
	return iokit_make_send_right(task, surface, IKOT_IOKIT_OBJECT);
}

/* A surface mapped into one client's task. */
class TulipSurfaceRef : public OSObject
{
	OSDeclareDefaultStructors(TulipSurfaceRef);

public:
	TulipSurface *surface;
	IOMemoryMap *dataMap;
	IOMemoryMap *stateMap;
	uint32_t count;

	virtual bool
	init() APPLE_KEXT_OVERRIDE
	{
		return OSObject::init();
	}

	virtual void
	free() APPLE_KEXT_OVERRIDE
	{
		OSSafeReleaseNULL(dataMap);
		OSSafeReleaseNULL(stateMap);
		if (surface) {
			surface_unuse(surface);
			surface->release();
		}
		OSObject::free();
	}
};

OSDefineMetaClassAndStructors(TulipSurfaceRef, OSObject);

class IOCoreSurfaceRoot : public IOService
{
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

class IOSurfaceRootUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

public:
	virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual void free() APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	virtual IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *, void *, void *, void *, void *, void *);
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);

private:
	task_t fTask;
	IOLock *fLock;
	OSArray *fRefs;

	TulipSurfaceRef *findRef(uint32_t id, unsigned int *index);
	TulipSurface *heldSurface(uint32_t id);
	IOReturn describe(TulipSurface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);
	IOReturn setValue(IOExternalMethodArguments *args);
	IOReturn copyValues(IOExternalMethodArguments *args);
	IOReturn removeValue(IOExternalMethodArguments *args);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	gSurfaceLock = IOLockAlloc();
	uint32_t log = 0;
	tulip_surface_log = PE_parse_boot_argn("tulip_surflog", &log, sizeof(log)) && log;
	registerService();
	return true;
}

IOReturn
IOCoreSurfaceRoot::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
	IOSurfaceRootUserClient *client = OSTypeAlloc(IOSurfaceRootUserClient);

	if (!client || !client->initWithTask(owningTask, securityID, type) || !client->attach(this) ||
	    !client->start(this)) {
		if (client) {
			client->detach(this);
			client->release();
		}
		return kIOReturnNoMemory;
	}
	*handler = client;
	return kIOReturnSuccess;
}

static uint64_t gLoggedSelectors;
static uint64_t gLoggedTraps;

static void
log_once(uint64_t *seen, const char *what, uint32_t n)
{
	if (n < 64 && !(os_atomic_or_orig(seen, 1ULL << n, relaxed) & (1ULL << n))) {
		char name[32];
		proc_selfname(name, sizeof(name));
		kprintf("TulipSurface: %s %s %u\n", name, what, n);
	}
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(8);
	return fLock && fRefs;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	if (fLock) {
		IOLockFree(fLock);
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

TulipSurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		TulipSurfaceRef *ref = (TulipSurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id) {
			if (index) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

TulipSurface *
IOSurfaceRootUserClient::heldSurface(uint32_t id)
{
	TulipSurface *s = NULL;

	IOLockLock(fLock);
	TulipSurfaceRef *ref = findRef(id, NULL);
	if (ref) {
		s = ref->surface;
		s->retain();
	}
	IOLockUnlock(fLock);
	return s;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, sizeof(v));
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, sizeof(v));
}

static IOReturn
struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (!args->structureOutput || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Map s into this client once and return its description; consumes one reference on s. */
IOReturn
IOSurfaceRootUserClient::describe(TulipSurface *s, IOExternalMethodArguments *args)
{
	bool existing = false;
	TulipSurfaceRef *ref;

	IOLockLock(fLock);
	ref = findRef(s->id, NULL);
	if (ref && ref->surface == s) {
		existing = true;
		ref->retain();
		IOLockUnlock(fLock);
		s->release();
	} else {
		IOLockUnlock(fLock);
		ref = OSTypeAlloc(TulipSurfaceRef);
		if (!ref || !ref->init()) {
			OSSafeReleaseNULL(ref);
			s->release();
			return kIOReturnNoMemory;
		}
		ref->surface = s;
		surface_use(s);
		ref->count = 1;
		ref->dataMap = s->data->createMappingInTask(fTask, 0, kIOMapAnywhere | s->userCache);
		ref->stateMap = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere);
		if (!ref->dataMap || !ref->stateMap) {
			ref->release();
			return kIOReturnNoMemory;
		}
		IOLockLock(fLock);
		fRefs->setObject(ref);
		IOLockUnlock(fLock);
	}

	uint8_t *out = (uint8_t *)IOMallocZeroData(DESC_SIZE);
	if (!out) {
		ref->release();
		return kIOReturnNoMemory;
	}
	/* Base -1 tells the library to reuse the object it already has for this surface. */
	put64(out, 0x00, existing ? ~0ULL : ref->dataMap->getAddress());
	put64(out, 0x08, ref->stateMap->getAddress());
	put64(out, 0x10, ref->stateMap->getAddress() + STATE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, sizeof(s->bytesPerElement));
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + DESC_PLANES + i * DESC_PLANE_STRIDE;
		put32(p, 0x00, s->planes[i].width);
		put32(p, 0x04, s->planes[i].height);
		put32(p, 0x10, s->planes[i].bytesPerRow);
		put32(p, 0x0c, s->planes[i].offset);
		put32(p, 0x14, s->planes[i].size);
		memcpy(p + 0x18, &s->planes[i].bytesPerElement, sizeof(s->planes[i].bytesPerElement));
		p[0x1a] = s->planes[i].elementWidth;
		p[0x1b] = s->planes[i].elementHeight;
	}
	IOReturn ret = struct_out(args, out, DESC_SIZE);
	IOFreeData(out, DESC_SIZE);

	if (ret != kIOReturnSuccess && !existing) {
		IOLockLock(fLock);
		unsigned int index;
		if (findRef(s->id, &index) == ref) {
			fRefs->removeObject(index);
		}
		IOLockUnlock(fLock);
	}
	ref->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
	const void *in = args->structureInput;
	uint32_t len = args->structureInputSize;
	IOMemoryMap *inMap = NULL;

	if (args->structureInputDescriptor) {
		inMap = args->structureInputDescriptor->map();
		if (!inMap) {
			return kIOReturnNoMemory;
		}
		in = (const void *)inMap->getVirtualAddress();
		len = (uint32_t)inMap->getLength();
	}
	if (!in || len == 0 || len > MAX_PROPERTY_BYTES) {
		OSSafeReleaseNULL(inMap);
		return kIOReturnBadArgument;
	}
	OSObject *obj = OSUnserializeXML((const char *)in, len);
	OSSafeReleaseNULL(inMap);
	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (!props) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	uint32_t parent = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	TulipSurface *s = surface_create(props, parent);
	if (!s && tulip_surface_log) {
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("TulipSurface: %s create %ux%u format 0x%x failed, %llu MiB in use\n", name,
		    dict_u32(props, "IOSurfaceWidth", 0), dict_u32(props, "IOSurfaceHeight", 0),
		    dict_u32(props, "IOSurfacePixelFormat", 0), gSurfaceBytes >> 20);
	}
	obj->release();
	if (!s) {
		return kIOReturnNoMemory;
	}
	surface_use(s);
	s->retain();
	IOReturn ret = describe(s, args);
	surface_unuse(s);
	s->release();
	return ret;
}

static uint32_t
next_values_gen(TulipSurface *s)
{
	uint32_t gen = gSurfaceValuesGen[s->id] = (gSurfaceValuesGen[s->id] + 2) | 1;
	put32((uint8_t *)s->state->getBytesNoCopy(), STATE_VALUES_GEN, gen);
	put32((uint8_t *)s->state->getBytesNoCopy(), STATE_VALUES_SIZE, 1);
	return gen;
}

static void
gen_out(IOExternalMethodArguments *args, uint32_t gen)
{
	if (args->structureOutput && args->structureOutputSize >= sizeof(gen)) {
		memcpy(args->structureOutput, &gen, sizeof(gen));
		args->structureOutputSize = sizeof(gen);
	}
}

IOReturn
IOSurfaceRootUserClient::setValue(IOExternalMethodArguments *args)
{
	if (args->structureInputSize < 9) {
		return kIOReturnBadArgument;
	}
	uint32_t id;
	memcpy(&id, args->structureInput, sizeof(id));
	TulipSurface *s = heldSurface(id);
	if (!s) {
		return kIOReturnNotFound;
	}
	OSObject *obj = OSUnserializeXML((const char *)args->structureInput + 8, args->structureInputSize - 8);
	OSArray *pair = OSDynamicCast(OSArray, obj);
	OSDictionary *merge = OSDynamicCast(OSDictionary, obj);
	OSString *key = pair && pair->getCount() == 2 ? OSDynamicCast(OSString, pair->getObject(1)) : NULL;

	IOLockLock(gSurfaceLock);
	OSDictionary *values = gSurfaceValues[id];
	if (!values) {
		values = gSurfaceValues[id] = OSDictionary::withCapacity(4);
	}
	if (values && key) {
		values->setObject(key->getCStringNoCopy(), pair->getObject(0));
	} else if (values && merge) {
		values->merge(merge);
	}
	uint32_t gen = next_values_gen(s);
	IOLockUnlock(gSurfaceLock);
	OSSafeReleaseNULL(obj);
	s->release();
	gen_out(args, gen);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::removeValue(IOExternalMethodArguments *args)
{
	if (args->structureInputSize < 8) {
		return kIOReturnBadArgument;
	}
	uint32_t id;
	memcpy(&id, args->structureInput, sizeof(id));
	TulipSurface *s = heldSurface(id);
	if (!s) {
		return kIOReturnNotFound;
	}
	char key[128] = "";
	uint32_t keyLen = MIN(args->structureInputSize - 8, (uint32_t)sizeof(key) - 1);
	memcpy(key, (const char *)args->structureInput + 8, keyLen);
	key[keyLen] = '\0';

	IOLockLock(gSurfaceLock);
	if (gSurfaceValues[id]) {
		if (key[0]) {
			gSurfaceValues[id]->removeObject(key);
		} else {
			gSurfaceValues[id]->flushCollection();
		}
	}
	uint32_t gen = next_values_gen(s);
	IOLockUnlock(gSurfaceLock);
	s->release();
	gen_out(args, gen);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::copyValues(IOExternalMethodArguments *args)
{
	if (args->structureInputSize < 4) {
		return kIOReturnBadArgument;
	}
	uint32_t id;
	memcpy(&id, args->structureInput, sizeof(id));
	TulipSurface *s = heldSurface(id);
	if (!s) {
		return kIOReturnNotFound;
	}
	s->release();

	OSSerialize *ser = OSSerialize::binaryWithCapacity(256);
	if (!ser) {
		return kIOReturnNoMemory;
	}
	IOLockLock(gSurfaceLock);
	uint32_t gen = gSurfaceValuesGen[id];
	bool ok = gSurfaceValues[id] && gSurfaceValues[id]->serialize(ser);
	IOLockUnlock(gSurfaceLock);
	if (!ok) {
		OSDictionary *empty = OSDictionary::withCapacity(0);
		ok = empty && empty->serialize(ser);
		OSSafeReleaseNULL(empty);
	}
	if (!ok) {
		ser->release();
		return kIOReturnNoMemory;
	}
	uint32_t need = 8 + ser->getLength();
	uint8_t *out = (uint8_t *)args->structureOutput;
	if (!out || args->structureOutputSize < need) {
		if (out && args->structureOutputSize >= sizeof(need)) {
			memcpy(out, &need, sizeof(need));
		}
		ser->release();
		return RETURN_TOO_SMALL;
	}
	put32(out, 0, 0);
	put32(out, 4, gen);
	memcpy(out + 8, ser->text(), ser->getLength());
	args->structureOutputSize = need;
	ser->release();
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused, void *reference __unused)
{
	log_once(&gLoggedSelectors, "selector", selector);

	switch (selector) {
	case kSelLimits: {
		uint8_t limits[0x18] = {};
		uint16_t version = 1;
		put32(limits, 0x00, PAGE_SIZE - 1);
		put32(limits, 0x04, ROW_ALIGN - 1);
		memcpy(limits + 0x16, &version, sizeof(version));
		return struct_out(args, limits, sizeof(limits));
	}
	case kSelCreate:
		return create(args);
	case kSelLookup: {
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		TulipSurface *s = tulip_surface_copy((uint32_t)args->scalarInput[0]);
		return s ? describe(s, args) : kIOReturnNotFound;
	}
	case kSelLookupFromPort: {
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		OSObject *obj = iokit_lookup_object_with_port_name((mach_port_name_t)args->scalarInput[0],
		    IKOT_IOKIT_OBJECT, fTask);
		TulipSurface *s = OSDynamicCast(TulipSurface, obj);
		if (!s) {
			OSSafeReleaseNULL(obj);
			return kIOReturnNotFound;
		}
		return describe(s, args);
	}
	case kSelCreatePort: {
		if (args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		TulipSurface *s = heldSurface((uint32_t)args->scalarInput[0]);
		if (!s) {
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = tulip_surface_port(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		return kIOReturnSuccess;
	}
	case kSelSetValue:
		return setValue(args);
	case kSelCopyValues:
		return copyValues(args);
	case kSelRemoveValue:
		return removeValue(args);
	default:
		if (tulip_surface_log) {
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("TulipSurface: %s unhandled selector %u scalars %u/%u struct %u/%u\n", name, selector,
			    args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
		}
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		args->structureOutputSize = 0;
		args->structureOutputDescriptorSize = 0;
		return kIOReturnSuccess;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(void *, void *, void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

static IOReturn
seed_out(TulipSurface *s, void *seedOut, bool bump)
{
	volatile uint32_t *seed = (volatile uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + STATE_SEED);
	uint32_t value = bump ? os_atomic_inc(seed, relaxed) : os_atomic_load(seed, relaxed);

	if (seedOut && copyout(&value, (user_addr_t)seedOut, sizeof(value)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, void *, void *seedOut, void *, void *, void *)
{
	TulipSurface *s = heldSurface((uint32_t)(uintptr_t)id);
	if (!s) {
		return kIOReturnNotFound;
	}
	IOReturn ret = seed_out(s, seedOut, false);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *)
{
	TulipSurface *s = heldSurface((uint32_t)(uintptr_t)id);
	if (!s) {
		return kIOReturnNotFound;
	}
	IOReturn ret = seed_out(s, seedOut, !((uintptr_t)options & LOCK_READ_ONLY));
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	TulipSurfaceRef *gone = NULL;

	IOLockLock(fLock);
	TulipSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index);
	bool found = ref != NULL;
	if (ref && --ref->count == 0) {
		gone = ref;
		gone->retain();
		fRefs->removeObject(index);
	}
	IOLockUnlock(fLock);
	OSSafeReleaseNULL(gone);
	return found ? kIOReturnSuccess : kIOReturnNotFound;
}

/* Traps 0/1 use count, 2 lock and 3 unlock (id, options, seed out), 4/5 release. */
static IOExternalTrap gSurfaceTraps[] = {
	{ NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	{ NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	{ NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	{ NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	{ NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
	{ NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	log_once(&gLoggedTraps, "trap", index);
	if (index >= sizeof(gSurfaceTraps) / sizeof(gSurfaceTraps[0])) {
		return NULL;
	}
	*targetP = this;
	return &gSurfaceTraps[index];
}
