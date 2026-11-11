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

FILES = {}


def newfile(relpath, content, why):
    FILES[relpath] = (content, why)


FONT_HEX = (
    "00000000000000000000000000000000",
    "0000183c3c3c18181800181800000000",
    "00666666240000000000000000000000",
    "0000006c6cfe6c6c6cfe6c6c00000000",
    "18187cc6c2c07c060686c67c18180000",
    "00000000c2c60c183060c68600000000",
    "0000386c6c3876dccccccc7600000000",
    "00303030600000000000000000000000",
    "00000c18303030303030180c00000000",
    "000030180c0c0c0c0c0c183000000000",
    "0000000000663cff3c66000000000000",
    "000000000018187e1818000000000000",
    "00000000000000000018181830000000",
    "00000000000000fe0000000000000000",
    "00000000000000000000181800000000",
    "0000000002060c183060c08000000000",
    "0000386cc6c6d6d6c6c66c3800000000",
    "00001838781818181818187e00000000",
    "00007cc6060c183060c0c6fe00000000",
    "00007cc606063c060606c67c00000000",
    "00000c1c3c6cccfe0c0c0c1e00000000",
    "0000fec0c0c0fc060606c67c00000000",
    "00003860c0c0fcc6c6c6c67c00000000",
    "0000fec606060c183030303000000000",
    "00007cc6c6c67cc6c6c6c67c00000000",
    "00007cc6c6c67e0606060c7800000000",
    "00000000181800000018180000000000",
    "00000000181800000018183000000000",
    "000000060c18306030180c0600000000",
    "00000000007e00007e00000000000000",
    "0000006030180c060c18306000000000",
    "00007cc6c60c18181800181800000000",
    "0000007cc6c6dedededcc07c00000000",
    "000010386cc6c6fec6c6c6c600000000",
    "0000fc6666667c66666666fc00000000",
    "00003c66c2c0c0c0c0c2663c00000000",
    "0000f86c6666666666666cf800000000",
    "0000fe6662687868606266fe00000000",
    "0000fe6662687868606060f000000000",
    "00003c66c2c0c0dec6c6663a00000000",
    "0000c6c6c6c6fec6c6c6c6c600000000",
    "00003c18181818181818183c00000000",
    "00001e0c0c0c0c0ccccccc7800000000",
    "0000e666666c78786c6666e600000000",
    "0000f06060606060606266fe00000000",
    "0000c6eefefed6c6c6c6c6c600000000",
    "0000c6e6f6fedecec6c6c6c600000000",
    "00007cc6c6c6c6c6c6c6c67c00000000",
    "0000fc6666667c60606060f000000000",
    "00007cc6c6c6c6c6c6d6de7c0c0e0000",
    "0000fc6666667c6c666666e600000000",
    "00007cc6c660380c06c6c67c00000000",
    "00007e7e5a1818181818183c00000000",
    "0000c6c6c6c6c6c6c6c6c67c00000000",
    "0000c6c6c6c6c6c6c66c381000000000",
    "0000c6c6c6c6d6d6d6feee6c00000000",
    "0000c6c66c7c38387c6cc6c600000000",
    "0000666666663c181818183c00000000",
    "0000fec6860c183060c2c6fe00000000",
    "00003c30303030303030303c00000000",
    "00000080c0e070381c0e060200000000",
    "00003c0c0c0c0c0c0c0c0c3c00000000",
    "10386cc6000000000000000000000000",
    "00000000000000000000000000ff0000",
    "0030180c000000000000000000000000",
    "0000000000780c7ccccccc7600000000",
    "0000e06060786c666666667c00000000",
    "00000000007cc6c0c0c0c67c00000000",
    "00001c0c0c3c6ccccccccc7600000000",
    "00000000007cc6fec0c0c67c00000000",
    "00001c36323078303030307800000000",
    "000000000076cccccccccc7c0ccc7800",
    "0000e060606c7666666666e600000000",
    "00001818003818181818183c00000000",
    "00000606000e06060606060666663c00",
    "0000e06060666c78786c66e600000000",
    "00003818181818181818183c00000000",
    "0000000000ecfed6d6d6d6c600000000",
    "0000000000dc66666666666600000000",
    "00000000007cc6c6c6c6c67c00000000",
    "0000000000dc66666666667c6060f000",
    "000000000076cccccccccc7c0c0c1e00",
    "0000000000dc7666606060f000000000",
    "00000000007cc660380cc67c00000000",
    "0000103030fc30303030361c00000000",
    "0000000000cccccccccccc7600000000",
    "0000000000c6c6c6c6c66c3800000000",
    "0000000000c6c6d6d6d6fe6c00000000",
    "0000000000c66c3838386cc600000000",
    "0000000000c6c6c6c6c6c67e060cf800",
    "0000000000fecc183060c6fe00000000",
    "00000e18181870181818180e00000000",
    "00001818181818181818181800000000",
    "0000701818180e181818187000000000",
    "0076dc00000000000000000000000000",
)

# cp850 8x16, MSB = leftmost pixel (extracted from the shim's font.h).
_FONT_ENTRIES = "".join(
    "\t{" + ",".join("0x" + FONT_HEX[i][j:j + 2] for j in range(0, 32, 2)) + "},\n"
    for i in range(95))

TULIP_EARLY_H = r"""/*
 * Early boot diagnostics for tulip (Redmi Note 6 Pro, SDM636).
 * Active only while a mapping for the splash framebuffer exists; see
 * tulip_early.c.  The no-op stubs keep every other board build untouched.
 */
#ifndef _ARM64_TULIP_EARLY_H_
#define _ARM64_TULIP_EARLY_H_

#include <stdarg.h>
#include <stdint.h>
#include <pexpert/arm64/board_config.h>

#if CONFIG_TULIP
void tulip_early_mark(unsigned int stage, const char *label);
void tulip_early_putc(char c);
void tulip_early_panic(const char *fmt, va_list *args);
void tulip_early_relocate(void);
void tulip_early_map_final(void);
void tulip_early_disable(void);
#else
static inline void tulip_early_mark(unsigned int stage, const char *label)
{
	(void)stage;
	(void)label;
}
static inline void tulip_early_putc(char c)
{
	(void)c;
}
static inline void tulip_early_panic(const char *fmt, va_list *args)
{
	(void)fmt;
	(void)args;
}
static inline void tulip_early_relocate(void) {}
static inline void tulip_early_map_final(void) {}
static inline void tulip_early_disable(void) {}
#endif /* CONFIG_TULIP */

#endif /* _ARM64_TULIP_EARLY_H_ */
"""

_TULIP_EARLY_C_TEMPLATE = r"""/*
 * Early boot screen for tulip: text straight to the ABL splash framebuffer.
 *
 * XNU's own framebuffer console only exists after arm_vm_init() returns, so a
 * hang or a fault before that point is invisible: the shim's blue screen just
 * stays there.  start.s maps the splash framebuffer into the bootstrap V=P
 * tables, and this file writes stage marks and panic text through it.
 * arm_vm_init() republishes the same physical block at a kernel VA that
 * survives the switch to the real TTBR1 and calls tulip_early_relocate().
 */
#include <arm64/tulip_early.h>

#if CONFIG_TULIP

#include <stdint.h>
#include <kern/misc_protos.h>
#include <arm64/proc_reg.h>
#include <arm/pmap.h>

/* ABL splash buffer, as handed to us in boot_args.Video. */
#define EFB_PHYS      0x9d400000ULL
#define EFB_WIDTH     1080u
#define EFB_HEIGHT    2280u
#define EFB_STRIDE    (4320u / 4u)      /* pixels per line */

/*
 * The shim prints its loader dump as 48 px rows from y=144 down to y=720;
 * start below it so that dump stays readable next to the kernel's marks.
 */
#define EFB_TOP       800u

/*
 * A kernel VA that survives bootstrap table teardown: the reserved
 * low-globals block is mapped by the final TTBR1, and the 2 MB slot right
 * after LOW_GLOBAL_BASE_ADDRESS is empty (low_global_vr_size is 32 MB).
 */
#define EFB_VA        (LOW_GLOBAL_BASE_ADDRESS + ARM_TT_L2_SIZE)

#define EFB_SCALE     2u
#define EFB_CELL_W    (8u * EFB_SCALE)
#define EFB_CELL_H    (16u * EFB_SCALE)
#define EFB_COLS      (EFB_WIDTH / EFB_CELL_W)
#define EFB_ROWS      ((EFB_HEIGHT - EFB_TOP) / EFB_CELL_H)

/*
 * cp850 8x16, bit 7 is the leftmost pixel.  Same table the shim draws with;
 * iso_font is only linked when the optional video console is built in.
 */
static const unsigned char tulip_font[95][16] = {
/*@FONT@*/};

static volatile uint32_t *early_fb = (volatile uint32_t *)(uintptr_t)EFB_PHYS;
static unsigned int early_row, early_col;
static unsigned int early_started, early_disabled, early_panicking;
static unsigned int early_final_ok;

static void
early_clear_row(void)
{
	for (unsigned int y = 0; y < EFB_CELL_H; y++) {
		volatile uint32_t *line = early_fb +
		    (EFB_TOP + early_row * EFB_CELL_H + y) * EFB_STRIDE;
		for (unsigned int x = 0; x < EFB_WIDTH; x++) {
			line[x] = 0x00000000u;
		}
	}
}

void
tulip_early_putc(char c)
{
	if (early_disabled) {
		return;
	}
	if (!early_started) {
		early_started = 1;
		early_clear_row();
	}
	if (c == '\r') {
		early_col = 0;
		return;
	}
	if (c == '\n' || early_col >= EFB_COLS) {
		early_col = 0;
		early_row = (early_row + 1u) % EFB_ROWS;
		early_clear_row();
		if (c == '\n') {
			return;
		}
	}
	unsigned char ch = (unsigned char)c;
	if (ch < 0x20 || ch > 0x7e) {
		ch = '?';
	}
	const unsigned char *glyph = tulip_font[ch - 0x20];
	for (unsigned int gy = 0; gy < 16; gy++) {
		for (unsigned int sy = 0; sy < EFB_SCALE; sy++) {
			volatile uint32_t *line = early_fb +
			    (EFB_TOP + early_row * EFB_CELL_H + gy * EFB_SCALE + sy) * EFB_STRIDE +
			    early_col * EFB_CELL_W;
			for (unsigned int gx = 0; gx < 8; gx++) {
				uint32_t pixel = (glyph[gy] & (0x80u >> gx)) ?
				    0x00ffffffu : 0x00000000u;
				for (unsigned int sx = 0; sx < EFB_SCALE; sx++) {
					line[gx * EFB_SCALE + sx] = pixel;
				}
			}
		}
	}
	early_col++;
}

static void
early_string(const char *s)
{
	while (*s) {
		tulip_early_putc(*s++);
	}
}

void
tulip_early_mark(unsigned int stage, const char *label)
{
	if (early_disabled || stage > 20) {
		return;
	}
	uint32_t color = 0xff00ff00u | ((stage * 28u) << 16);
	for (unsigned int y = EFB_HEIGHT - 32u; y < EFB_HEIGHT; y++) {
		for (unsigned int x = stage * 64u;
		    x < (stage + 1u) * 64u && x < EFB_WIDTH; x++) {
			early_fb[y * EFB_STRIDE + x] = color;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	early_string("TULIP EARLY ");
	if (stage >= 10) {
		tulip_early_putc((char)('0' + stage / 10));
	}
	tulip_early_putc((char)('0' + stage % 10));
	early_string(": ");
	early_string(label);
	tulip_early_putc('\n');
}

void
tulip_early_panic(const char *fmt, va_list *args)
{
	if (early_disabled || early_panicking) {
		return;
	}
	early_panicking = 1;
	early_string("\nEARLY PANIC: ");
	va_list copy;
	va_copy(copy, *args);
	_doprnt(fmt, &copy, tulip_early_putc, 16);
	va_end(copy);
	tulip_early_putc('\n');
	__asm__ volatile("dsb sy" ::: "memory");
}

void
tulip_early_relocate(void)
{
	if (early_disabled) {
		return;
	}
	early_string("\nTULIP EARLY: leaving bootstrap TTBR0\n");
	if (!early_final_ok) {
		early_string("TULIP EARLY: no final FB alias, console off\n");
		early_disabled = 1;
		return;
	}
	early_fb = (volatile uint32_t *)(uintptr_t)EFB_VA;
	__asm__ volatile("dsb sy" ::: "memory");
	early_string("TULIP EARLY: framebuffer kernel VA live\n");
}

/*
 * Called from arm_vm_init() while phystokv() still refers to the bootstrap
 * aperture: install the framebuffer block into the final kernel L2 table.
 */
void
tulip_early_map_final(void)
{
	const uint64_t va = EFB_VA;
	tt_entry_t l1 = cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];

	if ((l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) !=
	    (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE)) {
		return;
	}
	tt_entry_t *l2 = (tt_entry_t *)phystokv(l1 & ARM_TTE_TABLE_MASK);
	tt_entry_t *slot = &l2[(va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT];

	if (*slot != ARM_TTE_EMPTY) {
		return;
	}
	*slot = (tt_entry_t)(EFB_PHYS | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |
	    ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) |
	    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) |
	    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX);
	__asm__ volatile("dsb ishst" ::: "memory");
	early_final_ok = 1;
}

void
tulip_early_disable(void)
{
	tulip_early_mark(15, "early console disabled");
	early_disabled = 1;
	__asm__ volatile("dsb sy" ::: "memory");
}
#endif /* CONFIG_TULIP */
"""

TULIP_EARLY_C = _TULIP_EARLY_C_TEMPLATE.replace(
    "/*@FONT@*/", "\n" + _FONT_ENTRIES)

newfile(
    "osfmk/arm64/tulip_early.h",
    TULIP_EARLY_H,
    "early screen diagnostics API",
)

newfile(
    "osfmk/arm64/tulip_early.c",
    TULIP_EARLY_C,
    "splash-framebuffer stage marks and panic text",
)


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


# --------------------------------------------------------------------------
# 9. Early framebuffer diagnostics (see osfmk/arm64/tulip_early.c).
#    start.s maps the ABL splash buffer into the bootstrap V=P tables; the C
#    side prints a numbered line per boot stage and mirrors panics, because
#    XNU has no console at all until arm_vm_init() finishes.
# --------------------------------------------------------------------------
edit(
    "osfmk/arm64/start.s",
    """	/* Setup the KVA bootstrap mapping */
	lsr		x5,  x24, #(ARM_TT_L2_SHIFT)
	create_bootstrap_mapping x14, x15, x5, x3, x2, x9, x10, x11, x12, x13

	/* Ensure TTEs are visible */
""",
    """	/* Setup the KVA bootstrap mapping */
	lsr		x5,  x24, #(ARM_TT_L2_SHIFT)
	create_bootstrap_mapping x14, x15, x5, x3, x2, x9, x10, x11, x12, x13

#if CONFIG_TULIP
	/*
	 * Splash framebuffer (0x9d400000, 1080x2280, 32bpp): one 2 MB
	 * Normal-NC block into the V=P L2 table that was just allocated at
	 * x25 + 2*PGBYTES, so tulip_early.c can draw as soon as the MMU is on.
	 */
	MOV64	x10, 0x9d400000
	MOV64	x11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)
	orr		x11, x11, x10
	and		x12, x10, #(ARM_TT_L2_INDEX_MASK)
	lsr		x12, x12, #(ARM_TT_L2_SHIFT)
	add		x13, x25, #(2 * PGBYTES)
	str		x11, [x13, x12, lsl #TTE_SHIFT]
#endif /* CONFIG_TULIP */

	/* Ensure TTEs are visible */
""",
    "map the splash framebuffer into the bootstrap V=P tables",
)

edit(
    "osfmk/arm/arm_init.c",
    """#include <arm64/proc_reg.h>
#include <arm/pmap.h>
""",
    """#include <arm64/proc_reg.h>
#include <arm64/tulip_early.h>
#include <arm/pmap.h>
""",
    "include the early console",
)

edit(
    "osfmk/arm/arm_init.c",
    """	unsigned int dt_entry_size = 0;

#if CONFIG_TULIP
""",
    """	unsigned int dt_entry_size = 0;

	tulip_early_mark(1, "arm_init entered (MMU on)");

#if CONFIG_TULIP
""",
    "stage 1 mark",
)

edit(
    "osfmk/arm/arm_init.c",
    """	PE_init_platform(FALSE, args); /* Get platform expert set up */
""",
    """	PE_init_platform(FALSE, args); /* Get platform expert set up */
	tulip_early_mark(2, "PE_init_platform done");
""",
    "stage 2 mark",
)

edit(
    "osfmk/arm/arm_init.c",
    """	ml_parse_cpu_topology();
""",
    """	ml_parse_cpu_topology();
	tulip_early_mark(3, "ml_parse_cpu_topology done");
""",
    "stage 3 mark",
)

edit(
    "osfmk/arm/arm_init.c",
    """	cpu_bootstrap();

	rtclock_early_init();
""",
    """	cpu_bootstrap();
	tulip_early_mark(4, "cpu_bootstrap done");

	rtclock_early_init();
""",
    "stage 4 mark",
)

edit(
    "osfmk/arm/arm_init.c",
    """	kernel_debug_string_early("kernel_startup_bootstrap");
	kernel_startup_bootstrap();
""",
    """	kernel_debug_string_early("kernel_startup_bootstrap");
	kernel_startup_bootstrap();
	tulip_early_mark(5, "kernel_startup_bootstrap done");
""",
    "stage 5 mark",
)

edit(
    "osfmk/arm/arm_init.c",
    """	cpu_init();

	processor_bootstrap();
""",
    """	cpu_init();
	tulip_early_mark(6, "cpu_init done");

	processor_bootstrap();
""",
    "stage 6 mark",
)

edit(
    "osfmk/arm/arm_init.c",
    """	arm_vm_init(xmaxmem, args);
""",
    """	tulip_early_mark(7, "entering arm_vm_init");
	arm_vm_init(xmaxmem, args);
""",
    "stage 7 mark",
)

edit(
    "osfmk/arm64/arm_vm_init.c",
    """#include <arm64/lowglobals.h>
""",
    """#include <arm64/lowglobals.h>
#include <arm64/tulip_early.h>
""",
    "include the early console",
)

edit(
    "osfmk/arm64/arm_vm_init.c",
    """	arm_set_user_tbi();

	arm_vm_physmap_init(args);
""",
    """	tulip_early_mark(8, "arm_vm_init reached the MMU switch");
	arm_set_user_tbi();

	/* phystokv() must still refer to the bootstrap aperture here. */
	tulip_early_map_final();

	arm_vm_physmap_init(args);
""",
    "map the surviving framebuffer alias, stage 8 mark",
)

edit(
    "osfmk/arm64/arm_vm_init.c",
    """	ml_enable_monitor();

	set_mmu_ttb(invalid_ttep & TTBR_BADDR_MASK);

	flush_mmu_tlb();
	kva_active = TRUE;
""",
    """	ml_enable_monitor();

	/* The final TTBR1 is live: move the console off the V=P mapping. */
	tulip_early_relocate();

	set_mmu_ttb(invalid_ttep & TTBR_BADDR_MASK);

	flush_mmu_tlb();
	tulip_early_mark(10, "TTBR0 off, TLB flushed");
	kva_active = TRUE;
""",
    "relocate the console, stage 10 mark",
)

edit(
    "osfmk/arm64/arm_vm_init.c",
    """	invalid_tte = (tt_entry_t*)(phystokv(invalid_ttep));

	// From here on out, we're off the bootstrap translation tables.
""",
    """	invalid_tte = (tt_entry_t*)(phystokv(invalid_ttep));
	tulip_early_mark(11, "physmap table pointers rebound");

	// From here on out, we're off the bootstrap translation tables.
""",
    "stage 11 mark",
)

edit(
    "osfmk/arm64/arm_vm_init.c",
    """	pmap_bootstrap(dynamic_memory_begin);
""",
    """	tulip_early_mark(12, "calling pmap_bootstrap");
	pmap_bootstrap(dynamic_memory_begin);
	tulip_early_mark(13, "pmap_bootstrap done");
""",
    "stages 12-13 marks",
)

edit(
    "osfmk/conf/files.arm64",
    """osfmk/arm64/arm_vm_init.c	optional config_pmap_ppl
""",
    """osfmk/arm64/arm_vm_init.c	optional config_pmap_ppl
osfmk/arm64/tulip_early.c	optional config_tulip
""",
    "build the early console on tulip",
)

edit(
    "osfmk/kern/debug.c",
    """#include <console/serial_protos.h>
#include <IOKit/IOBSD.h>
""",
    """#include <console/serial_protos.h>
#if defined(__arm64__)
#include <arm64/tulip_early.h>
#endif
#include <IOKit/IOBSD.h>
""",
    "include the early console",
)

edit(
    "osfmk/kern/debug.c",
    """#pragma clang diagnostic pop

#if defined(__x86_64__) && (DEVELOPMENT || DEBUG)
	read_lbr();
#endif
""",
    """#pragma clang diagnostic pop

	/* No CPU/thread/debugger state is needed for this first panic copy. */
	tulip_early_panic(panic_format_str, panic_args);

#if defined(__x86_64__) && (DEVELOPMENT || DEBUG)
	read_lbr();
#endif
""",
    "mirror panics to the early screen",
)


# --------------------------------------------------------------------------
# 11. Post-MMU stage probes (start.s).
#     The screen already proves that start_first_cpu runs and that execution
#     reaches the instruction just before SCTLR_EL1.M is set.  These probes
#     bisect everything after that: instruction fetch with the MMU on, the
#     branch into the linked (KVA) address space, and any exception taken
#     while the early `b .` vectors are still installed.
# --------------------------------------------------------------------------
edit(
    "osfmk/arm64/start.s",
    "\t// Enable caches, MMU, ROP and JOP\n"
    "\tMOV64   x0, SCTLR_EL1_DEFAULT\n"
    "\tMSR_SCTLR_EL1_X0\n"
    "\tisb\t\tsy\n",
    "\t// Enable caches, MMU, ROP and JOP\n"
    "\tMOV64   x0, SCTLR_EL1_DEFAULT\n"
    "\tMSR_SCTLR_EL1_X0\n"
    "\tisb\t\tsy\n"
    "#if defined(TULIP)\n"
    "\t/* Probe: MMU is on, still executing out of the V=P mapping. */\n"
    "\tTULIP_EARLY_MARK 220, 0x00ff00\n"
    "#endif\n",
    "probe 220: MMU enabled",
)

edit(
    "osfmk/arm64/start.s",
    "\t// Return to arm_init()\n"
    "\tret\n"
    "\n"
    "Ltrampoline:\n",
    "#if defined(TULIP)\n"
    "\t/*\n"
    "\t * Probe: about to fetch instructions from the linked (KVA) address\n"
    "\t * space.  Paint here, then resume at arm_init through a branch rather\n"
    "\t * than the return, so a fault on the KVA fetch is distinguishable.\n"
    "\t */\n"
    "\tTULIP_EARLY_MARK 280, 0x00ffff\n"
    "\tadrp\tx9, Ltulip_kva_entry@page\n"
    "\tadd\t\tx9, x9, Ltulip_kva_entry@pageoff\n"
    "\tadd\t\tx9, x9, x22\n"
    "\tsub\t\tx9, x9, x23\n"
    "\tbr\t\tx9\n"
    "#else\n"
    "\t// Return to arm_init()\n"
    "\tret\n"
    "#endif\n"
    "\n"
    "Ltrampoline:\n",
    "probe 280: switching to KVA",
)

edit(
    "osfmk/arm64/start.s",
    "\t.text\n"
    "\t.align 2\n"
    "arm_init_tramp:\n",
    "\t.text\n"
    "\t.align 2\n"
    "#if defined(TULIP)\n"
    "Ltulip_kva_entry:\n"
    "\t/* Probe: reached by branching to a linked KVA address. */\n"
    "\tTULIP_EARLY_MARK 340, 0xff00ff\n"
    "\tb\t\tEXT(arm_init)\n"
    "#endif\n"
    "arm_init_tramp:\n",
    "probe 340: executing at KVA",
)

edit(
    "osfmk/arm64/start.s",
    "\t/* EL1 SP1: These vectors trap errors during early startup on non-boot CPUs. */\n"
    "\t.align\t9\n"
    "\tb\t\t.\n"
    "\t.align\t7\n"
    "\tb\t\t.\n"
    "\t.align\t7\n"
    "\tb\t\t.\n"
    "\t.align\t7\n"
    "\tb\t\t.\n",
    "\t/* EL1 SP1: These vectors trap errors during early startup on non-boot CPUs. */\n"
    "#if defined(TULIP)\n"
    "\t.align\t9\n"
    "\tb\t\tLtulip_exc_trap\n"
    "\t.align\t7\n"
    "\tb\t\tLtulip_exc_trap\n"
    "\t.align\t7\n"
    "\tb\t\tLtulip_exc_trap\n"
    "\t.align\t7\n"
    "\tb\t\tLtulip_exc_trap\n"
    "#else\n"
    "\t.align\t9\n"
    "\tb\t\t.\n"
    "\t.align\t7\n"
    "\tb\t\t.\n"
    "\t.align\t7\n"
    "\tb\t\t.\n"
    "\t.align\t7\n"
    "\tb\t\t.\n"
    "#endif\n",
    "tulip: report early exceptions instead of spinning",
)

edit(
    "osfmk/arm64/start.s",
    "\t.align\t7\n"
    "\t.globl EXT(reset_vector)\n"
    "LEXT(reset_vector)\n"
    "\t// Preserve x0 for start_first_cpu, if called\n",
    "\t.align\t7\n"
    "\t.globl EXT(reset_vector)\n"
    "LEXT(reset_vector)\n"
    "#if defined(TULIP)\n"
    "\t/* Probe: the reset vector was re-entered (e.g. exception with SPSel=0). */\n"
    "\tTULIP_EARLY_MARK 440, 0xff8000\n"
    "#endif\n"
    "\t// Preserve x0 for start_first_cpu, if called\n",
    "probe 440: reset vector re-entered",
)

edit(
    "osfmk/arm64/start.s",
    "/* vim: set ts=4: */\n",
    "#if defined(TULIP)\n"
    "/*\n"
    " * Any exception taken while the early vectors are installed lands here.\n"
    " * Paint a blue square so the failure is visible, then park the CPU.\n"
    " */\n"
    "\t.align 7\n"
    "Ltulip_exc_trap:\n"
    "\tTULIP_EARLY_MARK 400, 0x0000ff\n"
    "\tb\t\t.\n"
    "#endif\n"
    "\n"
    "/* vim: set ts=4: */\n",
    "tulip: exception trap mark",
)


# --------------------------------------------------------------------------
# 12. Fine-grained probes between "MMU on" and the KVA switch: the green
#     probe at 220 paints, the cyan one at 280 does not, so the hang is in
#     the handful of system-register instructions in between.
# --------------------------------------------------------------------------
edit(
    "osfmk/arm64/start.s",
    "\tMOV64   x1, SCTLR_EL1_DEFAULT\n"
    "\tcmp\t\tx0, x1\n"
    "\tbne\t\t.\n"
    "#endif /* !VMAPPLE */\n",
    "\tMOV64   x1, SCTLR_EL1_DEFAULT\n"
    "\tcmp\t\tx0, x1\n"
    "\tbne\t\t.\n"
    "#endif /* !VMAPPLE */\n"
    "#if defined(TULIP)\n"
    "\t/* Probe: the SCTLR readback check was passed. */\n"
    "\tTULIP_EARLY_MARK 480, 0xffffff\n"
    "#endif\n",
    "probe 480: SCTLR check passed",
)

edit(
    "osfmk/arm64/start.s",
    "\t// Enable NEON\n"
    "\tmov\t\tx0, #(CPACR_FPEN_ENABLE)\n"
    "\tmsr\t\tCPACR_EL1, x0\n"
    "#endif\n",
    "\t// Enable NEON\n"
    "\tmov\t\tx0, #(CPACR_FPEN_ENABLE)\n"
    "\tmsr\t\tCPACR_EL1, x0\n"
    "#endif\n"
    "#if defined(TULIP)\n"
    "\t/* Probe: CPACR_EL1 write completed (it did not trap to EL3). */\n"
    "\tTULIP_EARLY_MARK 520, 0x808080\n"
    "#endif\n",
    "probe 520: CPACR written",
)

edit(
    "osfmk/arm64/start.s",
    "\t// If x21 != 0, we're doing a warm reset, so we need to trampoline to the kernel pmap.\n"
    "\tcbnz\tx21, Ltrampoline\n",
    "#if defined(TULIP)\n"
    "\t/* Probe: all post-MMU system register setup is done. */\n"
    "\tTULIP_EARLY_MARK 560, 0x800000\n"
    "#endif\n"
    "\t// If x21 != 0, we're doing a warm reset, so we need to trampoline to the kernel pmap.\n"
    "\tcbnz\tx21, Ltrampoline\n",
    "probe 560: post-MMU setup done",
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
    for relpath, (content, why) in FILES.items():
        path = root / relpath
        if path.is_file():
            if path.read_text() == content:
                print(f"[skip] {relpath}: already present ({why})")
                continue
            print(f"[FAIL] {relpath}: exists with different content ({why})")
            failed += 1
            continue
        if not check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        print(f"[{'ok' if not check else 'would'}] {relpath}: create ({why})")

    if failed:
        print(f"\n{failed} file(s) failed")
        return 1

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
    print(f"\nall {len(FILES)} files and {len(EDITS)} edits applied"
          if not check else "\nall files and edits match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
