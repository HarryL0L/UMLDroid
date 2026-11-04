package dev.uml.android17;

import android.content.Context;
import android.os.StatFs;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.GZIPInputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Downloads the Android 17 image and builds the disk and the initramfs (the kit's setup.sh). */
final class Setup {
    static final String BUILD = "16644040";
    static final String TARGET = "aosp_cf_arm64_only_phone";
    static final String URL = "https://ci.android.com/builds/submitted/" + BUILD + "/" + TARGET
            + "-userdebug/latest/raw/" + TARGET + "-img-" + BUILD + ".zip";
    /* zip (1.2 GB) + disk contents (about 2.5 GB) + margin */
    static final long NEEDED = 4500L << 20;

    interface Listener {
        /** total <= 0: no progress figure */
        void stage(String text, long done, long total);
    }

    static final class Cancelled extends IOException {
        Cancelled() { super("cancelled"); }
    }

    private Setup() {}

    static void install(Context c, Paths p, Listener l, AtomicBoolean cancel) throws IOException {
        p.dir.mkdirs();
        p.unpack.mkdirs();
        long free = new StatFs(p.dir.getPath()).getAvailableBytes();
        long have = p.zip.exists() ? p.zip.length() : 0;
        if (free + have < NEEDED)
            throw new IOException(String.format("Not enough free storage: %.1f GB free, %.1f GB needed",
                    (free + have) / 1e9, NEEDED / 1e9));

        download(p.zip, l, cancel);
        try (ZipFile z = new ZipFile(p.zip)) {
            l.stage("Unpacking the boot images", 0, 0);
            writeFile(p.vendorRamdisk, Images.vendorRamdisk(readEntry(z, "vendor_boot.img")));
            writeFile(p.initRamdisk, Images.bootRamdisk(readEntry(z, "init_boot.img")));
            if (!p.disk.exists())
                buildDisk(c, p, z, l, cancel);
        }
        p.zip.delete();
        buildInitrd(c, p);
        l.stage("Done", 1, 1);
    }

    /* ---------------- download (resumable) ---------------- */

    private static void download(File zip, Listener l, AtomicBoolean cancel) throws IOException {
        File part = new File(zip.getPath() + ".part");
        if (zip.exists())
            return;
        for (int attempt = 0; ; attempt++) {
            try {
                downloadOnce(part, l, cancel);
                break;
            } catch (Cancelled e) {
                throw e;
            } catch (IOException e) {
                if (attempt >= 5)
                    throw new IOException("Download failed: " + e.getMessage(), e);
                l.stage("Download interrupted, retrying…", 0, 0);
                try {
                    Thread.sleep(3000L << attempt);
                } catch (InterruptedException ie) {
                    throw new Cancelled();
                }
            }
        }
        if (!part.renameTo(zip))
            throw new IOException("cannot rename " + part);
    }

    private static void downloadOnce(File part, Listener l, AtomicBoolean cancel) throws IOException {
        long have = part.exists() ? part.length() : 0;
        HttpURLConnection h = (HttpURLConnection) new URL(URL).openConnection();
        h.setInstanceFollowRedirects(true);
        h.setConnectTimeout(30000);
        h.setReadTimeout(60000);
        if (have > 0)
            h.setRequestProperty("Range", "bytes=" + have + "-");
        int code = h.getResponseCode();
        long total;
        boolean append;
        if (code == 206) {
            append = true;
            total = have + h.getContentLengthLong();
        } else if (code == 200) {
            append = false;
            have = 0;
            total = h.getContentLengthLong();
        } else if (code == 416) {          // already complete
            h.disconnect();
            return;
        } else {
            h.disconnect();
            throw new IOException("HTTP " + code);
        }
        try (InputStream in = h.getInputStream();
             OutputStream out = new FileOutputStream(part, append)) {
            byte[] buf = new byte[1 << 16];
            long done = have, shown = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                if (cancel.get())
                    throw new Cancelled();
                out.write(buf, 0, n);
                done += n;
                if (done - shown >= (4 << 20)) {
                    shown = done;
                    l.stage("Downloading Android 17", done, total);
                }
            }
            if (total > 0 && done < total)
                throw new IOException("connection closed early");
        } finally {
            h.disconnect();
        }
    }

    /* ---------------- disk ---------------- */

    private static final class Src {
        final String entry;      // zip entry, "@metadata" for the bundled empty ext4
        final Images.Part part;

        Src(String name, long mib, String entry) {
            this.part = new Images.Part(name, mib);
            this.entry = entry;
        }
    }

    private static void buildDisk(Context c, Paths p, ZipFile z, Listener l, AtomicBoolean cancel)
            throws IOException {
        Src[] layout = {
                new Src("misc", 1, null),
                new Src("custom", 1, "cuttlefish_example_custom.img"),
                new Src("metadata", 64, "@metadata"),
                new Src("boot_a", 64, "boot.img"),
                new Src("init_boot_a", 8, "init_boot.img"),
                new Src("vendor_boot_a", 64, "vendor_boot.img"),
                new Src("vbmeta_a", 1, "vbmeta.img"),
                new Src("vbmeta_system_a", 1, "vbmeta_system.img"),
                new Src("vbmeta_vendor_dlkm_a", 1, "vbmeta_vendor_dlkm.img"),
                new Src("vbmeta_system_dlkm_a", 1, "vbmeta_system_dlkm.img"),
                new Src("super", 8192, "super.img"),
                new Src("userdata", 8192, "userdata.img"),
                new Src("frp", 1, null),
        };
        List<Images.Part> parts = new ArrayList<>();
        long total = 0;
        for (Src s : layout) {
            parts.add(s.part);
            if (s.entry != null && !s.entry.startsWith("@")) {
                ZipEntry e = z.getEntry(s.entry);
                if (e == null)
                    throw new IOException("image is missing " + s.entry);
                total += e.getSize();
            }
        }
        File tmp = new File(p.disk.getPath() + ".part");
        final long fTotal = total;
        final long[] done = {0};
        try (RandomAccessFile out = new RandomAccessFile(tmp, "rw")) {
            Images.createGpt(out, parts);
            for (Src s : layout) {
                if (s.entry == null)
                    continue;
                if (cancel.get())
                    throw new Cancelled();
                l.stage("Building the disk: " + s.part.name, done[0], fTotal);
                InputStream in = s.entry.equals("@metadata")
                        ? new GZIPInputStream(c.getAssets().open("metadata-ext4.gzdata"), 1 << 16)
                        : z.getInputStream(z.getEntry(s.entry));
                try (InputStream cin = new Counting(in, n -> {
                    done[0] += n;
                    if (cancel.get())
                        throw new Cancelled();
                    l.stage("Building the disk: " + s.part.name, done[0], fTotal);
                })) {
                    Images.writeImage(cin, out, s.part.offset(), s.part.size(), null);
                }
            }
            out.getFD().sync();
        }
        if (!tmp.renameTo(p.disk))
            throw new IOException("cannot rename " + tmp);
    }

    private interface Count {
        void add(long n) throws IOException;
    }

    /** Reports the bytes read (from the zip, before sparse expansion). */
    private static final class Counting extends FilterInputStream {
        private final Count count;
        private long pending;

        Counting(InputStream in, Count count) {
            super(in);
            this.count = count;
        }

        private void got(long n) throws IOException {
            pending += n;
            if (pending >= (8 << 20)) {
                count.add(pending);
                pending = 0;
            }
        }

        @Override
        public int read() throws IOException {
            int r = super.read();
            if (r >= 0)
                got(1);
            return r;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            int r = super.read(b, off, len);
            if (r > 0)
                got(r);
            return r;
        }

        @Override
        public long skip(long n) throws IOException {
            long r = super.skip(n);
            if (r > 0)
                got(r);
            return r;
        }

        @Override
        public void close() throws IOException {
            if (pending > 0)
                count.add(pending);
            pending = 0;
            super.close();
        }
    }

    /* ---------------- initramfs ---------------- */

    /**
     * Our first-stage hook and helpers, then the vendor ramdisk, then the
     * generic ramdisk (the kernel unpacks concatenated archives in order).
     * Rebuilt on every start, so app updates reach the guest without touching
     * Android's data.
     */
    static void buildInitrd(Context c, Paths p) throws IOException {
        Images.Cpio cpio = new Images.Cpio();
        cpio.add("first_stage.sh", asset(c, "guest/first_stage.sh"), 0755);
        cpio.add("uml/init.rc", asset(c, "guest/init.rc"), 0644);
        cpio.add("uml/fixup", asset(c, "guest/fixup"), 0755);
        cpio.add("uml/bpfstub", asset(c, "guest/bpfstub"), 0755);
        cpio.add("uml/umlbench", asset(c, "guest/umlbench"), 0755);
        cpio.add("uml/lite.sh", asset(c, "guest/lite.sh"), 0755);
        File tmp = new File(p.initrd.getPath() + ".part");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(cpio.finish());
            copy(new FileInputStream(p.vendorRamdisk), out);
            copy(new FileInputStream(p.initRamdisk), out);
        }
        if (!tmp.renameTo(p.initrd))
            throw new IOException("cannot rename " + tmp);
    }

    /* ---------------- helpers ---------------- */

    private static byte[] asset(Context c, String name) throws IOException {
        return readAll(c.getAssets().open(name));
    }

    private static byte[] readEntry(ZipFile z, String name) throws IOException {
        ZipEntry e = z.getEntry(name);
        if (e == null)
            throw new IOException("image is missing " + name);
        return readAll(z.getInputStream(e));
    }

    static byte[] readAll(InputStream in) throws IOException {
        try (InputStream i = in) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = i.read(buf)) > 0)
                b.write(buf, 0, n);
            return b.toByteArray();
        }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        try (InputStream i = in) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = i.read(buf)) > 0)
                out.write(buf, 0, n);
        }
    }

    private static void writeFile(File f, byte[] data) throws IOException {
        File tmp = new File(f.getPath() + ".part");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(data);
        }
        if (!tmp.renameTo(f))
            throw new IOException("cannot rename " + tmp);
    }
}
