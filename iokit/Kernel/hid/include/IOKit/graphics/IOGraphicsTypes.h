/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

/* IOGraphicsFamily is not part of xnu; these are the two types the HID headers use, with its layouts. */
#ifndef _IOKIT_IOGRAPHICSTYPES_H
#define _IOKIT_IOGRAPHICSTYPES_H

#include <IOKit/IOTypes.h>

typedef struct IOGPoint {
	SInt16 x;
	SInt16 y;
} IOGPoint;

typedef struct IOGBounds {
	SInt16 minx;
	SInt16 maxx;
	SInt16 miny;
	SInt16 maxy;
} IOGBounds;

#endif
