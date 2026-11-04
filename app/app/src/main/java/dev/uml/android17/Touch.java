package dev.uml.android17;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Touch and keys for the guest: 12-byte records {u32 type; s32 a; s32 b}
 * written to the kernel's umltouch FIFO. type 1 = finger down/move to (a, b),
 * 0 = finger up, 2 = key a (Linux key code) pressed (b = 1) or released (0).
 */
final class Touch {
    static final int KEY_BACK = 158, KEY_HOME = 172, KEY_RECENTS = 580, KEY_POWER = 116;

    private static FileDescriptor fd;
    private static String path;

    private Touch() {}

    static synchronized void init(String fifo) {
        path = fifo;
    }

    /** Drop the FIFO, a new kernel opens a new one. */
    static synchronized void reset() {
        if (fd != null) {
            try {
                Os.close(fd);
            } catch (ErrnoException ignored) {
            }
        }
        fd = null;
    }

    static synchronized void send(int type, int a, int b) {
        if (path == null)
            return;
        try {
            if (fd == null)
                // O_RDWR: opening never blocks, and the kernel never sees EOF
                fd = Os.open(path, OsConstants.O_RDWR | OsConstants.O_NONBLOCK | OsConstants.O_CLOEXEC, 0);
            byte[] rec = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
                    .putInt(type).putInt(a).putInt(b).array();
            Os.write(fd, rec, 0, rec.length);
        } catch (ErrnoException e) {
            // EAGAIN: the guest is not reading right now, drop the event
            if (e.errno != OsConstants.EAGAIN)
                reset();
        } catch (IOException e) {
            reset();
        }
    }

    static void key(int code) {
        send(2, code, 1);
        send(2, code, 0);
    }
}
