package dev.uml.android17;

import android.Manifest;
import android.animation.ObjectAnimator;
import android.animation.PropertyValuesHolder;
import android.animation.ValueAnimator;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.progressindicator.LinearProgressIndicator;
import com.google.android.material.slider.Slider;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;

public class MainActivity extends AppCompatActivity {
    private Paths paths;
    private View panel, screenRoot;
    private FrameLayout screenHolder;
    private ScreenView screen;
    private TextView statusTitle, statusText, console, storageText, memLabel, cpuLabel;
    private LinearProgressIndicator progress;
    private MaterialButton primary, secondary;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable onState = this::refresh;
    private boolean autoShow;
    private UmlService.Phase lastPhase;
    private View bootOverlay;
    private TextView bootStage, bootTime;
    private LinearProgressIndicator bootProgress;
    private View bootRestart;
    private ObjectAnimator pulse;
    private float shownProgress;

    /* boot screen: 10 updates a second while the screen shows */
    private final Runnable bootTicker = new Runnable() {
        @Override
        public void run() {
            if (screenRoot.getVisibility() == View.VISIBLE) {
                updateBootScreen();
                handler.postDelayed(this, 100);
            }
        }
    };

    private final Runnable ticker = new Runnable() {
        @Override
        public void run() {
            if (panel.getVisibility() == View.VISIBLE)
                refreshConsole();
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        paths = new Paths(this);
        Touch.init(paths.fifo.getPath());
        setContentView(R.layout.activity_main);

        panel = findViewById(R.id.panel);
        screenRoot = findViewById(R.id.screenRoot);
        screenHolder = findViewById(R.id.screenHolder);
        statusTitle = findViewById(R.id.statusTitle);
        statusText = findViewById(R.id.statusText);
        console = findViewById(R.id.console);
        storageText = findViewById(R.id.storageText);
        progress = findViewById(R.id.progress);
        primary = findViewById(R.id.primary);
        secondary = findViewById(R.id.secondary);
        memLabel = findViewById(R.id.memLabel);
        cpuLabel = findViewById(R.id.cpuLabel);
        bootOverlay = findViewById(R.id.bootOverlay);
        bootStage = findViewById(R.id.bootStage);
        bootTime = findViewById(R.id.bootTime);
        bootProgress = findViewById(R.id.bootProgress);
        bootRestart = findViewById(R.id.bootRestart);
        bootRestart.setOnClickListener(v -> {
            bootRestart.setVisibility(View.GONE);
            UmlService.command(this, UmlService.ACTION_RESTART);
        });
        pulse = ObjectAnimator.ofPropertyValuesHolder(findViewById(R.id.bootLogo),
                PropertyValuesHolder.ofFloat(View.SCALE_X, 0.88f, 1f),
                PropertyValuesHolder.ofFloat(View.SCALE_Y, 0.88f, 1f));
        pulse.setDuration(1100);
        pulse.setRepeatCount(ValueAnimator.INFINITE);
        pulse.setRepeatMode(ValueAnimator.REVERSE);

        setupSettings();

        findViewById(R.id.navBack).setOnClickListener(v -> Touch.key(Touch.KEY_BACK));
        findViewById(R.id.navHome).setOnClickListener(v -> Touch.key(Touch.KEY_HOME));
        findViewById(R.id.navRecents).setOnClickListener(v -> Touch.key(Touch.KEY_RECENTS));
        findViewById(R.id.navClose).setOnClickListener(v -> showScreen(false));
        findViewById(R.id.closeFloat).setOnClickListener(v -> showScreen(false));
        findViewById(R.id.saveLog).setOnClickListener(v -> saveLog());
        findViewById(R.id.deleteData).setOnClickListener(v -> confirmDelete());
    }

    private void setupSettings() {
        Slider mem = findViewById(R.id.mem);
        Slider cpus = findViewById(R.id.cpus);
        MaterialSwitch wake = findViewById(R.id.wakelock);
        MaterialSwitch stubs = findViewById(R.id.stubthreads);
        MaterialSwitch lite = findViewById(R.id.lite);
        com.google.android.material.button.MaterialButtonToggleGroup gpu = findViewById(R.id.gpuMode);
        int[] gpuIds = {R.id.gpuSoftware, R.id.gpuDisplay, R.id.gpuGfxstream};
        gpu.check(gpuIds[Math.max(0, Math.min(2, Paths.gpuMode(this)))]);
        gpu.addOnButtonCheckedListener((g, id, checked) -> {
            if (!checked)
                return;
            for (int i = 0; i < gpuIds.length; i++)
                if (gpuIds[i] == id)
                    Paths.prefs(this).edit().putInt("gpu", i).apply();
        });
        MaterialSwitch fill = findViewById(R.id.fill);
        MaterialSwitch extraNav = findViewById(R.id.extranav);
        lite.setChecked(Paths.lite(this));
        fill.setChecked(Paths.prefs(this).getBoolean("fill", false));
        extraNav.setChecked(Paths.extraNav(this));
        lite.setOnCheckedChangeListener((b, on) -> Paths.prefs(this).edit().putBoolean("lite", on).apply());
        fill.setOnCheckedChangeListener((b, on) -> Paths.prefs(this).edit().putBoolean("fill", on).apply());
        extraNav.setOnCheckedChangeListener((b, on) -> Paths.prefs(this).edit().putBoolean("extranav", on).apply());

        int n = Math.max(2, Runtime.getRuntime().availableProcessors());
        cpus.setValueTo(n);
        mem.setValue(clamp(Paths.memMiB(this), 1024, 4096, 512));
        cpus.setValue(Math.min(n, Math.max(1, Paths.cpus(this))));
        wake.setChecked(Paths.wakeLock(this));
        stubs.setChecked(Paths.stubThreads(this));
        showSettingLabels(mem.getValue(), cpus.getValue());

        mem.addOnChangeListener((s, v, user) -> {
            Paths.prefs(this).edit().putInt("mem", (int) v).apply();
            showSettingLabels(v, cpus.getValue());
        });
        cpus.addOnChangeListener((s, v, user) -> {
            Paths.prefs(this).edit().putInt("cpus", (int) v).apply();
            showSettingLabels(mem.getValue(), v);
        });
        wake.setOnCheckedChangeListener((b, on) -> Paths.prefs(this).edit().putBoolean("wakelock", on).apply());
        stubs.setOnCheckedChangeListener((b, on) -> Paths.prefs(this).edit().putBoolean("stubthreads", on).apply());
    }

    private static float clamp(int v, int lo, int hi, int step) {
        v = Math.max(lo, Math.min(hi, v));
        return lo + Math.round((v - lo) / (float) step) * step;
    }

    private void showSettingLabels(float mem, float cpus) {
        memLabel.setText(String.format("Memory: %.1f GB", mem / 1024));
        cpuLabel.setText(String.format("Processors: %d", (int) cpus));
    }

    @Override
    protected void onStart() {
        super.onStart();
        UmlService.listen(onState);
        handler.post(ticker);
        refresh();
    }

    @Override
    protected void onStop() {
        UmlService.unlisten(onState);
        handler.removeCallbacks(ticker);
        super.onStop();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (screen != null)
            screen.onResume();
    }

    @Override
    protected void onPause() {
        if (screen != null)
            screen.onPause();
        super.onPause();
    }

    /* ---- state ---- */

    private void refresh() {
        UmlService.State s = UmlService.state();
        boolean installed = paths.installed();
        progress.setVisibility(View.GONE);
        secondary.setVisibility(View.GONE);
        primary.setVisibility(View.VISIBLE);
        primary.setEnabled(true);

        switch (s.phase) {
            case SETUP:
                statusTitle.setText("Installing");
                statusText.setText(s.text + (s.total > 0
                        ? String.format(" (%.0f of %.0f MB)", s.done / 1e6, s.total / 1e6) : ""));
                progress.setVisibility(View.VISIBLE);
                progress.setMax(1000);
                progress.setIndeterminate(s.total <= 0);
                if (s.total > 0)
                    progress.setProgressCompat((int) (s.done * 1000 / s.total), true);
                primary.setVisibility(View.GONE);
                showSecondary("Cancel", v -> UmlService.command(this, UmlService.ACTION_STOP));
                break;
            case BOOTING:
            case RUNNING:
                statusTitle.setText(s.phase == UmlService.Phase.RUNNING ? "Running" : "Booting");
                statusText.setText(s.text);
                if (s.phase == UmlService.Phase.BOOTING) {
                    progress.setVisibility(View.VISIBLE);
                    progress.setIndeterminate(true);
                }
                showPrimary("Show screen", R.drawable.ic_screen, v -> showScreen(true));
                showSecondary("Stop", v -> UmlService.command(this, UmlService.ACTION_STOP));
                if (autoShow) {
                    autoShow = false;
                    showScreen(true);
                }
                break;
            case STOPPING:
                statusTitle.setText("Stopping");
                statusText.setText(s.text);
                progress.setVisibility(View.VISIBLE);
                progress.setIndeterminate(true);
                showPrimary("Stop", R.drawable.ic_stop, null);
                primary.setEnabled(false);
                break;
            default:   // IDLE, ERROR
                if (screenRoot.getVisibility() == View.VISIBLE)
                    showScreen(false);
                if (!installed) {
                    statusTitle.setText("Not installed");
                    statusText.setText(s.phase == UmlService.Phase.ERROR ? s.text
                            : "Downloads Android 17 (1.2 GB) and builds its disk. Needs about 4.5 GB of free storage; 3 GB stay in use.");
                    showPrimary("Install", R.drawable.ic_download,
                            v -> UmlService.command(this, UmlService.ACTION_SETUP));
                } else {
                    statusTitle.setText(s.phase == UmlService.Phase.ERROR ? "Stopped" : "Ready");
                    statusText.setText(!s.text.isEmpty() ? s.text
                            : "Android 17 is installed. The first boot takes several minutes, later ones about one.");
                    showPrimary("Start", R.drawable.ic_play, v -> {
                        autoShow = true;
                        UmlService.command(this, UmlService.ACTION_START);
                    });
                }
                break;
        }
        lastPhase = s.phase;
        refreshStorage(installed);
    }

    private void showPrimary(String text, int icon, View.OnClickListener l) {
        primary.setText(text);
        primary.setIconResource(icon);
        primary.setOnClickListener(l);
    }

    private void showSecondary(String text, View.OnClickListener l) {
        secondary.setVisibility(View.VISIBLE);
        secondary.setText(text);
        secondary.setOnClickListener(l);
    }

    private void refreshConsole() {
        List<String> l = UmlService.consoleLines(40);
        StringBuilder b = new StringBuilder();
        for (String s : l) {
            if (s.length() > 160)
                s = s.substring(0, 160) + "…";
            b.append(s).append('\n');
        }
        console.setText(l.isEmpty() ? "(no output yet)" : b.toString().trim());
    }

    private void refreshStorage(boolean installed) {
        long used = du(paths.dir);
        long free = new StatFs(getFilesDir().getPath()).getAvailableBytes();
        storageText.setText(String.format("%s%.1f GB used by Android 17, %.1f GB free on this phone.",
                installed ? "" : "Not installed. ", used / 1e9, free / 1e9));
    }

    /** Space actually used (sparse files count only their data). */
    private static long du(File f) {
        if (f.isFile()) {
            try {
                return android.system.Os.stat(f.getPath()).st_blocks * 512;
            } catch (Exception e) {
                return f.length();
            }
        }
        long t = 0;
        File[] l = f.listFiles();
        if (l != null)
            for (File c : l)
                t += du(c);
        return t;
    }

    /* ---- screen ---- */

    private void showScreen(boolean show) {
        if (show) {
            if (screen == null) {
                screen = new ScreenView(this, paths.fb);
                screenHolder.addView(screen);
            } else {
                screen.onResume();
            }
            boolean extra = Paths.extraNav(this);
            findViewById(R.id.navBar).setVisibility(extra ? View.VISIBLE : View.GONE);
            findViewById(R.id.closeFloat).setVisibility(extra ? View.GONE : View.VISIBLE);
            panel.setVisibility(View.GONE);
            screenRoot.setVisibility(View.VISIBLE);
            if (UmlService.state().phase == UmlService.Phase.BOOTING) {
                bootOverlay.animate().cancel();
                bootOverlay.setAlpha(1f);
                bootOverlay.setVisibility(View.VISIBLE);
                shownProgress = UmlService.bootPercent;
                if (!pulse.isStarted())
                    pulse.start();
            } else {
                bootOverlay.setVisibility(View.GONE);
            }
            handler.removeCallbacks(bootTicker);
            handler.post(bootTicker);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            immersive(true);
        } else {
            if (screen != null)
                screen.onPause();
            screenRoot.setVisibility(View.GONE);
            panel.setVisibility(View.VISIBLE);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            immersive(false);
            refreshConsole();
        }
    }

    /** The boot screen follows the console's milestones and creeps between them. */
    private void updateBootScreen() {
        UmlService.State s = UmlService.state();
        if (s.phase == UmlService.Phase.BOOTING) {
            if (bootOverlay.getVisibility() != View.VISIBLE) {
                bootOverlay.setAlpha(1f);
                bootOverlay.setVisibility(View.VISIBLE);
                if (!pulse.isStarted())
                    pulse.start();
            }
            long now = System.currentTimeMillis();
            float target = UmlService.bootPercent;
            if (shownProgress < target)
                shownProgress += Math.max(0.3f, (target - shownProgress) * 0.15f);
            else if (shownProgress < Math.min(97, target + 9))
                shownProgress += 0.02f;     // slowly, while a phase takes long
            bootProgress.setProgressCompat((int) (shownProgress * 10), true);
            long secs = (now - UmlService.bootStartMs) / 1000;
            bootTime.setText(String.format("%d:%02d", secs / 60, secs % 60));
            boolean stuck = now - UmlService.lastOutputMs > 90_000;
            bootStage.setText(stuck ? "Android seems to be stuck" : UmlService.bootLabel + "…");
            bootRestart.setVisibility(stuck ? View.VISIBLE : View.GONE);
        } else if (bootOverlay.getVisibility() == View.VISIBLE && bootOverlay.getAlpha() == 1f) {
            bootProgress.setProgressCompat(1000, true);
            if (s.phase == UmlService.Phase.RUNNING && UmlService.bootDoneMs > 0)
                bootStage.setText(String.format("Booted in %d s",
                        (UmlService.bootDoneMs - UmlService.bootStartMs + 500) / 1000));
            bootOverlay.animate().alpha(0f).setStartDelay(600).setDuration(400).withEndAction(() -> {
                bootOverlay.setVisibility(View.GONE);
                pulse.cancel();
            });
        }
    }

    @SuppressWarnings("deprecation")
    private void immersive(boolean on) {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c == null)
                return;
            if (on) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            } else {
                c.show(WindowInsets.Type.systemBars());
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(on
                    ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    : 0);
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        // While the screen shows, the phone's Back is Android 17's Back
        if (screenRoot.getVisibility() == View.VISIBLE && keyCode == KeyEvent.KEYCODE_BACK) {
            Touch.key(Touch.KEY_BACK);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    /* ---- log, data ---- */

    private void saveLog() {
        if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
            return;
        }
        File dl = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        File out = new File(dl, "android17-uml-console.log");
        if (paths.gpuLog.exists())
            copyFile(paths.gpuLog, new File(dl, "android17-uml-umlgpu.log"));
        try (InputStream in = new FileInputStream(paths.console);
             OutputStream o = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0)
                o.write(buf, 0, n);
            Toast.makeText(this, "Saved to Downloads/" + out.getName(), Toast.LENGTH_LONG).show();
        } catch (IOException e) {
            Toast.makeText(this, "Could not save the log: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private static void copyFile(File from, File to) {
        try (InputStream in = new FileInputStream(from); OutputStream o = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0)
                o.write(buf, 0, n);
        } catch (IOException ignored) {
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code == 1 && results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED)
            saveLog();
    }

    private void confirmDelete() {
        if (UmlService.state().busy()) {
            Toast.makeText(this, "Stop Android first", Toast.LENGTH_SHORT).show();
            return;
        }
        new MaterialAlertDialogBuilder(this)
                .setTitle(R.string.delete_data)
                .setMessage(R.string.delete_confirm)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton("Delete", (d, w) -> {
                    deleteTree(paths.dir);
                    deleteTree(paths.tmp);
                    refresh();
                })
                .show();
    }

    private static void deleteTree(File f) {
        File[] l = f.listFiles();
        if (l != null)
            for (File c : l)
                deleteTree(c);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
