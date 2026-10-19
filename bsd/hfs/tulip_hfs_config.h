/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _TULIP_HFS_CONFIG_H_
#define _TULIP_HFS_CONFIG_H_

#include <TargetConditionals.h>

#undef CONFIG_PROTECT
#undef CONFIG_SEARCHFS

#define HFS_COMPRESSION         1
#define FIFO                    1
#define QUOTA                   1
#define NAMEDSTREAMS            1
#define CONFIG_HFS_DIRLINK      1

#endif
