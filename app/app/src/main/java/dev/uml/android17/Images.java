package dev.uml.android17;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.List;
import java.util.zip.CRC32;

/**
 * Disk and ramdisk images, without external tools (a port of the kit's
 * mkdisk.py, bootimg.py and mkoverlay.py). No Android dependencies, so it
 * can be tested on any JVM.
 */
final class Images {
    static final long MiB = 1 << 20;
    private static final int SECTOR = 512;
    private static final int BLOCK = 4096;

    interface Progress {
        void bytes(long n);
    }

    /* ---------------- GPT disk ---------------- */

    static final class Part {
        final String name;
        final long sizeMiB;
        long firstLba, lastLba;

        Part(String name, long sizeMiB) {
            this.name = name;
            this.sizeMiB = sizeMiB;
        }

        long offset() {
            return firstLba * SECTOR;
        }

        long size() {
            return sizeMiB * MiB;
        }
    }

    private static final byte[] LINUX_DATA = guidLe("0FC63DAF-8483-4772-8E79-3D69D8477DE4");

    private static byte[] guidLe(String s) {
        String h = s.replace("-", "");
        byte[] b = new byte[16];
        for (int i = 0; i < 16; i++)
            b[i] = (byte) Integer.parseInt(h.substring(2 * i, 2 * i + 2), 16);
        // first three fields are little-endian
        swap(b, 0, 3); swap(b, 1, 2); swap(b, 4, 5); swap(b, 6, 7);
        return b;
    }

    private static void swap(byte[] b, int i, int j) {
        byte t = b[i]; b[i] = b[j]; b[j] = t;
    }

    private static byte[] randomGuid(SecureRandom r) {
        byte[] b = new byte[16];
        r.nextBytes(b);
        b[7] = (byte) ((b[7] & 0x0f) | 0x40);   // version 4 (field 3 is LE)
        b[8] = (byte) ((b[8] & 0x3f) | 0x80);
        return b;
    }

    /**
     * Create @out as a sparse GPT disk with @parts (1 MiB aligned, numbered in
     * order), the layout mkdisk.py writes. Fills in the partitions' LBAs.
     */
    static void createGpt(RandomAccessFile out, List<Part> parts) throws IOException {
        SecureRandom rnd = new SecureRandom();
        long lba = 2048;
        for (Part p : parts) {
            p.firstLba = lba;
            lba += p.sizeMiB * MiB / SECTOR;
            p.lastLba = lba - 1;
        }
        long total = lba + 2048;

        ByteBuffer e = ByteBuffer.allocate(128 * 128).order(ByteOrder.LITTLE_ENDIAN);
        for (Part p : parts) {
            int start = e.position();
            e.put(LINUX_DATA).put(randomGuid(rnd)).putLong(p.firstLba).putLong(p.lastLba).putLong(0);
            byte[] name = p.name.getBytes(StandardCharsets.UTF_16LE);
            e.put(name);
            e.position(start + 128);
        }
        byte[] entries = e.array();
        CRC32 c = new CRC32();
        c.update(entries);
        int ecrc = (int) c.getValue();
        byte[] diskGuid = randomGuid(rnd);

        byte[] mbr = new byte[SECTOR];
        ByteBuffer m = ByteBuffer.wrap(mbr).order(ByteOrder.LITTLE_ENDIAN);
        m.position(446);
        m.put((byte) 0).put((byte) 0).put((byte) 2).put((byte) 0).put((byte) 0xEE)
                .put((byte) 0xFF).put((byte) 0xFF).put((byte) 0xFF).putInt(1)
                .putInt((int) Math.min(total - 1, 0xFFFFFFFFL));
        mbr[510] = 0x55;
        mbr[511] = (byte) 0xAA;

        out.setLength(0);
        out.setLength(total * SECTOR);
        out.seek(0);
        out.write(mbr);
        out.write(gptHeader(1, total - 1, 2, total, diskGuid, ecrc));
        out.write(entries);
        out.seek((total - 33) * SECTOR);
        out.write(entries);
        out.write(gptHeader(total - 1, 1, total - 33, total, diskGuid, ecrc));
    }

    private static byte[] gptHeader(long my, long alt, long entriesLba, long total, byte[] guid, int ecrc) {
        ByteBuffer h = ByteBuffer.allocate(SECTOR).order(ByteOrder.LITTLE_ENDIAN);
        h.put("EFI PART".getBytes(StandardCharsets.US_ASCII)).putInt(0x10000).putInt(92)
                .putInt(0).putInt(0).putLong(my).putLong(alt).putLong(2048).putLong(total - 2049)
                .put(guid).putLong(entriesLba).putInt(128).putInt(128).putInt(ecrc);
        CRC32 c = new CRC32();
        c.update(h.array(), 0, 92);
        h.putInt(16, (int) c.getValue());
        return h.array();
    }

    /* ---------------- writing partition contents ---------------- */

    /**
     * Copy a raw or Android sparse image from @in to @out at @base, leaving
     * holes for zero blocks (so the disk file stays sparse). Returns the
     * image's (unsparsed) size.
     */
    static long writeImage(InputStream in, RandomAccessFile out, long base, long limit, Progress p)
            throws IOException {
        DataInputStream d = new DataInputStream(in);
        byte[] head = new byte[28];
        int n = readUpTo(d, head);
        ByteBuffer hb = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
        if (n == 28 && hb.getInt(0) == 0xED26FF3A)
            return writeSparse(d, hb, out, base, limit, p);
        return writeRaw(d, head, n, out, base, limit, p);
    }

    private static int readUpTo(InputStream in, byte[] b) throws IOException {
        return readUpTo(in, b, 0, b.length);
    }

    private static int readUpTo(InputStream in, byte[] b, int off, int len) throws IOException {
        int n = 0;
        while (n < len) {
            int r = in.read(b, off + n, len - n);
            if (r < 0)
                break;
            n += r;
        }
        return n;
    }

    private static long writeRaw(InputStream in, byte[] first, int firstLen, RandomAccessFile out,
                                 long base, long limit, Progress p) throws IOException {
        byte[] buf = new byte[1 << 20];
        System.arraycopy(first, 0, buf, 0, firstLen);
        int fill = firstLen;
        long pos = 0;
        while (true) {
            fill += readUpTo(in, buf, fill, buf.length - fill);
            if (fill == 0)
                break;
            if (pos + fill > limit)
                throw new IOException("image larger than its partition");
            writeNonZero(out, base + pos, buf, fill);
            pos += fill;
            if (p != null)
                p.bytes(fill);
            if (fill < buf.length)
                break;
            fill = 0;
        }
        return pos;
    }

    private static void writeNonZero(RandomAccessFile out, long at, byte[] buf, int len) throws IOException {
        for (int off = 0; off < len; off += BLOCK) {
            int n = Math.min(BLOCK, len - off);
            boolean zero = true;
            for (int i = off; i < off + n; i++)
                if (buf[i] != 0) { zero = false; break; }
            if (zero)
                continue;
            // merge consecutive non-zero blocks into one write
            int end = off + n;
            while (end < len) {
                int m = Math.min(BLOCK, len - end);
                boolean z = true;
                for (int i = end; i < end + m; i++)
                    if (buf[i] != 0) { z = false; break; }
                if (z)
                    break;
                end += m;
            }
            out.seek(at + off);
            out.write(buf, off, end - off);
            off = end - BLOCK;
        }
    }

    private static long writeSparse(DataInputStream d, ByteBuffer hdr, RandomAccessFile out,
                                    long base, long limit, Progress p) throws IOException {
        int fileHdr = hdr.getShort(8) & 0xffff;
        int chunkHdr = hdr.getShort(10) & 0xffff;
        int blk = hdr.getInt(12);
        int chunks = hdr.getInt(20);
        skip(d, fileHdr - 28);
        byte[] ch = new byte[12];
        byte[] buf = new byte[1 << 20];
        long pos = 0;
        for (int i = 0; i < chunks; i++) {
            d.readFully(ch);
            ByteBuffer cb = ByteBuffer.wrap(ch).order(ByteOrder.LITTLE_ENDIAN);
            int type = cb.getShort(0) & 0xffff;
            long size = (cb.getInt(4) & 0xffffffffL) * blk;
            skip(d, chunkHdr - 12);
            if (pos + size > limit)
                throw new IOException("sparse image larger than its partition");
            if (type == 0xCAC1) {            // raw
                long left = size, at = pos;
                while (left > 0) {
                    int n = (int) Math.min(left, buf.length);
                    d.readFully(buf, 0, n);
                    writeNonZero(out, base + at, buf, n);
                    at += n;
                    left -= n;
                    if (p != null)
                        p.bytes(n);
                }
            } else if (type == 0xCAC2) {     // fill
                byte[] f = new byte[4];
                d.readFully(f);
                if (f[0] != 0 || f[1] != 0 || f[2] != 0 || f[3] != 0) {
                    int n = (int) Math.min(size, buf.length);
                    for (int j = 0; j < n; j += 4)
                        System.arraycopy(f, 0, buf, j, 4);
                    for (long at = 0; at < size; at += n) {
                        int m = (int) Math.min(n, size - at);
                        out.seek(base + pos + at);
                        out.write(buf, 0, m);
                    }
                }
            } else if (type == 0xCAC4) {     // crc
                skip(d, 4);
            }                                // 0xCAC3: don't care
            pos += size;
        }
        return pos;
    }

    private static void skip(DataInputStream d, long n) throws IOException {
        while (n > 0) {
            long s = d.skip(n);
            if (s <= 0) {
                if (d.read() < 0)
                    throw new EOFException();
                s = 1;
            }
            n -= s;
        }
    }

    /* ---------------- boot images ---------------- */

    private static long pages(long n, long ps) {
        return (n + ps - 1) / ps * ps;
    }

    /** The ramdisk of a boot or init_boot image (v3/v4). */
    static byte[] bootRamdisk(byte[] img) throws IOException {
        ByteBuffer b = ByteBuffer.wrap(img).order(ByteOrder.LITTLE_ENDIAN);
        if (!new String(img, 0, 8, StandardCharsets.US_ASCII).equals("ANDROID!"))
            throw new IOException("not a boot image");
        long kernel = b.getInt(8) & 0xffffffffL;
        long ramdisk = b.getInt(12) & 0xffffffffL;
        long header = b.getInt(20) & 0xffffffffL;
        long off = pages(header, 4096) + pages(kernel, 4096);
        return slice(img, off, ramdisk);
    }

    /** All vendor ramdisks of a vendor_boot image (v3/v4), concatenated. */
    static byte[] vendorRamdisk(byte[] img) throws IOException {
        ByteBuffer b = ByteBuffer.wrap(img).order(ByteOrder.LITTLE_ENDIAN);
        if (!new String(img, 0, 8, StandardCharsets.US_ASCII).equals("VNDRBOOT"))
            throw new IOException("not a vendor_boot image");
        long ps = b.getInt(12) & 0xffffffffL;
        long vrSize = b.getInt(24) & 0xffffffffL;
        long header = b.getInt(2096) & 0xffffffffL;
        return slice(img, pages(header, ps), vrSize);
    }

    private static byte[] slice(byte[] a, long off, long len) throws IOException {
        if (off < 0 || len < 0 || off + len > a.length)
            throw new IOException("truncated image");
        byte[] r = new byte[(int) len];
        System.arraycopy(a, (int) off, r, 0, (int) len);
        return r;
    }

    /* ---------------- cpio ---------------- */

    /** An uncompressed newc cpio archive, as mkoverlay.py writes it. */
    static final class Cpio {
        private final ByteArrayOutputStream out = new ByteArrayOutputStream();
        private final java.util.Set<String> dirs = new java.util.HashSet<>();
        private int ino = 1;

        void add(String dest, byte[] data, int mode) throws IOException {
            String[] parts = dest.split("/");
            StringBuilder d = new StringBuilder();
            for (int i = 0; i < parts.length - 1; i++) {
                if (i > 0)
                    d.append('/');
                d.append(parts[i]);
                if (dirs.add(d.toString()))
                    entry(d.toString(), new byte[0], 040755, ino++);
            }
            entry(dest, data, 0100000 | mode, ino++);
        }

        private void entry(String name, byte[] data, int mode, int inode) throws IOException {
            byte[] nameB = (name + "\0").getBytes(StandardCharsets.UTF_8);
            StringBuilder h = new StringBuilder("070701");
            int[] v = {inode, mode, 0, 0, 1, 0, data.length, 0, 0, 0, 0, nameB.length, 0};
            for (int x : v)
                h.append(String.format("%08X", x));
            out.write(h.toString().getBytes(StandardCharsets.US_ASCII));
            out.write(nameB);
            pad(4);
            out.write(data);
            pad(4);
        }

        private void pad(int a) {
            while (out.size() % a != 0)
                out.write(0);
        }

        byte[] finish() throws IOException {
            entry("TRAILER!!!", new byte[0], 0, 0);
            pad(512);
            return out.toByteArray();
        }
    }
}
