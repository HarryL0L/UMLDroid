package dev.uml.android17;

import android.app.ActivityManager;
import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;

/** Where everything lives (app-private storage) and the user's settings. */
final class Paths {
    final File dir, disk, initrd, console, fb, fifo, zip, unpack, vendorRamdisk, initRamdisk, home, tmp;
    final String kernel, umlgpu;
    final File gpuSock, gpuLog;

    Paths(Context c) {
        dir = new File(c.getFilesDir(), "android");
        disk = new File(dir, "disk.img");
        initrd = new File(dir, "initrd.img");
        console = new File(dir, "console.log");
        fb = new File(dir, "fb.bin");
        fifo = new File(dir, "touch.fifo");
        zip = new File(dir, "img.zip");
        unpack = new File(dir, "unpack");
        vendorRamdisk = new File(unpack, "vendor_ramdisk_all");
        initRamdisk = new File(unpack, "init_ramdisk");
        home = c.getFilesDir();
        tmp = new File(c.getCacheDir(), "uml-tmp");
        kernel = c.getApplicationInfo().nativeLibraryDir + "/libuml.so";
        umlgpu = c.getApplicationInfo().nativeLibraryDir + "/libumlgpu.so";
        gpuSock = new File(dir, "gpu.sock");
        gpuLog = new File(dir, "umlgpu.log");
    }

    boolean installed() {
        return disk.exists() && vendorRamdisk.exists() && initRamdisk.exists();
    }

    /* ---- settings ---- */

    static final int WIDTH = 720, HEIGHT = 1280, DENSITY = 320;

    /** Guest display size: 720x1280, or 720 wide with the phone's aspect ratio. */
    static int[] displaySize(Context c) {
        if (!prefs(c).getBoolean("fill", false))
            return new int[]{WIDTH, HEIGHT};
        android.util.DisplayMetrics m = c.getResources().getDisplayMetrics();
        int w = Math.min(m.widthPixels, m.heightPixels), h = Math.max(m.widthPixels, m.heightPixels);
        int gh = Math.round(WIDTH * (float) h / w / 8) * 8;
        return new int[]{WIDTH, Math.max(HEIGHT, Math.min(gh, 1800))};
    }

    static boolean lite(Context c) {
        return prefs(c).getBoolean("lite", true);
    }

    /** 0: software (vkms), 1: virtio-gpu display only, 2: GPU acceleration (gfxstream) */
    static final int GPU_SOFTWARE = 0, GPU_DISPLAY = 1, GPU_GFXSTREAM = 2;

    static int gpuMode(Context c) {
        return prefs(c).getInt("gpu", GPU_SOFTWARE);
    }

    static boolean extraNav(Context c) {
        return prefs(c).getBoolean("extranav", false);
    }

    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("settings", Context.MODE_PRIVATE);
    }

    static int defaultMemMiB(Context c) {
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        ((ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(mi);
        return mi.totalMem >= 7L << 30 ? 2048 : 1536;
    }

    static int defaultCpus() {
        return Runtime.getRuntime().availableProcessors() >= 6 ? 4 : 2;
    }

    static int memMiB(Context c) {
        return prefs(c).getInt("mem", defaultMemMiB(c));
    }

    static int cpus(Context c) {
        return prefs(c).getInt("cpus", defaultCpus());
    }

    static boolean stubThreads(Context c) {
        return prefs(c).getBoolean("stubthreads", false);
    }

    static boolean wakeLock(Context c) {
        return prefs(c).getBoolean("wakelock", true);
    }
}
