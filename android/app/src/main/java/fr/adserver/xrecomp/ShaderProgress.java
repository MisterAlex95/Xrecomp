package fr.adserver.xrecomp;

/**
 * What the N64 game's screen shows while the Vulkan RDP builds its shaders at the start (ShaderOverlay): the native
 * side's progress (n64_rdp.h n64_rdp_shader_progress, polled: active, pipelines built, pipelines expected) turned into
 * « Compilation des shaders… 7/13 » with a bar, or « Compilation des shaders… 7 » with a spinner when the total is not
 * known (the first start of this driver and resolution, before startup-&lt;f&gt;x.count exists).
 *
 * <p>A start served from the cache (a fraction of a second) shows nothing: the text appears only once the start has
 * been compiling for {@link #SHOW_AFTER_MS}. Polling stops when the start is over, or when none began within
 * {@link #GIVE_UP_MS} (the software RDP, or the game never got that far). Pure: the tests drive it with a clock.
 */
final class ShaderProgress {
    static final long SHOW_AFTER_MS = 300;
    static final long GIVE_UP_MS = 60_000;

    enum Kind { HIDDEN, SPINNER, BAR }

    /** One frame of the overlay: what to show; done / total for the text and the bar (total 0 with SPINNER). */
    static final class Frame {
        final Kind kind;
        final int done, total;

        Frame(Kind kind, int done, int total) {
            this.kind = kind;
            this.done = done;
            this.total = total;
        }
    }

    static final Frame HIDDEN = new Frame(Kind.HIDDEN, 0, 0);

    private long firstPoll = -1, activeSince = -1;
    private boolean finished;
    private int lastDone, lastTotal;

    /** One poll at now (ms, any monotonic clock): the native { active, done, total } (null: no native side). */
    Frame update(long now, int[] p) {
        if (finished)
            return HIDDEN;
        if (firstPoll < 0)
            firstPoll = now;
        boolean active = p != null && p.length >= 3 && p[0] != 0;
        if (!active) {
            if (activeSince >= 0 || now - firstPoll >= GIVE_UP_MS)
                finished = true;   // the start is over (or never came): no more polls
            return HIDDEN;
        }
        if (activeSince < 0)
            activeSince = now;
        lastDone = Math.max(0, p[1]);
        lastTotal = p[2] > 0 && p[1] <= p[2] ? p[2] : 0;
        if (now - activeSince < SHOW_AFTER_MS)
            return HIDDEN;
        return new Frame(lastTotal > 0 ? Kind.BAR : Kind.SPINNER, lastDone, lastTotal);
    }

    /** True once polling may stop. */
    boolean finished() {
        return finished;
    }

    /** The start was seen compiling at all (for the log line at its end). */
    boolean seen() {
        return activeSince >= 0;
    }

    int lastDone() {
        return lastDone;
    }

    int lastTotal() {
        return lastTotal;
    }

    /**
     * The overlay's text: withTotal (« Compilation des shaders… %1$d/%2$d ») when the total is known, withCount
     * (« … %1$d ») once something was built, else plain (« Compilation des shaders… »). HIDDEN: "".
     */
    static String text(Frame f, String plain, String withCount, String withTotal) {
        switch (f.kind) {
        case BAR:
            return String.format(java.util.Locale.ROOT, withTotal, f.done, f.total);
        case SPINNER:
            return f.done > 0 ? String.format(java.util.Locale.ROOT, withCount, f.done) : plain;
        default:
            return "";
        }
    }
}
