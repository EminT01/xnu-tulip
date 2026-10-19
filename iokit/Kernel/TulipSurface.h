/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_TULIPSURFACE_H
#define _IOKIT_TULIPSURFACE_H

#include <libkern/c++/OSObject.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#define TULIP_SURFACE_MAX_PLANES 8

struct TulipSurfacePlane {
	uint32_t width, height, bytesPerRow, offset, size;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
};

/* One IOSurface: wired pixel memory, its geometry and a shared state page. */
class TulipSurface : public OSObject
{
	OSDeclareDefaultStructors(TulipSurface);

public:
	uint32_t id;
	uint32_t parentID;
	uint32_t width, height, bytesPerRow, offset, pixelFormat, allocSize, cacheMode;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
	uint32_t planeCount;
	TulipSurfacePlane planes[TULIP_SURFACE_MAX_PLANES];

	IOBufferMemoryDescriptor *data;
	IOBufferMemoryDescriptor *state;

	virtual bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	virtual void free() APPLE_KEXT_OVERRIDE;

	/* Kernel address of the first pixel byte. */
	uint8_t *pixels() const;
};

/* The surface with this id, retained, or NULL. */
TulipSurface *tulip_surface_copy(uint32_t id);

#endif /* _IOKIT_TULIPSURFACE_H */
