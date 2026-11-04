package dev.uml.android17;

import android.annotation.SuppressLint;
import android.content.Context;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.system.Os;
import android.view.MotionEvent;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * Shows the guest's display: the kernel writes every frame into fb.bin
 * (vkms.export=), a 4 KiB header (u32 magic "UMFB", version, width, height,
 * stride, format; u64 seq, frames) and RGBA pixels. seq is odd while a frame
 * is written. The file is mapped and each new frame uploaded as a texture.
 */
@SuppressLint("ViewConstructor")
final class ScreenView extends GLSurfaceView implements GLSurfaceView.Renderer {
    private static final int MAGIC = 0x42464d55, HEADER = 4096;

    private final File fb;
    private MappedByteBuffer map;
    private long mapLen, mapIno = -1;
    private int frameCount;
    private long lastSeq = -1;
    private ByteBuffer pix;
    private int texW, texH;
    private int prog, tex, aPos;
    private int viewW, viewH;
    private FloatBuffer quad;

    /* content rectangle in view pixels, and the guest display size (touch) */
    private volatile float rx, ry, rw, rh;
    private volatile int fbW, fbH;

    ScreenView(Context c, File fb) {
        super(c);
        this.fb = fb;
        setEGLContextClientVersion(2);
        setRenderer(this);
        setRenderMode(RENDERMODE_CONTINUOUSLY);
    }

    /* ---- GL ---- */

    @Override
    public void onSurfaceCreated(GL10 unused, EGLConfig config) {
        String vs = "attribute vec2 p; varying vec2 t;\n"
                + "void main() { t = vec2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5); gl_Position = vec4(p, 0.0, 1.0); }";
        String fs = "precision mediump float; varying vec2 t; uniform sampler2D s;\n"
                + "void main() { gl_FragColor = vec4(texture2D(s, t).rgb, 1.0); }";
        prog = GLES20.glCreateProgram();
        GLES20.glAttachShader(prog, shader(GLES20.GL_VERTEX_SHADER, vs));
        GLES20.glAttachShader(prog, shader(GLES20.GL_FRAGMENT_SHADER, fs));
        GLES20.glLinkProgram(prog);
        aPos = GLES20.glGetAttribLocation(prog, "p");
        int[] t = new int[1];
        GLES20.glGenTextures(1, t, 0);
        tex = t[0];
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        float[] v = {-1, -1, 1, -1, -1, 1, 1, 1};
        quad = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        quad.put(v).position(0);
        texW = texH = 0;
        lastSeq = -1;
    }

    private static int shader(int type, String src) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        return s;
    }

    @Override
    public void onSurfaceChanged(GL10 unused, int w, int h) {
        viewW = w;
        viewH = h;
    }

    @Override
    public void onDrawFrame(GL10 unused) {
        poll();
        GLES20.glViewport(0, 0, viewW, viewH);
        GLES20.glClearColor(0, 0, 0, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        if (texW == 0)
            return;
        // fit the guest display into the view, keeping its aspect ratio
        float s = Math.min((float) viewW / texW, (float) viewH / texH);
        float w = texW * s, h = texH * s;
        rx = (viewW - w) / 2;
        ry = (viewH - h) / 2;
        rw = w;
        rh = h;
        GLES20.glViewport((int) rx, (int) ry, (int) w, (int) h);
        GLES20.glUseProgram(prog);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex);
        GLES20.glEnableVertexAttribArray(aPos);
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, quad);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
    }

    /** Upload the newest complete frame, if there is one. */
    private void poll() {
        try {
            // a new boot makes a new file: check now and then
            if (map == null || (++frameCount % 60) == 0) {
                long ino = fb.exists() ? Os.stat(fb.getPath()).st_ino : -1;
                if (map == null || ino != mapIno || fb.length() != mapLen)
                    remap(ino);
            }
            if (map == null)
                return;
            ByteBuffer m = map.duplicate().order(ByteOrder.LITTLE_ENDIAN);
            if (m.getInt(0) != MAGIC)
                return;
            long seq = m.getLong(24);
            if ((seq & 1) != 0 || seq == lastSeq)
                return;
            int w = m.getInt(8), h = m.getInt(12), stride = m.getInt(16);
            if (w <= 0 || h <= 0 || stride < w * 4 || HEADER + (long) stride * h > mapLen)
                return;
            int size = w * h * 4;
            if (pix == null || pix.capacity() < size)
                pix = ByteBuffer.allocateDirect(size);
            pix.clear();
            if (stride == w * 4) {
                ByteBuffer src = m.duplicate();
                src.position(HEADER).limit(HEADER + size);
                pix.put(src);
            } else {
                for (int y = 0; y < h; y++) {
                    ByteBuffer src = m.duplicate();
                    src.position(HEADER + y * stride).limit(HEADER + y * stride + w * 4);
                    pix.put(src);
                }
            }
            if (m.getLong(24) != seq)
                return;              // torn: try again next frame
            pix.position(0);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex);
            if (w != texW || h != texH) {
                GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, w, h, 0,
                        GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, pix);
                texW = w;
                texH = h;
                fbW = w;
                fbH = h;
            } else {
                GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, pix);
            }
            lastSeq = seq;
        } catch (Exception e) {
            map = null;
        }
    }

    private void remap(long ino) throws Exception {
        map = null;
        mapIno = ino;
        if (ino < 0)
            return;
        try (RandomAccessFile f = new RandomAccessFile(fb, "r")) {
            mapLen = f.length();
            if (mapLen < HEADER)
                return;
            map = f.getChannel().map(FileChannel.MapMode.READ_ONLY, 0, mapLen);
        }
        lastSeq = -1;
    }

    /* ---- touch ---- */

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (rw <= 0 || fbW <= 0)
            return true;
        int x = (int) ((e.getX() - rx) / rw * fbW);
        int y = (int) ((e.getY() - ry) / rh * fbH);
        x = Math.max(0, Math.min(fbW - 1, x));
        y = Math.max(0, Math.min(fbH - 1, y));
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                Touch.send(1, x, y);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                Touch.send(0, 0, 0);
                break;
            default:
                break;
        }
        return true;
    }
}
