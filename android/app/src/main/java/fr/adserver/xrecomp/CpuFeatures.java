package fr.adserver.xrecomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

/**
 * The CPU features a pack may be compiled for (XBA-313, tools/prepare/cpu.py): "lse", the ARMv8.1 atomics, when the
 * kernel's AT_HWCAP has HWCAP_ATOMICS. The on-device Prepare compiles them in when the device has them and writes
 * pack.json "cpu_features"; {@link Pack#check} refuses a pack whose features this CPU lacks (a pack restored onto
 * another device), so the launcher offers to prepare it again. Read from /proc/self/auxv: the same bit Prepare's
 * Python and the native loader (getauxval) read, so the three agree.
 */
final class CpuFeatures {
    private CpuFeatures() {}

    static final int AT_HWCAP = 16;
    static final long HWCAP_ATOMICS = 1L << 8;

    private static volatile String override;
    private static volatile String cached;

    /**
     * This CPU's features, comma-separated ("lse" or ""), or null when they cannot be read (then {@link Pack#check}
     * does not refuse on them: the native loader, which reads them with getauxval, still would).
     */
    static String host() {
        String o = override;
        if (o != null)
            return o;
        String c = cached;
        if (c == null) {
            c = detect();
            cached = c;
        }
        return c.equals(UNKNOWN) ? null : c;
    }

    private static final String UNKNOWN = "?";

    /** Tests: replace {@link #host()}'s answer; null goes back to the CPU's. */
    static void setForTest(String features) {
        override = features;
    }

    private static String detect() {
        String arch = String.valueOf(System.getProperty("os.arch")).toLowerCase(Locale.ROOT);
        if (!arch.equals("aarch64") && !arch.equals("arm64"))
            return "";
        try (InputStream in = new FileInputStream("/proc/self/auxv")) {
            byte[] buf = new byte[4096];
            int n = 0, r;
            while (n < buf.length && (r = in.read(buf, n, buf.length - n)) > 0)
                n += r;
            Long hw = hwcap(buf, n);
            if (hw != null)
                return (hw & HWCAP_ATOMICS) != 0 ? "lse" : "";
        } catch (IOException | RuntimeException e) {
            // below: /proc/cpuinfo, the same bits by name
        }
        try {
            String info = PrepareProtocol.readText(new File("/proc/cpuinfo"), 1 << 20);
            for (String line : info.split("\n"))
                if (line.startsWith("Features"))
                    return (" " + line.substring(line.indexOf(':') + 1) + " ").contains(" atomics ") ? "lse" : "";
        } catch (IOException | RuntimeException e) {
            // unknown
        }
        return UNKNOWN;
    }

    /** AT_HWCAP from a 64-bit little-endian auxv, or null. */
    static Long hwcap(byte[] auxv, int len) {
        for (int off = 0; off + 16 <= len; off += 16) {
            long key = le64(auxv, off), val = le64(auxv, off + 8);
            if (key == 0)
                break;
            if (key == AT_HWCAP)
                return val;
        }
        return null;
    }

    private static long le64(byte[] b, int o) {
        long v = 0;
        for (int i = 7; i >= 0; i--)
            v = (v << 8) | (b[o + i] & 0xFFL);
        return v;
    }

    /** The first feature of the comma-separated needed that have lacks (an unknown one too), or null. */
    static String missing(String needed, String have) {
        if (needed == null)
            return null;
        for (String f : needed.split(",")) {
            f = f.trim().toLowerCase(Locale.ROOT);
            if (f.isEmpty())
                continue;
            boolean found = false;
            for (String h : (have == null ? "" : have).split(","))
                if (h.trim().toLowerCase(Locale.ROOT).equals(f))
                    found = true;
            if (!found)
                return f;
        }
        return null;
    }
}
