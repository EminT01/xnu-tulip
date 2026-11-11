#!/usr/bin/env python3
"""Build the Apple Device Tree (ADT) for Xiaomi Redmi Note 6 Pro (tulip) Qualcomm SDM636.

Derived from ios-vince/tools/mkadt.py. Hardware values come from the E7T DTB inside tulip's boot.img
(msm-id 0x159, board-id 0x40008, Linux 4.4.153). Values marked VERIFY must be checked on the real phone.
Kernel-facing names (node 'vince-fbcon', device_type 'vince-io', target-type 'vince') are kept on purpose,
as the daisy branch does: the XNU patches still look those up until a TULIP machine config exists.

- GICv3: GICD @ 0x17a00000, GICR @ 0x17b00000 (8 x 0x20000), virtual-timer-intid 19; node 'gic', reg relative to SOC_BASE.
  Needs the TULIP machine config (TULIP.h): the kernel must take the sweet GICv3 path, not vince's GICv2 one.
- Framebuffer: splash_region @ 0x9d400000 (36 MiB), 1080x2280. Android's fb0 is 32 bpp, stride 4352 (1088 px),
  but ABL's own scanout buffer is not known yet. VERIFY by reading the MDP SSPP registers at shim start (see FB notes).
- Physical RAM: 0xa2000000..0xf4000000 inside the 94b00000-feabffff bank (checked with /proc/iomem, 3 GB model)
- SDHCI eMMC (SDCC5, qcom,sdhci-msm-v5) @ 0x0c0c4000, SPI 110
- Touch: Novatek NVT (nt36672a Tianma E7T, PID 590B) @ 0x62 on i2c-1 (0xc175000), irq GPIO 67, reset GPIO 66;
  the FocalTech probe at 0x38 fails on this unit (dmesg: 'not focal IC'). Disabled for first boot.
- CPUs: Kryo 260, cpu0-3 = efficiency (A53-class), cpu4-7 = performance (A73-class), PSCI/smc. First boot: cpu0 only.
"""
import hashlib
import os
import struct
import sys
import argparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "upstream", "ios-sweet", "tools"))
import adt  # noqa: E402

# Tulip (SDM636) Hardware Definitions:
# /proc/iomem (OrangeFox, 3 GB unit): System RAM 40000000-855fffff, 88f00000-8abfffff, 94b00000-feabffff.
# Fixed reservations in the DTB: splash 0x9d400000 (36 MiB), pstore 0xa0000000 (4 MiB), modem/adsp/wlan below 0x94b00000.
# Linux itself carves CMA pools near the top (0xf4400000-0xfe800000); we stay below them.
# mkpayload_vince.py maps the kernel at phys_base + (va - virt_base) with virt_base 32 MiB aligned, so keep
# PHYS_BASE 32 MiB aligned too (0xa2000000 is the first such address above the pstore region).
PHYS_BASE = 0xa2000000
MEM_SIZE = 0xf4000000 - PHYS_BASE  # 0x52000000 = 1.28 GiB contiguous, ends at 0xf4000000

PANIC_LOG_SIZE = 0x100000  # 1 MB panic log buffer

# Non-zero SOC base for arm-io so pe_arm_get_soc_base_phys() returns non-zero
SOC_BASE = 0x01000000

# GICv3 (SDM660 family) MMIO addresses, from /soc/interrupt-controller@17a00000 in the tulip DTB
GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17b00000, 0x100000

# Framebuffer: continuous splash left by ABL (dmesg: "bootloader display is on", mdss version 0x30020000, DSI0 video panel).
# DTB: splash_region@9d400000 size 0x2400000 (36 MiB). Android's fb0 reports 32 bpp, virtual 1080x4560, stride 4352,
# but that is Linux's own double buffer, not ABL's. The bootloader's real scanout address/stride/format are in the MDP
# SSPP registers (MDP base 0x0c900000; VIG0 +0x5000, VIG1 +0x7000, DMA0 +0x25000, DMA1 +0x27000, DMA2 +0x29000;
# SRC0_ADDR +0x14, SRC_YSTRIDE0 +0x24, SRC_FORMAT +0x30). Read them in the shim before anything touches the display and
# patch this node at runtime. Until verified, assume 32 bpp with no row padding.
FB = 0x9d400000
FB_W, FB_H, FB_STRIDE = 1080, 2280, 1080 * 4
FB_SIZE = FB_STRIDE * FB_H  # the console maps reg[1] bytes; it must cover stride * height
FB_BPP = 32  # VERIFY (vince used 24 bpp RGB888 from LK; tulip boots through UEFI ABL)

# Storage: Qualcomm SDCC5 eMMC host controller (hc_mem 0x0c0c4000, cmdq 0x0c0c5000 in the DTB).
# Unlike vince (SDCC4, standard regs at +0x900) the SDHCI register block starts at the base here.
SDHCI, SDHCI_SIZE = 0x0c0c4000, 0x1000
SDHCI_SPI = 110  # DTB interrupts = <GIC_SPI 110 0>, <GIC_SPI 112 0> (hc_irq, pwr_irq)

TIMEBASE = 19200000  # 19.2 MHz Qualcomm XO clock
VTIMER_INTID = 19    # Virtual timer: DTB lists PPIs 1,2,3,0 (sec-phys, phys, virt, hyp) -> virt = PPI 3 = 16 + 3 (VERIFY)

VINCE_ECID = 0x56494e4345000001  # 'VINCE' + 1
BOOT_MANIFEST_HASH = hashlib.sha384(b'vince boot manifest').digest()

D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')

def d22ap_product(path=D22AP_ADT):
    """Fallback or extract /product properties from iPhone X device tree."""
    if os.path.exists(path):
        root, _ = adt.parse(open(path, 'rb').read())
        for c in root.children:
            if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
                return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    return None

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'

def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n

def build_tulip_adt(seed=None):
    phys_base = PHYS_BASE
    mem_size = MEM_SIZE

    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE

    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('display-scale', u32(3)),
        ('display-rotation', u32(0)),
        ('unique-chip-id', u64(VINCE_ECID)),
        ('chip-id', u32(0x8015)),  # Apple A11 Bionic (iPhone X)
        ('board-id', u32(0x16)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('vince-shim')),
        ('system-firmware-version', cstr('vince-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [
        node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])
    ])

    # GICv3 node as the sweet kernel expects it (pe_fiq.c, HAS_GIC_V3 path): named 'gic', compatible arm,gic-v3,
    # reg = <GICD size GICR size> RELATIVE to the arm-io base (SOC_BASE), because pe_fiq.c adds
    # pe_arm_get_soc_base_phys() itself. GICR covers 8 redistributors of GICR_PE_SIZE (0x20000) each.
    # The vince GICv2 node ('interrupt-controller', absolute GICD/GICC) is not used on tulip.
    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE)),
    ])

    # Watchdog node with no-pmu flag to prevent watchdogd panic
    wdt = node('wdt', [
        ph(),
        ('no-pmu', u32(1)),
    ])

    # Display subsystem node for backboardd / IOMobileFramebuffer
    disp0 = node('disp0', [
        ph(),
        ('device_type', cstr('display-subsystem')),
        ('compatible', cstr('vince,display') + cstr('sweet,display') + b'\0'),
        # IOMobileFramebuffer reads the canvas from this node: the iPhone X size AppleCLCD presents (1125x2436,
        # 375x812 points @3x; the panel shows the centered 1080x2160 part), as sweet's ADT does.
        ('canvas-width', u32(1125)),
        ('canvas-height', u32(2436)),
    ])

    # SDHCI eMMC Storage Node
    sdhci_node = node('sdhci', [
        ph(),
        ('compatible', cstr('generic-sdhci') + cstr('qcom,sdhci-msm-v5') + b'\0'),
        ('reg', u64(SDHCI, SDHCI_SIZE)),
        ('interrupts', u32(0, SDHCI_SPI, 4)),  # SPI 110
    ])

    # Touchscreen: Novatek NVT (nt36672a, Tianma E7T panel, PID 590B) at 0x62 on i2c@0xc175000 (Linux i2c-1, BLSP1 QUP1).
    # irq GPIO 67 (falling edge), reset GPIO 66. The FocalTech FTS probe at 0x38 fails on this unit. Keep it off on
    # first boot (cmdline vincetouch=0); no XNU driver exists for it yet.
    touch_node = node('touchscreen', [
        ph(),
        ('compatible', cstr('novatek,NVT-ts') + b'\0'),
        ('i2c-address', u32(0x62)),
        ('i2c-bus', u32(1)),
        ('i2c-controller-base', u64(0x0c175000)),
        ('reset-gpio', u32(66)),
        ('interrupts', u32(0, 67, 2)),  # GPIO 67, falling edge
    ])

    # No Wi-Fi or Bluetooth driver: MobileGestalt's WifiAddress/BluetoothAddress (for the A11-style UniqueDeviceID)
    # look up IONameMatch wlan / bluetooth and read local-mac-address; with no such service it waits for an
    # IONetworkController, times out and retries forever (lsd's database seed hangs in GenerateBuiltInAppInstallSessionID,
    # SpringBoard and backboardd wait on lsd). Stub nubs with synthetic locally administered addresses, not the phone's.
    wlan = node('wlan', [
        ph(),
        ('device_type', cstr('wlan')),
        ('compatible', cstr('vince,wlan-stub')),
        ('local-mac-address', bytes([0x02, 0x56, 0x49, 0x4e, 0x43, 0x01])),
    ])
    bluetooth = node('bluetooth', [
        ph(),
        ('device_type', cstr('bluetooth')),
        ('compatible', cstr('vince,bluetooth-stub')),
        ('local-mac-address', bytes([0x02, 0x56, 0x49, 0x4e, 0x43, 0x02])),
    ])

    # arm-io with device_type and ranges so pe_arm_get_soc_base_phys() succeeds
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('vince-io')),
        ('compatible', cstr('vince-io') + cstr('arm-io') + b'\0'),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic, wdt, disp0, sdhci_node, touch_node, wlan, bluetooth])

    # Vince Framebuffer console node for early kprintf
    fbcon = node('vince-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
        ('bpp', u32(FB_BPP)),
    ])

    # System defaults
    defaults = node('defaults', [
        ph(),
        ('no-effaceable-storage', u32(1)),
    ])

    # Buttons: Home button type 2 = iPhone X style (gestures, no physical home button)
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    # CPUs: Kryo 260 x8 in two clusters (MPIDR 0x0-0x3 efficiency/A53-class, 0x100-0x103 performance/A73-class;
    # DTB efficiency values 0x400 / 0x666). PSCI 1.0 over SMC. First boot: cpu0 only. Add cores to CPU_LIST once
    # single-core boot works; the other cores are started through PSCI CPU_ON.
    CPU_LIST = (0,)   # later: (0, 1, 2, 3, 4, 5, 6, 7)  (vince left CPU1 for the shim's USB monitor; check if tulip needs that)
    cpu_nodes = []
    for core in CPU_LIST:
        perf = core >= 4
        c = node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo260') + cstr('ARM,v8') + b'\0'),
            ('reg', u32(core if core < 4 else 0x100 + core - 4)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(1 if perf else 0)),
            ('cluster-type', u32(1 if perf else 0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core % 4)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(1800000000 if perf else 1600000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
            ('cpu-model', cstr('kryo260-gold' if perf else 'kryo260-silver')),
        ])
        cpu_nodes.append(c)

    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    # Product identity: iPhone X (iPhone10,3 / D22AP)
    product_props = d22ap_product()
    if product_props:
        product = [node('product', [ph()] + product_props)]
    else:
        product = [node('product', [
            ph(),
            ('product-id', u32(0x1003)),
            ('product-name', cstr('iPhone X')),
            ('model-number', cstr('D22AP')),
            ('device-class', cstr('iPhone')),
        ])]

    # Panic log region: /pram reg = <panic_base, PANIC_LOG_SIZE>
    pram = node('pram', [ph(), ('reg', u64(panic_base, PANIC_LOG_SIZE))])

    root = node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,vince') + cstr('xiaomi,tulip') + cstr('qcom,sdm636') + cstr('AppleARM') + b'\0'),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('vince')),
        ('target-sub-type', cstr('vince')),
        ('manufacturer', cstr('Apple Inc.')),
        ('device-id', u64(VINCE_ECID)),
        ('timebase-frequency', u32(TIMEBASE)),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product)

    return root

def main():
    parser = argparse.ArgumentParser(description="Generate Tulip (SDM636) Apple Device Tree")
    parser.add_argument("output", help="Output .adt file path")
    parser.add_argument("--dump", action="store_true", help="Print human readable tree")
    args = parser.parse_args()

    root = build_tulip_adt()
    data = adt.serialize(root)
    with open(args.output, "wb") as f:
        f.write(data)

    print(f"[+] Tulip ADT successfully generated: {args.output} ({len(data)} bytes)")
    if args.dump:
        adt.dump(adt.parse(data)[0])

if __name__ == "__main__":
    main()
