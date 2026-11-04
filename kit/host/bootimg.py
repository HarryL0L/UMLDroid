#!/usr/bin/env python3
"""Minimal unpacker for Android boot (v3/v4) and vendor_boot (v3/v4) images.
Usage: bootimg.py <image> <outdir>
Writes kernel, ramdisk(s), bootconfig and prints the cmdline(s)."""
import os
import struct
import sys

img, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
data = open(img, "rb").read()


def pages(n, ps):
    return (n + ps - 1) // ps * ps


def save(name, blob):
    if blob:
        open(os.path.join(out, name), "wb").write(blob)
        print(f"  {name}: {len(blob)} bytes")


magic = data[:8]
if magic == b"ANDROID!":
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from("<IIII", data, 8)
    header_version = struct.unpack_from("<I", data, 40)[0]
    cmdline = data[44:44 + 1536].split(b"\0")[0].decode()
    ps = 4096
    print(f"boot image v{header_version} os_version=0x{os_version:08x}")
    print(f"  cmdline: {cmdline!r}")
    off = pages(header_size, ps)
    save("kernel", data[off:off + kernel_size]); off += pages(kernel_size, ps)
    save("ramdisk", data[off:off + ramdisk_size])
elif magic == b"VNDRBOOT":
    (hv, ps, kaddr, raddr, vr_size) = struct.unpack_from("<IIIII", data, 8)
    cmdline = data[28:28 + 2048].split(b"\0")[0].decode()
    tags_addr = struct.unpack_from("<I", data, 2076)[0]
    name = data[2080:2096].split(b"\0")[0].decode()
    header_size, dtb_size = struct.unpack_from("<II", data, 2096)
    dtb_addr = struct.unpack_from("<Q", data, 2104)[0]
    print(f"vendor_boot v{hv} page_size={ps} name={name!r}")
    print(f"  cmdline: {cmdline!r}")
    off = pages(header_size, ps)
    vr = data[off:off + vr_size]; off += pages(vr_size, ps)
    save("dtb", data[off:off + dtb_size]); off += pages(dtb_size, ps)
    if hv >= 4:
        table_size, entry_num, entry_size, bootconfig_size = struct.unpack_from("<IIII", data, 2112)
        table = data[off:off + table_size]; off += pages(table_size, ps)
        save("bootconfig", data[off:off + bootconfig_size])
        print(f"  vendor ramdisk table: {entry_num} entries")
        for i in range(entry_num):
            e = table[i * entry_size:(i + 1) * entry_size]
            size, offset, rtype = struct.unpack_from("<III", e, 0)
            rname = e[12:12 + 32].split(b"\0")[0].decode()
            print(f"    [{i}] type={rtype} name={rname!r} size={size} offset={offset}")
            save(f"vendor_ramdisk{i:02d}_{rname or 'unnamed'}", vr[offset:offset + size])
    save("vendor_ramdisk_all", vr)
else:
    sys.exit(f"unknown magic {magic!r}")
