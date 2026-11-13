package fr.adserver.xboxrecomp;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Icon;
import android.util.Log;

import java.util.List;

/**
 * A pinned home-screen shortcut for one game of the library (Manage › Add to home screen; docs/android/app-shell.md
 * « A game on the home screen »): id « game_<TID> », the game's title image as an adaptive icon (its monogram when it
 * has none), its title as the label, and an explicit intent to LauncherActivity with ACTION_PLAY_TITLE and the title
 * ID. The launcher (Library, GameStore) decides what a tap does; nothing here reads the library.
 *
 * Pinned, not dynamic: the « Change game » and « Launcher » shortcuts of the app icon's long-press stay as they are, and
 * a pinned shortcut needs no slot among the dynamic ones (at most four or five per app on most launchers).
 */
final class TitleShortcut {
    private TitleShortcut() {}

    /** The adaptive icon's canvas: 108 dp at xxxhdpi. The launcher's mask shows its inner 72 dp (two thirds). */
    static final int SIDE = 432;
    /** The title image's side on that canvas: two thirds, so the mask (a circle, a squircle) barely cuts it. */
    static final int ART_SIDE = SIDE * 2 / 3;
    /** Launchers show about 10-12 characters under an icon; the long label (the pin dialog, menus) up to 25. */
    static final int SHORT_MAX = 12, LONG_MAX = 25;
    /** The background behind an image whose border is (mostly) transparent: the stock launcher's dark green. */
    static final int DARK = 0xFF0C110F;

    private static final String ID_PREFIX = "game_";

    static String id(String tid) {
        return ID_PREFIX + tid;
    }

    /** The title ID a shortcut id stands for, or null when it is not a game's. */
    static String titleId(String id) {
        return id != null && id.startsWith(ID_PREFIX) && id.length() > ID_PREFIX.length()
               ? id.substring(ID_PREFIX.length()) : null;
    }

    /**
     * The shortcut's intent: explicit (the package name differs per title APK), a fresh task as for the app icon's
     * shortcuts, so the launcher decides in onCreate (play, or the game's Home).
     */
    static Intent intent(Context c, String tid) {
        return new Intent(c, LauncherActivity.class).setAction(LauncherActivity.ACTION_PLAY_TITLE)
                .putExtra(LauncherActivity.EXTRA_TITLE_ID, tid)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
    }

    /** The shortcut for tid: art may be null (then the monogram of title on the title ID's colour). */
    static ShortcutInfo info(Context c, String tid, String title, Bitmap art) {
        String t = title == null || title.trim().isEmpty() ? tid : title.trim();
        return new ShortcutInfo.Builder(c, id(tid))
                .setShortLabel(label(t, SHORT_MAX))
                .setLongLabel(label(t, LONG_MAX))
                .setIcon(Icon.createWithAdaptiveBitmap(icon(art, tid, t)))
                .setIntent(intent(c, tid))
                .build();
    }

    /**
     * title cut to at most max characters: at the last word break when that keeps more than half of it (« Burnout 3:
     * Takedown » -> « Burnout 3… »), else in the word, with an ellipsis.
     */
    static String label(String title, int max) {
        String t = title == null ? "" : title.trim();
        if (t.codePointCount(0, t.length()) <= max)
            return t;
        String cut = t.substring(0, t.offsetByCodePoints(0, max - 1));
        int sp = -1;
        for (int i = cut.length() - 1; i > 0; i--) {
            if (Character.isWhitespace(cut.charAt(i)) || cut.charAt(i) == ':' || cut.charAt(i) == '-') {
                sp = i;
                break;
            }
        }
        if (sp > max / 2)
            cut = cut.substring(0, sp);
        return cut.replaceAll("[\\s:\\-,.]+$", "") + "…";
    }

    /**
     * The adaptive icon (SIDE x SIDE, opaque): the title image centred at ART_SIDE on its own border's colour, so an
     * image with a plain background reads as one picture under any mask; without an image the monogram, as on the
     * library's cards (GameCard), on the title ID's colour.
     */
    static Bitmap icon(Bitmap art, String key, String title) {
        Bitmap b = Bitmap.createBitmap(SIDE, SIDE, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        if (art != null && art.getWidth() > 0 && art.getHeight() > 0) {
            c.drawColor(borderColor(art));
            float s = (float)ART_SIDE / Math.max(art.getWidth(), art.getHeight());
            float w = art.getWidth() * s, h = art.getHeight() * s;
            RectF dst = new RectF((SIDE - w) / 2, (SIDE - h) / 2, (SIDE + w) / 2, (SIDE + h) / 2);
            Paint p = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
            c.drawBitmap(art, new Rect(0, 0, art.getWidth(), art.getHeight()), dst, p);
            return b;
        }
        int bg = TitleArt.monogramColor(key == null ? title : key);
        c.drawColor(bg);
        String s = TitleArt.initials(title);
        if (s.isEmpty())
            s = "?";
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setTypeface(Theme.MEDIUM);
        // The monogram hues are vivid: white on most, the dark on the light ones (yellow, lime, cyan) to stay legible.
        p.setColor(GamePalette.contrast(0xFFFFFFFF, bg) >= GamePalette.contrast(DARK, bg) ? 0xFFFFFFFF : DARK);
        p.setTextSize(ART_SIDE * 0.5f);
        float tw = p.measureText(s);
        if (tw > ART_SIDE * 0.9f)
            p.setTextSize(ART_SIDE * 0.5f * ART_SIDE * 0.9f / tw);
        Paint.FontMetrics fm = p.getFontMetrics();
        c.drawText(s, (SIDE - p.measureText(s)) / 2, SIDE / 2f - (fm.ascent + fm.descent) / 2, p);
        return b;
    }

    /**
     * The average colour of the image's outer pixel ring, weighted by alpha; DARK when that ring is mostly
     * transparent (a logo cut out on nothing), so the logo sits on the launcher's own dark background.
     */
    static int borderColor(Bitmap art) {
        int w = art.getWidth(), h = art.getHeight();
        long r = 0, g = 0, bl = 0, a = 0;
        int n = 0;
        for (int x = 0; x < w; x++) {
            for (int y : new int[] { 0, h - 1 }) {
                int px = art.getPixel(x, y);
                int al = px >>> 24;
                r += (px >> 16 & 0xff) * al;
                g += (px >> 8 & 0xff) * al;
                bl += (px & 0xff) * al;
                a += al;
                n++;
            }
        }
        for (int y = 1; y < h - 1; y++) {
            for (int x : new int[] { 0, w - 1 }) {
                int px = art.getPixel(x, y);
                int al = px >>> 24;
                r += (px >> 16 & 0xff) * al;
                g += (px >> 8 & 0xff) * al;
                bl += (px & 0xff) * al;
                a += al;
                n++;
            }
        }
        if (n == 0 || a < 255L * n / 2)
            return DARK;
        return 0xFF000000 | (int)(r / a) << 16 | (int)(g / a) << 8 | (int)(bl / a);
    }

    /** Pinning is offered: the default home screen accepts requests (minSdk 30: ShortcutManager is always there). */
    static boolean supported(Context c) {
        try {
            ShortcutManager sm = c.getSystemService(ShortcutManager.class);
            return sm != null && sm.isRequestPinShortcutSupported();
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** Asks the home screen to pin tid's shortcut (it shows its own dialog). True when the request went out. */
    static boolean request(Context c, String tid, String title, Bitmap art) {
        try {
            ShortcutManager sm = c.getSystemService(ShortcutManager.class);
            return sm != null && sm.requestPinShortcut(info(c, tid, title, art), null);
        } catch (RuntimeException e) {   // IllegalStateException in the background, a launcher that refuses
            Log.w(LauncherActivity.TAG, "launcher: cannot request the shortcut of " + tid, e);
            return false;
        }
    }

    /**
     * When tid's shortcut is pinned, its icon and label follow the art and title the launcher now has (the title
     * image may arrive after the pin, from the library scan or the theme's bake). Nothing when it is not pinned.
     */
    static void refresh(Context c, String tid, String title, Bitmap art) {
        try {
            ShortcutManager sm = c.getSystemService(ShortcutManager.class);
            if (sm == null)
                return;
            List<ShortcutInfo> pinned = sm.getPinnedShortcuts();
            String id = id(tid);
            for (ShortcutInfo s : pinned) {
                if (id.equals(s.getId())) {
                    sm.updateShortcuts(java.util.Collections.singletonList(info(c, tid, title, art)));
                    return;
                }
            }
        } catch (RuntimeException e) {
            Log.w(LauncherActivity.TAG, "launcher: cannot update the shortcut of " + tid, e);
        }
    }
}
