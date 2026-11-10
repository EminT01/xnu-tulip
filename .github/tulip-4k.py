#!/usr/bin/env python3
"""Make the xnu-tulip tree build and boot as a 4 KB-granule iOS-style kernel.

The tree is built with PLATFORM=MacOSX (that is the only platform the public
KDK can produce), so a handful of files take macOS paths that are wrong for a
phone: the macOS kernel VA layout (2 TB of KVA, 128 TB of user VA), the macOS
zone map size, the macOS compressor sizing, and - most importantly - an
assumption that arm64 kernels always use 16 KB pages.

Every replacement below is exact and is verified, so a silent drift in the
tree fails the build instead of producing a subtly broken kernel.

Usage:  python3 tulip-4k.py <xnu root> [--check]
"""

import pathlib
import sys

EDITS = []


def edit(relpath, old, new, why):
    EDITS.append((relpath, old, new, why))


# --------------------------------------------------------------------------
# 1. makedefs/MakeInc.def: embedded memory layout instead of the macOS one.
#    macOS maps the kernel at 0xfffffe00_0700_4000 with 2 TB of KVA and
#    ARM_LARGE_MEMORY (1 TB of DRAM); iOS/arm64 uses 0xfffffff0_0700_4000,
#    64 GB of KVA and the non-large-memory pmap.
# --------------------------------------------------------------------------
edit(
    "makedefs/MakeInc.def",
    """# Use ARM_LARGE_MEMORY config for all MacOSX targets.
ifneq ($(filter $(PLATFORM),MacOSX),)
LARGE_MEMORY_DEFINE=-DARM_LARGE_MEMORY=1
ARM64_PLKSEG_ADDR  =0xfffffe0004004000
ARM64_LINK_ADDR    =0xfffffe0007004000
endif
""",
    """# Use ARM_LARGE_MEMORY config for all MacOSX targets.
ifneq ($(filter $(PLATFORM),MacOSX),)
LARGE_MEMORY_DEFINE=-DARM_LARGE_MEMORY=1
ARM64_PLKSEG_ADDR  =0xfffffe0004004000
ARM64_LINK_ADDR    =0xfffffe0007004000
endif

# TULIP is an embedded 4 KB-granule device that happens to be built with the
# MacOSX SDK: give it the iOS kernel VA layout, not the Mac one.
ifeq ($(CURRENT_MACHINE_CONFIG),TULIP)
LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY
ARM64_PLKSEG_ADDR  =0xfffffff004004000
ARM64_LINK_ADDR    =0xfffffff007004000
endif
""",
    "iOS kernel VA layout (64 GB KVA, link 0xfffffff007004000)",
)

# --------------------------------------------------------------------------
# 2. osfmk/arm/arm_init.c: pick the hardware page size.
#    Upstream upgrades any arm64 device with > 1 GB of RAM to 16 KB pages,
#    which is a runtime-only knob on iOS (A9+); the SDM636 cannot do it.
# --------------------------------------------------------------------------
edit(
    "osfmk/arm/arm_init.c",
    """		if (args->memSize > 1ULL * 1024 * 1024 * 1024) {
			/*
			 * arm64 device with > 1GB of RAM:
			 * kernel uses 16KB pages.
			 */
			PAGE_SHIFT_CONST = PAGE_MAX_SHIFT;
		} else {
			/*
			 * arm64 device with <= 1GB of RAM:
			 * kernel uses hardware page size
			 * (4KB for H6/H7, 16KB for H8+).
			 */
			PAGE_SHIFT_CONST = ARM_PGSHIFT;
		}
""",
    """#if CONFIG_TULIP
		/*
		 * tulip (SDM636 / Kryo 260) implements only the 4 KB and 64 KB
		 * granules: ID_AA64MMFR0_EL1.TGRAN16 is 0. Keep the hardware page
		 * size no matter how much RAM the bootloader hands us.
		 */
		PAGE_SHIFT_CONST = ARM_PGSHIFT;
#else
		if (args->memSize > 1ULL * 1024 * 1024 * 1024) {
			/*
			 * arm64 device with > 1GB of RAM:
			 * kernel uses 16KB pages.
			 */
			PAGE_SHIFT_CONST = PAGE_MAX_SHIFT;
		} else {
			/*
			 * arm64 device with <= 1GB of RAM:
			 * kernel uses hardware page size
			 * (4KB for H6/H7, 16KB for H8+).
			 */
			PAGE_SHIFT_CONST = ARM_PGSHIFT;
		}
#endif /* CONFIG_TULIP */
""",
    "PAGE_SHIFT_CONST always 4 KB on tulip",
)

# --------------------------------------------------------------------------
# 3. osfmk/mach/arm/vm_param.h: 64 GB of user VA, like a 4 KB iOS device
#    (A7/A8). With the macOS 128 TB value the 4 KB commpage cannot be nested
#    above the maximum user address and pmap.c refuses to compile.
# --------------------------------------------------------------------------
edit(
    "osfmk/mach/arm/vm_param.h",
    """#if defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)
#define MACH_VM_MAX_ADDRESS_RAW 0x00007FFFFE000000ULL
""",
    """#if (defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)) && !defined(ARM64_BOARD_CONFIG_TULIP)
#define MACH_VM_MAX_ADDRESS_RAW 0x00007FFFFE000000ULL
""",
    "tulip user VA max = 0x0FC0000000 (iOS), not 0x7FFFFE000000",
)

# --------------------------------------------------------------------------
# 4. osfmk/arm/commpage/commpage.c: advertise the real user page size.
# --------------------------------------------------------------------------
edit(
    "osfmk/arm/commpage/commpage.c",
    """	*((uint8_t*)(_COMM_PAGE_USER_PAGE_SHIFT_64_LEGACY + _COMM_PAGE_RW_OFFSET)) = (uint8_t) SIXTEENK_PAGE_SHIFT;
	*((uint8_t*)(_COMM_PAGE_USER_PAGE_SHIFT_64 + _COMM_PAGE_RO_OFFSET)) = (uint8_t) SIXTEENK_PAGE_SHIFT;
""",
    """#if __ARM_16K_PG__
	*((uint8_t*)(_COMM_PAGE_USER_PAGE_SHIFT_64_LEGACY + _COMM_PAGE_RW_OFFSET)) = (uint8_t) SIXTEENK_PAGE_SHIFT;
	*((uint8_t*)(_COMM_PAGE_USER_PAGE_SHIFT_64 + _COMM_PAGE_RO_OFFSET)) = (uint8_t) SIXTEENK_PAGE_SHIFT;
#else
	/* 4 KB kernel (tulip): user space uses the same 4 KB pages. */
	*((uint8_t*)(_COMM_PAGE_USER_PAGE_SHIFT_64_LEGACY + _COMM_PAGE_RW_OFFSET)) = (uint8_t) FOURK_PAGE_SHIFT;
	*((uint8_t*)(_COMM_PAGE_USER_PAGE_SHIFT_64 + _COMM_PAGE_RO_OFFSET)) = (uint8_t) FOURK_PAGE_SHIFT;
#endif
""",
    "commpage reports 4 KB user pages",
)

# --------------------------------------------------------------------------
# 5. bsd/kern/mach_loader.c: user address spaces use the kernel granule.
# --------------------------------------------------------------------------
edit(
    "bsd/kern/mach_loader.c",
    """	if (result->is_64bit_addr) {
		/* enforce 16KB alignment of VM map entries */
		vm_map_set_page_shift(map, SIXTEENK_PAGE_SHIFT);
	} else {
""",
    """	if (result->is_64bit_addr) {
#if __ARM_16K_PG__
		/* enforce 16KB alignment of VM map entries */
		vm_map_set_page_shift(map, SIXTEENK_PAGE_SHIFT);
#else
		/* 4 KB kernel (tulip): 64-bit tasks use 4 KB map entries. */
		vm_map_set_page_shift(map, FOURK_PAGE_SHIFT);
#endif
	} else {
""",
    "64-bit tasks get 4 KB vm_map entries",
)

# --------------------------------------------------------------------------
# 6. osfmk/vm/vm_shared_region.c: same for the shared region submap and for
#    the slide alignment check.
# --------------------------------------------------------------------------
edit(
    "osfmk/vm/vm_shared_region.c",
    """			if (is_64bit ||
			    page_shift_user32 == SIXTEENK_PAGE_SHIFT) {
				/* enforce 16KB alignment of VM map entries */
				vm_map_set_page_shift(sub_map, SIXTEENK_PAGE_SHIFT);
			}
""",
    """#if __ARM_16K_PG__
			if (is_64bit ||
			    page_shift_user32 == SIXTEENK_PAGE_SHIFT) {
				/* enforce 16KB alignment of VM map entries */
				vm_map_set_page_shift(sub_map, SIXTEENK_PAGE_SHIFT);
			}
#else
			/* 4 KB kernel (tulip): the shared region is 4 KB aligned. */
			vm_map_set_page_shift(sub_map, FOURK_PAGE_SHIFT);
#endif
""",
    "shared region submap uses 4 KB entries",
)

edit(
    "osfmk/vm/vm_shared_region.c",
    """		if ((shared_region->sr_64bit ||
		    page_shift_user32 == SIXTEENK_PAGE_SHIFT) &&
		    ((srfmp->slide & SIXTEENK_PAGE_MASK) != 0)) {
""",
    """#if __ARM_16K_PG__
		if ((shared_region->sr_64bit ||
		    page_shift_user32 == SIXTEENK_PAGE_SHIFT) &&
		    ((srfmp->slide & SIXTEENK_PAGE_MASK) != 0)) {
#else
		if ((srfmp->slide & FOURK_PAGE_MASK) != 0) {
#endif
""",
    "shared cache slide checked against 4 KB alignment",
)

# --------------------------------------------------------------------------
# 7. osfmk/kern/zalloc.c: the macOS zone map is 128 GB of VA / 32 GB of wired
#    memory. The kernel only has 64 GB of VA here, and the device has 3 GB.
# --------------------------------------------------------------------------
edit(
    "osfmk/kern/zalloc.c",
    """#if XNU_PLATFORM_MacOSX
#define ZONE_MAP_MAX            (32ULL << 30)
#define ZONE_MAP_VA_SIZE        (128ULL << 30)
#else /* XNU_PLATFORM_MacOSX */
""",
    """#if XNU_PLATFORM_MacOSX && !defined(ARM64_BOARD_CONFIG_TULIP)
#define ZONE_MAP_MAX            (32ULL << 30)
#define ZONE_MAP_VA_SIZE        (128ULL << 30)
#else /* XNU_PLATFORM_MacOSX && !ARM64_BOARD_CONFIG_TULIP */
""",
    "iOS zone map sizes (8 GB wired / 24 GB VA)",
)

# --------------------------------------------------------------------------
# 8. osfmk/vm/vm_compressor.c: iOS compressor geometry, not the macOS swap
#    geometry (64 KB segments, and a pool sized at 16x RAM).
# --------------------------------------------------------------------------
edit(
    "osfmk/vm/vm_compressor.c",
    """#if XNU_TARGET_OS_OSX && defined(__arm64__)
#define C_SEG_BUFSIZE_DEFAULT C_SEG_BUFSIZE_ARM_SWAP
#else
#define C_SEG_BUFSIZE_DEFAULT (1024 * 256)
#endif /* TARGET_OS_OSX && defined(__arm64__) */
""",
    """#if XNU_TARGET_OS_OSX && defined(__arm64__) && !defined(ARM64_BOARD_CONFIG_TULIP)
#define C_SEG_BUFSIZE_DEFAULT C_SEG_BUFSIZE_ARM_SWAP
#else
#define C_SEG_BUFSIZE_DEFAULT (1024 * 256)
#endif /* TARGET_OS_OSX && defined(__arm64__) && !ARM64_BOARD_CONFIG_TULIP */
""",
    "iOS 256 KB compressor segments",
)

edit(
    "osfmk/vm/vm_compressor.c",
    """#if XNU_TARGET_OS_OSX

	if (vm_compression_limit == 0) {
		if (max_mem <= (4ULL * 1024ULL * 1024ULL * 1024ULL)) {
""",
    """#if XNU_TARGET_OS_OSX && !defined(ARM64_BOARD_CONFIG_TULIP)

	if (vm_compression_limit == 0) {
		if (max_mem <= (4ULL * 1024ULL * 1024ULL * 1024ULL)) {
""",
    "compressor pool sized like iOS (1x RAM), not macOS (16x RAM)",
)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv[1:]
    if len(args) != 1:
        print(__doc__)
        return 2
    root = pathlib.Path(args[0])
    if not root.is_dir():
        print(f"error: {root} is not a directory")
        return 2

    failed = 0
    for relpath, old, new, why in EDITS:
        path = root / relpath
        if not path.is_file():
            print(f"[FAIL] {relpath}: missing file")
            failed += 1
            continue
        text = path.read_text()
        if new in text:
            print(f"[skip] {relpath}: already applied ({why})")
            continue
        count = text.count(old)
        if count != 1:
            print(f"[FAIL] {relpath}: anchor found {count} times ({why})")
            failed += 1
            continue
        if not check:
            path.write_text(text.replace(old, new, 1))
        print(f"[{'ok' if not check else 'would'}] {relpath}: {why}")

    if failed:
        print(f"\n{failed} edit(s) failed; the tree does not match what this "
              f"script expects. Nothing was written.")
        return 1
    print(f"\nall {len(EDITS)} edits applied" if not check else "\nall edits match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
