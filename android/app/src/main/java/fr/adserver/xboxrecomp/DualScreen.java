package fr.adserver.xboxrecomp;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.view.Display;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The dual-screen decisions (AYN Thor: a 1920 x 1080 top screen and a 1240 x 1080 touch screen below, a second
 * Display; docs/android/app-shell.md « Dual screen »), as pure functions over plain display records so the JVM tests
 * can feed them fake lists. The launcher and the game stay on the display they start on; the companion panel goes on
 * another one, chosen by {@link #choose}. Single-screen devices: choose returns null and nothing runs.
 */
final class DualScreen {
    private DualScreen() {}

    /** A display as the decisions see it (android.view.Display is final and hard to fake). */
    static final class Disp {
        final int id;
        final String name;
        final int flags, width, height, state;
        final float refresh;
        /** Listed by DisplayManager.getDisplays(DISPLAY_CATEGORY_PRESENTATION). */
        final boolean presentation;

        Disp(int id, String name, int flags, int width, int height, float refresh, int state, boolean presentation) {
            this.id = id;
            this.name = name == null ? "" : name;
            this.flags = flags;
            this.width = width;
            this.height = height;
            this.refresh = refresh;
            this.state = state;
            this.presentation = presentation;
        }

        boolean isPrivate() {
            return (flags & Display.FLAG_PRIVATE) != 0;
        }

        boolean isOn() {
            return state == Display.STATE_ON;
        }

        @Override
        public String toString() {
            return String.format(Locale.ROOT, "display %d \"%s\" flags 0x%x %dx%d %.0f Hz %s%s", id, name, flags, width,
                                 height, refresh, stateName(state), presentation ? " presentation" : "");
        }
    }

    static String stateName(int state) {
        switch (state) {
        case Display.STATE_ON: return "on";
        case Display.STATE_OFF: return "off";
        case Display.STATE_DOZE: return "doze";
        case Display.STATE_DOZE_SUSPEND: return "doze-suspend";
        case Display.STATE_ON_SUSPEND: return "on-suspend";
        case Display.STATE_VR: return "vr";
        default: return "unknown";
        }
    }

    /**
     * The display for the companion panel, or null. A presentation display other than ours first (not private, not
     * off); failing that, any other public display that is on. ownId: the display the activity is on.
     */
    static Disp choose(List<Disp> displays, int ownId) {
        for (Disp d : displays) {
            if (d.presentation && d.id != ownId && !d.isPrivate() && d.state != Display.STATE_OFF)
                return d;
        }
        for (Disp d : displays) {
            if (d.id != ownId && !d.isPrivate() && d.isOn())
                return d;
        }
        return null;
    }

    /** What the companion host should do now. */
    enum Action { NONE, SHOW, DISMISS, MOVE }

    /**
     * From the setting, the chosen display (null: none) and the display a Presentation is showing on (-1: none).
     * Black and Panel both show a Presentation (Black's is empty); Off shows nothing.
     */
    static Action decide(GameStore.BottomScreen mode, Disp target, int shownOn) {
        boolean want = mode != GameStore.BottomScreen.OFF && target != null;
        if (!want)
            return shownOn >= 0 ? Action.DISMISS : Action.NONE;
        if (shownOn < 0)
            return Action.SHOW;
        return shownOn == target.id ? Action.NONE : Action.MOVE;
    }

    // ---- Focus: Plan A (not focusable) or Plan B (focusable, keys forwarded) --------------------------------------

    static final String FOCUS_NONE = "none", FOCUS_FORWARD = "forward";
    static final String CFG_FOCUS = "RECOMP_COMPANION_FOCUS";

    /**
     * Plan B when RECOMP_COMPANION_FOCUS=forward in recomp.cfg (wins, so it can be tried without the app's UI), else
     * when the companion_focus setting says forward; Plan A otherwise.
     */
    static boolean forwardFocus(String cfgValue, String prefValue) {
        String v = cfgValue != null && !cfgValue.isEmpty() ? cfgValue : prefValue;
        return FOCUS_FORWARD.equalsIgnoreCase(v == null ? "" : v.trim());
    }

    static boolean forwardFocus(Context c) {
        File ext = GameStore.externalFilesDir(c);
        String cfg = ext == null ? null : RecompConfig.load(ext).get(CFG_FOCUS);
        return forwardFocus(cfg, GameStore.companionFocusPref(c));
    }

    // ---- The framework side ----------------------------------------------------------------------------------------

    static Disp from(Display d, boolean presentation) {
        Display.Mode m = d.getMode();
        return new Disp(d.getDisplayId(), d.getName(), d.getFlags(), m.getPhysicalWidth(), m.getPhysicalHeight(),
                        d.getRefreshRate(), d.getState(), presentation);
    }

    /** Every display, presentation ones marked. */
    static List<Disp> list(DisplayManager dm) {
        List<Disp> out = new ArrayList<>();
        if (dm == null)
            return out;
        List<Integer> pres = new ArrayList<>();
        for (Display d : dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION))
            pres.add(d.getDisplayId());
        for (Display d : dm.getDisplays())
            out.add(from(d, pres.contains(d.getDisplayId())));
        return out;
    }

    /**
     * True when this device has a display the panel could use: the setting row (« Bottom screen ») is only shown
     * then. For the launcher's Settings (another agent's screen): GameStore.bottomScreen / setBottomScreen.
     */
    static boolean available(Context c, int ownDisplayId) {
        return choose(list(c.getSystemService(DisplayManager.class)), ownDisplayId) != null;
    }
}
