#!/usr/bin/env python3
"""Build a GPT disk image from partition images, without external tools.

  mkdisk.py OUT.img NAME:SIZE_MIB[:SOURCE] ...

SOURCE may be a raw image or an Android sparse image; only data is written
(holes stay holes, so the result is a sparse file). Partitions are 1 MiB
aligned and numbered in the order given."""
import os, struct, sys, uuid, zlib

SECTOR = 512
MiB = 1 << 20
LINUX_DATA = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")


def copy_raw(src, out, base):
    with open(src, "rb") as f:
        fd = f.fileno()
        end = os.fstat(fd).st_size
        pos = 0
        while pos < end:
            try:
                data = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:
                break
            hole = os.lseek(fd, data, os.SEEK_HOLE)
            f.seek(data)
            left = hole - data
            out.seek(base + data)
            while left:
                b = f.read(min(left, 4 * MiB))
                out.write(b)
                left -= len(b)
            pos = hole


def copy_sparse(src, out, base):
    with open(src, "rb") as f:
        magic, _, _, fhs, chs, blk, _, chunks, _ = struct.unpack("<I4H4I", f.read(28))
        f.seek(fhs)
        pos = 0
        for _ in range(chunks):
            ctype, _, nblk, _ = struct.unpack("<2H2I", f.read(12))
            f.seek(chs - 12, 1)
            size = nblk * blk
            if ctype == 0xCAC1:
                out.seek(base + pos)
                left = size
                while left:
                    b = f.read(min(left, 4 * MiB))
                    out.write(b)
                    left -= len(b)
            elif ctype == 0xCAC2:
                fill = f.read(4)
                if fill != b"\0\0\0\0":
                    out.seek(base + pos)
                    out.write(fill * (size // 4))
            elif ctype == 0xCAC4:
                f.read(4)
            pos += size


def is_sparse(path):
    with open(path, "rb") as f:
        return f.read(4) == struct.pack("<I", 0xED26FF3A)


def main(out_path, specs):
    parts, lba = [], 2048                         # first partition at 1 MiB
    for spec in specs:
        name, size, *src = spec.split(":", 2)
        sectors = int(size) * MiB // SECTOR
        parts.append((name, lba, lba + sectors - 1, src[0] if src else None))
        lba += sectors
    total = lba + 2048                           # 1 MiB for the backup GPT
    entries = b""
    for name, first, last, _ in parts:
        entries += (LINUX_DATA.bytes_le + uuid.uuid4().bytes_le
                    + struct.pack("<QQQ", first, last, 0)
                    + name.encode("utf-16-le").ljust(72, b"\0"))
    entries = entries.ljust(128 * 128, b"\0")
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4().bytes_le

    def header(my, alt, entries_lba):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0,
                        my, alt, 2048, total - 2049, disk_guid, entries_lba, 128, 128, ecrc)
        crc = zlib.crc32(h) & 0xFFFFFFFF
        return (h[:16] + struct.pack("<I", crc) + h[20:]).ljust(SECTOR, b"\0")

    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1,
                               min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    with open(out_path, "wb") as out:
        out.truncate(total * SECTOR)
        out.seek(0); out.write(bytes(mbr))
        out.write(header(1, total - 1, 2)); out.write(entries)
        out.seek((total - 33) * SECTOR); out.write(entries)
        out.write(header(total - 1, 1, total - 33))
        for name, first, last, src in parts:
            if src:
                (copy_sparse if is_sparse(src) else copy_raw)(src, out, first * SECTOR)
                print(f"  {name}: {os.path.basename(src)}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
