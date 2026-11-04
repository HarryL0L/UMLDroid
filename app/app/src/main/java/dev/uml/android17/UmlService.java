package dev.uml.android17;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.system.Os;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Runs the setup and the UML kernel in the foreground (so that Android
 * does not kill them while the app is in the background).
 */
public class UmlService extends Service {
    static final String ACTION_SETUP = "dev.uml.android17.SETUP";
    static final String ACTION_START = "dev.uml.android17.START";
    static final String ACTION_STOP = "dev.uml.android17.STOP";
    static final String ACTION_RESTART = "dev.uml.android17.RESTART";

    enum Phase { IDLE, SETUP, BOOTING, RUNNING, STOPPING, ERROR }

    static final class State {
        final Phase phase;
        final String text;
        final long done, total;

        State(Phase phase, String text, long done, long total) {
            this.phase = phase;
            this.text = text;
            this.done = done;
            this.total = total;
        }

        boolean busy() {
            return phase == Phase.SETUP || phase == Phase.BOOTING || phase == Phase.RUNNING
                    || phase == Phase.STOPPING;
        }
    }

    /* ---- state shared with the activity (same process) ---- */

    private static volatile State state = new State(Phase.IDLE, "", 0, 0);
    private static final List<Runnable> listeners = new CopyOnWriteArrayList<>();
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static final ArrayDeque<String> lines = new ArrayDeque<>();
    private static final int MAX_LINES = 400;

    static State state() {
        return state;
    }

    /* ---- boot progress, from the console (for the boot screen) ---- */

    private static final Object[][] MILESTONES = {
            {"init first stage started", 4, "Starting the kernel"},
            {"init second stage started", 10, "Starting Android"},
            {"apexd-bootstrap", 14, "Loading system modules"},
            {"Marking APEXd as activated", 24, "Loading system modules"},
            {"post-fs-data", 30, "Preparing your data"},
            {"starting service 'zygote'", 38, "Starting Android's runtime"},
            {"starting service 'surfaceflinger'", 44, "Starting the display"},
            {"sid=u:r:system_server", 55, "Starting system services"},
            {"sid=u:r:platform_app", 78, "Starting the home screen"},
            {"sid=u:r:priv_app", 86, "Almost there"},
    };
    static volatile int bootPercent;
    static volatile String bootLabel = "";
    static volatile long bootStartMs, bootDoneMs, lastOutputMs;

    private static void milestone(String l) {
        for (Object[] m : MILESTONES) {
            int pct = (Integer) m[1];
            if (pct > bootPercent && l.contains((String) m[0])) {
                bootPercent = pct;
                bootLabel = (String) m[2];
            }
        }
    }

    static void listen(Runnable r) {
        listeners.add(r);
    }

    static void unlisten(Runnable r) {
        listeners.remove(r);
    }

    static List<String> consoleLines(int n) {
        synchronized (lines) {
            List<String> r = new ArrayList<>(lines);
            return r.subList(Math.max(0, r.size() - n), r.size());
        }
    }

    private static long lastNotify;

    private void set(Phase phase, String text, long done, long total) {
        State old = state;
        state = new State(phase, text, done, total);
        long now = System.currentTimeMillis();
        // progress updates are frequent: rate-limit them
        if (old.phase == phase && now - lastNotify < 300 && done < total)
            return;
        lastNotify = now;
        main.post(() -> {
            for (Runnable r : listeners)
                r.run();
            updateNotification();
        });
    }

    /* ---- service ---- */

    private Paths paths;
    private Thread worker;
    private final AtomicBoolean cancel = new AtomicBoolean();
    private volatile Process kernel, gpu;
    private volatile OutputStream consoleIn;
    private volatile boolean consoleReady, stopping;
    private PowerManager.WakeLock wakeLock;

    @Override
    public void onCreate() {
        super.onCreate();
        paths = new Paths(this);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel("uml", "Android 17",
                NotificationManager.IMPORTANCE_LOW));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(1, notification());
        String a = intent != null ? intent.getAction() : null;
        if (ACTION_SETUP.equals(a))
            startWorker(this::setupThread);
        else if (ACTION_START.equals(a))
            startWorker(this::kernelThread);
        else if (ACTION_STOP.equals(a))
            stopAll();
        else if (ACTION_RESTART.equals(a))
            restart();
        else if (worker == null)
            stopSelf();
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        cancel.set(true);
        Process k = kernel;
        if (k != null)
            k.destroyForcibly();
        stopGpu();
        releaseWakeLock();
        super.onDestroy();
    }

    private synchronized void startWorker(Runnable r) {
        if (worker != null && worker.isAlive())
            return;
        cancel.set(false);
        worker = new Thread(() -> {
            try {
                r.run();
            } finally {
                main.post(this::workerDone);
            }
        }, "uml-worker");
        worker.start();
    }

    private void workerDone() {
        // posted from the worker's last lines: let it finish first
        Thread w = worker;
        if (w != null) {
            try {
                w.join(2000);
            } catch (InterruptedException ignored) {
            }
        }
        releaseWakeLock();
        if (restartPending) {
            restartPending = false;
            startWorker(this::kernelThread);
            return;
        }
        if (worker == null || !worker.isAlive()) {
            stopForeground(state.phase != Phase.ERROR);
            if (state.phase == Phase.ERROR)
                updateNotification();
            stopSelf();
        }
    }

    private void stopAll() {
        if (state.phase == Phase.SETUP) {
            cancel.set(true);
            return;
        }
        Process k = kernel;
        if (k == null) {
            stopSelf();
            return;
        }
        stopping = true;
        set(Phase.STOPPING, "Shutting down Android…", 0, 0);
        new Thread(() -> {
            // A clean shutdown (Android writes its data out); kill it if that
            // is not possible or takes too long.
            if (!(consoleReady && sendConsole("su 0 sh -c 'sync; reboot -p'")))
                k.destroyForcibly();
            try {
                if (!k.waitFor(60, TimeUnit.SECONDS))
                    k.destroyForcibly();
            } catch (InterruptedException ignored) {
                k.destroyForcibly();
            }
        }, "uml-stop").start();
    }

    private volatile boolean restartPending;

    /** A stuck boot: kill the kernel (no clean shutdown possible) and boot again. */
    private void restart() {
        Process k = kernel;
        if (k == null || worker == null || !worker.isAlive()) {
            startWorker(this::kernelThread);
            return;
        }
        restartPending = true;
        stopping = true;
        k.destroyForcibly();
    }

    /* ---- setup ---- */

    private void setupThread() {
        set(Phase.SETUP, "Preparing…", 0, 0);
        try {
            Setup.install(this, paths, (text, done, total) -> set(Phase.SETUP, text, done, total), cancel);
            set(Phase.IDLE, "Android 17 is installed", 0, 0);
        } catch (Setup.Cancelled e) {
            set(Phase.IDLE, "Setup cancelled (the download continues where it stopped)", 0, 0);
        } catch (Throwable e) {
            set(Phase.ERROR, "Setup failed: " + e.getMessage(), 0, 0);
        }
    }

    /* ---- kernel ---- */

    private void kernelThread() {
        stopping = false;
        consoleReady = false;
        synchronized (lines) {
            lines.clear();
        }
        bootPercent = 0;
        bootLabel = "Starting the kernel";
        bootStartMs = lastOutputMs = System.currentTimeMillis();
        bootDoneMs = 0;
        set(Phase.BOOTING, "Starting…", 0, 0);
        try {
            if (!paths.installed())
                throw new IOException("Android 17 is not installed yet");
            killStaleKernels();
            Setup.buildInitrd(this, paths);
            paths.tmp.mkdirs();
            paths.fb.delete();
            if (!paths.fifo.exists())
                Os.mkfifo(paths.fifo.getPath(), 0600);
            Touch.reset();
            startGpu();

            ProcessBuilder pb = new ProcessBuilder(kernelArgs())
                    .directory(paths.dir)
                    .redirectErrorStream(true)
                    .redirectOutput(paths.console);
            pb.environment().put("HOME", paths.home.getPath());
            pb.environment().put("TMPDIR", paths.tmp.getPath());
            if (Paths.wakeLock(this))
                acquireWakeLock();
            Process k = pb.start();
            kernel = k;
            consoleIn = k.getOutputStream();
            set(Phase.BOOTING, "Android is booting (the first boot takes several minutes)…", 0, 0);

            Thread tail = new Thread(() -> tailConsole(k), "uml-console");
            tail.start();
            int code = k.waitFor();
            tail.join(2000);
            kernel = null;
            stopGpu();
            if (stopping || code == 0)
                set(Phase.IDLE, "Android stopped", 0, 0);
            else
                set(Phase.ERROR, "Android stopped unexpectedly (exit code " + code
                        + "). The console log shows why.", 0, 0);
        } catch (Throwable e) {
            kernel = null;
            stopGpu();
            set(Phase.ERROR, "Could not start: " + e.getMessage(), 0, 0);
        }
    }

    /*
     * GPU modes: umlgpu (a vhost-user virtio-gpu device, kit/gpu) runs next to
     * the kernel, which connects to it at boot. It writes the frames into
     * fb.bin like vkms does; in gfxstream mode Android's GPU drivers run on the
     * phone's GPU through it.
     */
    private void startGpu() throws IOException, InterruptedException {
        int mode = Paths.gpuMode(this);
        if (mode == Paths.GPU_SOFTWARE)
            return;
        if (!new File(paths.umlgpu).exists())
            throw new IOException("this build has no GPU backend (libumlgpu.so)");
        int[] size = Paths.displaySize(this);
        paths.gpuSock.delete();
        ProcessBuilder pb = new ProcessBuilder(paths.umlgpu, "gpu.sock", paths.fb.getPath(),
                Integer.toString(size[0]), Integer.toString(size[1]))
                .directory(paths.dir)
                .redirectErrorStream(true)
                .redirectOutput(paths.gpuLog);
        if (mode == Paths.GPU_GFXSTREAM)
            pb.environment().put("UMLGPU_GFXSTREAM", "1");
        gpu = pb.start();
        for (int i = 0; i < 100 && !paths.gpuSock.exists(); i++) {
            if (!gpu.isAlive())
                break;
            Thread.sleep(50);
        }
        if (!paths.gpuSock.exists()) {
            stopGpu();
            throw new IOException("the GPU backend did not start (see umlgpu.log)");
        }
    }

    private void stopGpu() {
        Process g = gpu;
        gpu = null;
        if (g != null)
            g.destroyForcibly();
    }

    private List<String> kernelArgs() {
        int[] size = Paths.displaySize(this);
        int w = size[0], h = size[1];
        String d = paths.dir.getPath();
        List<String> a = new ArrayList<>();
        a.add(paths.kernel);
        a.add("mem=" + Paths.memMiB(this) + "M");
        a.add("ncpus=" + Paths.cpus(this));
        a.add("idlepoll=200");
        // Per-thread stubs: off by default (see the kit's README)
        if (!Paths.stubThreads(this))
            a.add("nostubthreads");
        String[] fixed = {
                "ubd0=" + d + "/disk.img", "initrd=" + d + "/initrd.img",
                "console=ttyS0", "con0=null", "con=null", "ssl0=fd:0,fd:1", "ssl=null",
                "printk.devkmsg=on", "audit=0", "8250.nr_uarts=1", "firmware_class.path=/vendor/etc/",
                "loop.max_part=7", "init=/init",
                "vkms.width=" + w, "vkms.height=" + h, "vkms.export=" + d + "/fb.bin",
                "umltouch=" + d + "/touch.fifo," + w + "x" + h,
                "androidboot.hardware=cutf_cvm", "androidboot.slot_suffix=_a",
                "androidboot.fstab_suffix=cf.f2fs.hctr2", "androidboot.boot_devices=virtual/block/ubda",
                "androidboot.verifiedbootstate=orange", "androidboot.serialno=UML0001",
                "androidboot.selinux=permissive", "androidboot.first_stage_console=1",
                "androidboot.force_normal_boot=1", "androidboot.init_rc=/dev/uml/init.rc",
                "androidboot.vendor.apex.com.android.hardware.gatekeeper=com.android.hardware.gatekeeper.nonsecure",
                "androidboot.vendor.apex.com.android.hardware.keymint=com.android.hardware.keymint.rust_nonsecure",
                "androidboot.vendor.apex.com.android.hardware.graphics.composer=com.android.hardware.graphics.composer.drm_hwcomposer",
                "androidboot.vendor.apex.com.google.emulated.camera.provider.hal=com.google.emulated.camera.provider.hal",
                "androidboot.cpuvulkan.version=4206592", "androidboot.hardware.gralloc=minigbm",
                "androidboot.hardware.hwcomposer=drm", "androidboot.hardware.hwcomposer.display_finder_mode=drm",
                "androidboot.hardware.hwcomposer.display_framebuffer_format=rgba", "androidboot.hardware.egl=angle",
                "androidboot.hardware.vulkan=pastel", "androidboot.opengles.version=196609",
                "androidboot.lcd_density=" + Paths.DENSITY, "androidboot.hw_timeout_multiplier=50",
                "androidboot.serialconsole=1", "androidboot.console=ttyS0", "androidboot.enable_bootanimation=0",
                "androidboot.setupwizard_mode=DISABLED", "androidboot.ddr_size=2048",
                "androidboot.vsock_lights_port=6900", "androidboot.vsock_lights_cid=3",
                "androidboot.hypervisor.vm.supported=0", "androidboot.hypervisor.protected_vm.supported=0",
                "androidboot.guest_soc.model=UML",
                "androidboot.uml_lite=" + (Paths.lite(this) ? 1 : 0),
        };
        for (String s : fixed)
            a.add(s);
        int gpuMode = Paths.gpuMode(this);
        if (gpuMode != Paths.GPU_SOFTWARE) {
            // the kernel's cwd is paths.dir: a short relative socket path
            a.add("vkms.create_default_dev=0");
            a.add("virtio_uml.device=gpu.sock:16");
        }
        if (gpuMode == Paths.GPU_GFXSTREAM) {
            // Cuttlefish's gfxstream setup: ranchu composer and Vulkan, ANGLE for GLES
            for (int i = 0; i < a.size(); i++) {
                String s = a.get(i);
                if (s.startsWith("androidboot.hardware.hwcomposer="))
                    a.set(i, "androidboot.hardware.hwcomposer=ranchu");
                else if (s.startsWith("androidboot.hardware.vulkan="))
                    a.set(i, "androidboot.hardware.vulkan=ranchu");
                else if (s.startsWith("androidboot.cpuvulkan.version="))
                    a.set(i, "androidboot.cpuvulkan.version=0");
                else if (s.startsWith("androidboot.vendor.apex.com.android.hardware.graphics.composer="))
                    a.set(i, "androidboot.vendor.apex.com.android.hardware.graphics.composer="
                            + "com.android.hardware.graphics.composer.ranchu");
            }
            a.add("androidboot.hardware.hwcomposer.mode=client");
            a.add("androidboot.hardware.gltransport=virtio-gpu-asg");
            // while GPU mode is experimental: Android's errors in the console log
            a.add("androidboot.uml_logcat=1");
        }
        return a;
    }

    /** A kernel left over from a crashed app still holds the disk: stop it. */
    private void killStaleKernels() {
        File[] procs = new File("/proc").listFiles();
        if (procs == null)
            return;
        for (File f : procs) {
            String n = f.getName();
            if (!n.matches("\\d+"))
                continue;
            try (FileInputStream in = new FileInputStream(new File(f, "cmdline"))) {
                byte[] b = new byte[512];
                int len = in.read(b);
                if (len <= 0)
                    continue;
                String cmd = new String(b, 0, len, StandardCharsets.UTF_8);
                if ((cmd.startsWith(paths.kernel + "\0") || cmd.startsWith(paths.umlgpu + "\0"))
                        && Integer.parseInt(n) != android.os.Process.myPid())
                    android.os.Process.sendSignal(Integer.parseInt(n), 9);
            } catch (IOException | RuntimeException ignored) {
            }
        }
    }

    /**
     * Follows the console log: keeps the last lines for the activity, leaves
     * the first-stage shell (as run.sh does) and notices the end of the boot.
     */
    private void tailConsole(Process k) {
        long pos = 0;
        boolean leftFirstStage = false;
        StringBuilder partial = new StringBuilder();
        byte[] buf = new byte[1 << 16];
        while (k.isAlive()) {
            boolean got = false;
            try (RandomAccessFile f = new RandomAccessFile(paths.console, "r")) {
                if (f.length() < pos)
                    pos = 0;
                f.seek(pos);
                int n;
                while ((n = f.read(buf)) > 0) {
                    got = true;
                    pos += n;
                    partial.append(new String(buf, 0, n, StandardCharsets.UTF_8));
                    int nl;
                    while ((nl = partial.indexOf("\n")) >= 0) {
                        line(partial.substring(0, nl));
                        partial.delete(0, nl + 1);
                    }
                    if (!consoleReady && contains(partial, "console:/"))
                        consoleReady = true;
                }
            } catch (IOException ignored) {
            }
            if (!leftFirstStage && sawJobControl) {
                leftFirstStage = true;
                sleep(200);
                sendConsole("exit");
            }
            if (!got)
                sleep(300);
        }
    }

    private volatile boolean sawJobControl;

    private void line(String l) {
        l = l.replace("\r", "");
        lastOutputMs = System.currentTimeMillis();
        if (state.phase == Phase.BOOTING)
            milestone(l);
        synchronized (lines) {
            lines.add(l);
            while (lines.size() > MAX_LINES)
                lines.removeFirst();
        }
        if (l.contains("full job control"))
            sawJobControl = true;
        if (l.contains("console:/"))
            consoleReady = true;
        if (state.phase == Phase.BOOTING && l.contains("sys.boot_completed=1")) {
            bootDoneMs = System.currentTimeMillis();
            bootPercent = 100;
            set(Phase.RUNNING, String.format("Android 17 is running (booted in %d s)",
                    (bootDoneMs - bootStartMs + 500) / 1000), 0, 0);
        }
    }

    private static boolean contains(CharSequence s, String what) {
        return s.toString().contains(what);
    }

    /** Type a command into the guest's console shell. */
    boolean sendConsole(String cmd) {
        OutputStream o = consoleIn;
        if (o == null)
            return false;
        try {
            synchronized (this) {
                o.write((cmd + "\n").getBytes(StandardCharsets.UTF_8));
                o.flush();
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    /* ---- wake lock, notification ---- */

    private void acquireWakeLock() {
        PowerManager pm = getSystemService(PowerManager.class);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "uml:android");
        wakeLock.acquire();
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        wakeLock = null;
    }

    private Notification notification() {
        Intent open = new Intent(this, MainActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, UmlService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
        State s = state;
        Notification.Builder b = new Notification.Builder(this, "uml")
                .setSmallIcon(R.drawable.ic_stat)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(s.text.isEmpty() ? "Working…" : s.text)
                .setContentIntent(pi)
                .setOngoing(s.busy());
        if (s.phase == Phase.SETUP && s.total > 0)
            b.setProgress(1000, (int) (s.done * 1000 / s.total), false);
        else if (s.phase == Phase.SETUP || s.phase == Phase.BOOTING)
            b.setProgress(0, 0, true);
        if (s.busy())
            b.addAction(new Notification.Action.Builder(null,
                    s.phase == Phase.SETUP ? "Cancel" : "Stop", stop).build());
        return b.build();
    }

    private void updateNotification() {
        getSystemService(NotificationManager.class).notify(1, notification());
    }

    /* ---- helpers for the activity ---- */

    static void command(Context c, String action) {
        c.startForegroundService(new Intent(c, UmlService.class).setAction(action));
    }
}
