#!/usr/bin/env python3
"""Pack an Android boot image (header v0) for Xiaomi Redmi Note 6 Pro (tulip / SDM636).

Header values are copied from tulip's stock MIUI boot.img (Android 9, page size 4096, offsets relative to base 0):
  kernel 0x8000, ramdisk 0x1000000, second 0xf00000, tags 0x100, os_version 9.0.0 / 2019-08.
Unlike vince (LK, DTB passed in the 'second' slot) tulip's ABL expects the DTB(s) appended to the kernel image, as in
the stock image (gzip Image + 36 DTBs): ABL picks the DTB whose qcom,msm-id / board-id / pmic-id match (E7T: msm-id
0x159, board-id 0x40008) and hands it to the kernel. So --dtb is APPENDED to --kernel here. The kernel/shim should
carry an arm64 Image header (image_size at offset 16) so ABL can find where the appended DTB starts.
UNTESTED on hardware: use `fastboot boot`, never flash.
"""
import argparse
import hashlib
import os
import struct

PAGE = 4096
KERNEL_OFF, RAMDISK_OFF, SECOND_OFF, TAGS_OFF = 0x00008000, 0x01000000, 0x00f00000, 0x00000100
OS_VERSION = (9 << 25) | (0 << 18) | (0 << 11) | ((2019 - 2000) << 4) | 8  # 9.0.0, 2019-08  (= 0x12000138 in stock)


def round_up(n, page=PAGE):
    return (n + page - 1) // page * page


def pack(kernel_path, out_path, ramdisk_path=None, dtb_paths=(), cmdline="console=ttyMSM0,115200,n8 loop.max_part=7"):
    kernel = open(kernel_path, "rb").read()
    for d in dtb_paths:
        kernel += open(d, "rb").read()   # appended DTB(s), concatenated like the stock Image.gz-dtb
    ramdisk = open(ramdisk_path, "rb").read() if ramdisk_path else b""

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    header = struct.pack(
        "<8s10I16s512s32s1024s",
        b"ANDROID!", len(kernel), KERNEL_OFF, len(ramdisk), RAMDISK_OFF, 0, SECOND_OFF, TAGS_OFF,
        PAGE, 0, OS_VERSION, b"", cmdline.encode()[:511].ljust(512, b"\0"),
        sha.digest().ljust(32, b"\0"), b"\0" * 1024)

    with open(out_path, "wb") as f:
        f.write(header.ljust(PAGE, b"\0"))
        f.write(kernel.ljust(round_up(len(kernel)), b"\0"))
        if ramdisk:
            f.write(ramdisk.ljust(round_up(len(ramdisk)), b"\0"))
    print(f"[+] {out_path}: {os.path.getsize(out_path)} bytes (kernel+dtb {len(kernel)} B, ramdisk {len(ramdisk)} B)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Pack tulip boot.img (appended-DTB layout)")
    ap.add_argument("--kernel", required=True, help="shim image (arm64 Image)")
    ap.add_argument("--ramdisk", help="payload (mkpayload output)")
    ap.add_argument("--dtb", action="append", default=[], help="DTB to append to the kernel (repeatable)")
    ap.add_argument("--cmdline", default="console=ttyMSM0,115200,n8 loop.max_part=7")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    pack(a.kernel, a.out, a.ramdisk, a.dtb, a.cmdline)
