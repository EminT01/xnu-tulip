#!/usr/bin/env python3
"""Verification of the tulip (Redmi Note 6 Pro / SDM636) ADT.

Replaces verify_adt_vince.py for the tulip build: that script asserts the MSM8953 values (GICv2, SDHCI 0x07824900,
FB 0x90001000) and would fail by design. Note that tulip's GICD (0x17a00000) legitimately equals sweet's, so there
is no "sweet GICD trap" here; the GIC node must be GICv3 and its reg relative to the arm-io base.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Hardware facts, from the tulip DTB / dmesg / iomem (see tulip_hw_map.md)
GICD, GICR = 0x17a00000, 0x17b00000
SDHCI = 0x0c0c4000
FB, FB_W, FB_H = 0x9d400000, 1080, 2280
# highest fixed carve-out below the splash/pstore: pstore ends at 0xa0400000
MIN_DRAM_BASE = 0xa0400000
MAX_DRAM_END = 0xfeac0000   # end of System RAM bank 94b00000-feabffff
SWEET_TRAPS = {"sweet_fb": 0x9c000000, "sweet_ufs": 0x01d84000, "sweet_gicr": 0x17a60000, "vince_gicd": 0x0b000000}


def props(n):
    return {k: v for k, _, v in n.props}


def nm(n):
    return n.name


def find(root, name):
    for c in root.children:
        if nm(c) == name:
            return c
        r = find(c, name)
        if r:
            return r
    return None


def verify(path):
    print(f"[*] Verifying tulip ADT: {path}")
    data = open(path, "rb").read()
    root, end = adt.parse(data)
    assert end == len(data), f"trailing bytes: parsed {end} of {len(data)}"
    rp = props(root)

    tt = rp["target-type"].rstrip(b"\0").decode()
    assert tt == "vince", f"target-type {tt!r}: the XNU patches still match 'vince' (see mkadt_tulip.py header)"
    comp = rp["compatible"]
    assert b"xiaomi,tulip" in comp and b"qcom,sdm636" in comp, "compatible lacks tulip/sdm636"
    print("  [PASS] identity: D22AP/iPhone10,3 + xiaomi,tulip + qcom,sdm636, target-type vince")

    chosen, arm_io = find(root, "chosen"), find(root, "arm-io")
    assert chosen and arm_io, "chosen/arm-io missing"
    cp = props(chosen)
    base, = struct.unpack("<Q", cp["dram-base"])
    size, = struct.unpack("<Q", cp["dram-size"])
    plog, = struct.unpack("<I", cp["embedded-panic-log-size"])
    assert base >= MIN_DRAM_BASE, f"DRAM base 0x{base:x} overlaps splash/pstore carve-outs"
    assert base % 0x2000000 == 0, f"DRAM base 0x{base:x} is not 32 MiB aligned (mkpayload kernel mapping)"
    assert base + size + plog <= MAX_DRAM_END, "DRAM range runs past the end of System RAM"
    pram = struct.unpack("<2Q", props(find(root, "pram"))["reg"])
    assert pram[0] == base + size and pram[1] == plog, "pram not directly above dram"
    print(f"  [PASS] DRAM 0x{base:x} + 0x{size:x}, panic log 0x{pram[0]:x} + 0x{pram[1]:x}")

    soc_base = struct.unpack("<3Q", props(arm_io)["ranges"])[1]
    assert soc_base != 0, "arm-io ranges SOC base is 0 (serial_init bails)"

    gic = find(arm_io, "gic")
    assert gic is not None, "gic node missing (the GICv3 path looks for 'gic')"
    assert find(arm_io, "interrupt-controller") is None, "vince GICv2 node must not be present"
    gp = props(gic)
    assert gp["compatible"].rstrip(b"\0") == b"arm,gic-v3", "gic is not arm,gic-v3"
    reg = struct.unpack("<4Q", gp["reg"])
    assert reg[0] + soc_base == GICD and reg[2] + soc_base == GICR, f"GIC reg (relative) {[hex(x) for x in reg]} wrong"
    assert reg[3] == 8 * 0x20000, "GICR size must cover 8 redistributors"
    intid, = struct.unpack("<I", gp["virtual-timer-intid"])
    assert 16 <= intid < 32, f"virtual timer INTID {intid} is not a PPI"
    print(f"  [PASS] GICv3: GICD=0x{reg[0] + soc_base:x} GICR=0x{reg[2] + soc_base:x}, vtimer INTID {intid}")

    sd = props(find(arm_io, "sdhci"))
    assert struct.unpack("<2Q", sd["reg"])[0] == SDHCI, "SDHCI base"
    assert b"qcom,sdhci-msm-v5" in sd["compatible"], "SDHCI must be msm-v5 (SDCC5)"
    print(f"  [PASS] SDHCI (SDCC5) 0x{SDHCI:x}")

    ts = props(find(arm_io, "touchscreen"))
    assert struct.unpack("<I", ts["i2c-address"])[0] == 0x62 and struct.unpack("<I", ts["i2c-bus"])[0] == 1, "NVT touch"
    print("  [PASS] touchscreen: Novatek NVT @0x62, i2c-1 (kept off with sweettouch=0)")

    fb = props(find(root, "vince-fbcon"))
    f_reg = struct.unpack("<2Q", fb["reg"])
    w, h, stride = (struct.unpack("<I", fb[k])[0] for k in ("width", "height", "stride"))
    bpp = struct.unpack("<I", fb["bpp"])[0] if "bpp" in fb else 24
    assert f_reg[0] == FB and (w, h) == (FB_W, FB_H), "framebuffer geometry"
    assert stride >= w * bpp // 8 and f_reg[1] >= stride * h, "framebuffer size does not cover stride * height"
    assert f_reg[0] != SWEET_TRAPS["sweet_fb"]
    print(f"  [PASS] fbcon 0x{f_reg[0]:x} {w}x{h}, stride {stride}, {bpp} bpp (UNVERIFIED against ABL scanout)")

    cpus = find(root, "cpus")
    running = [c for c in cpus.children if props(c)["state"].rstrip(b"\0") == b"running"]
    assert len(running) == 1 and nm(running[0]) == "cpu0", "exactly cpu0 must be running"
    for c in cpus.children:
        mpidr, = struct.unpack("<I", props(c)["reg"])
        assert mpidr in (0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103), f"{nm(c)} bad MPIDR 0x{mpidr:x}"
    print(f"  [PASS] cpus: {len(cpus.children)} node(s), cpu0 running, MPIDR layout 0x0-0x3 / 0x100-0x103")

    print("[SUCCESS] tulip ADT checks passed")


if __name__ == "__main__":
    verify(sys.argv[1] if len(sys.argv) > 1 else "artifacts/tulip.adt")
