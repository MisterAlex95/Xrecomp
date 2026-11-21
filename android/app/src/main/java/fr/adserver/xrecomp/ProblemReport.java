package fr.adserver.xrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.util.function.Consumer;

/**
 * « Report a problem » (XBA-339): a problem that is not a crash (a black screen, broken textures, no sound), told in
 * one line by the player and sent as a Rustrak feedback with the game's log and breadcrumbs, and a screenshot only
 * when the player ticks it (unticked by default, previewed before sending). Tap-only; no name or e-mail field. The
 * in-game menu (GameMenu's Report page) and the launcher (Settings › Advanced) both build it here.
 */
final class ProblemReport {
    private ProblemReport() {}

    static final String FROM_GAME = "game", FROM_LAUNCHER = "launcher";
    /** The line's length cap. */
    static final int TEXT_MAX = 500;
    /** The screenshot's longest side, in pixels. */
    static final int SHOT_SIDE = 640;
    /** JPEG qualities tried in turn until the picture fits {@link CrashReport#SHOT_MAX}. */
    static final int[] QUALITIES = { 85, 70, 55, 40 };

    /** What the player typed, made one line: trimmed, every run of white space one space, at most TEXT_MAX. */
    static String clean(String s) {
        if (s == null)
            return "";
        String t = s.replaceAll("\\s+", " ").trim();
        return t.length() > TEXT_MAX ? t.substring(0, TEXT_MAX).trim() : t;
    }

    /** The size a w x h picture is sent at: its longest side SHOT_SIDE at most, the shape kept. */
    static int[] shotSize(int w, int h) {
        if (w <= 0 || h <= 0)
            return new int[] { 0, 0 };
        int side = Math.max(w, h);
        if (side <= SHOT_SIDE)
            return new int[] { w, h };
        float k = SHOT_SIDE / (float)side;
        return new int[] { Math.max(1, Math.round(w * k)), Math.max(1, Math.round(h * k)) };
    }

    /** inSampleSize for decoding a w x h file near SHOT_SIDE without holding the full picture (a power of two). */
    static int sampleSize(int w, int h) {
        int s = 1;
        while (Math.max(w, h) / (s * 2) >= SHOT_SIDE)
            s *= 2;
        return s;
    }

    /**
     * The frame as the screenshot sent: downscaled to SHOT_SIDE, JPEG, under CrashReport.SHOT_MAX; null when there is
     * no frame or it cannot fit.
     */
    static byte[] jpeg(Bitmap frame) {
        if (frame == null || frame.getWidth() <= 0 || frame.getHeight() <= 0)
            return null;
        int[] sz = shotSize(frame.getWidth(), frame.getHeight());
        Bitmap b = sz[0] == frame.getWidth() && sz[1] == frame.getHeight() ? frame
                   : Bitmap.createScaledBitmap(frame, sz[0], sz[1], true);
        try {
            for (int q : QUALITIES) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                if (!b.compress(Bitmap.CompressFormat.JPEG, q, out))
                    return null;
                if (out.size() > 0 && out.size() <= CrashReport.SHOT_MAX)
                    return out.toByteArray();
            }
            return null;
        } finally {
            if (b != frame)
                b.recycle();
        }
    }

    /** A frame file (the runtime's BMP) decoded near SHOT_SIDE, then made the screenshot; the file is deleted. */
    static byte[] jpegFromFile(File f) {
        try {
            if (f == null || !f.isFile())
                return null;
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(f.getPath(), o);
            if (o.outWidth <= 0 || o.outHeight <= 0)
                return null;
            BitmapFactory.Options d = new BitmapFactory.Options();
            d.inSampleSize = sampleSize(o.outWidth, o.outHeight);
            Bitmap b = BitmapFactory.decodeFile(f.getPath(), d);
            if (b == null)
                return null;
            try {
                return jpeg(b);
            } finally {
                b.recycle();
            }
        } catch (RuntimeException | OutOfMemoryError e) {
            return null;
        } finally {
            if (f != null)
                f.delete();
        }
    }

    /** The preview: exactly the bytes that would be sent, decoded. */
    static Bitmap preview(byte[] jpeg) {
        return jpeg == null ? null : BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
    }

    /**
     * The report: the line, the game (its key and title, or none), its log, the run's breadcrumbs, and the screenshot
     * when the player added one (null: none). from: FROM_GAME or FROM_LAUNCHER (a tag).
     */
    static CrashReport build(Context c, String from, String key, String title, String text, File log, String logName,
                             byte[] shot) {
        CrashReport r = CrashReport.feedback(c, key, title, clean(text), log, logName);
        r.tags.put("from", from);
        setShot(r, shot);
        return r;
    }

    /** Adds (or with null takes away) the screenshot; one over SHOT_MAX is never set (the envelope would drop it). */
    static void setShot(CrashReport r, byte[] shot) {
        r.image = shot != null && shot.length > 0 && shot.length <= CrashReport.SHOT_MAX ? shot : null;
        r.imageName = "screenshot.jpg";
        r.imageType = "image/jpeg";
    }

    /**
     * The one-line dialog (the keyboard: as Add a code, CheatsUi): OK only with some text; done(text) on OK, nothing
     * on Cancel.
     */
    static AlertDialog askText(Activity a, String current, Consumer<String> done) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int)(20 * a.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        EditText t = new EditText(a);
        t.setSingleLine(true);
        t.setHint(R.string.REPORT_PROBLEM_HINT);
        t.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        t.setImeOptions(EditorInfo.IME_ACTION_DONE);
        t.setFilters(new InputFilter[] { new InputFilter.LengthFilter(TEXT_MAX) });
        if (current != null)
            t.setText(current);
        box.addView(t);
        AlertDialog d = CheatsUi.builder(a).setTitle(R.string.REPORT_PROBLEM).setView(box)
                                .setPositiveButton(R.string.CHEATS_OK, null)
                                .setNegativeButton(R.string.CANCEL, null).create();
        Runnable ok = () -> {
            String s = clean(t.getText().toString());
            if (s.isEmpty())
                return;
            d.dismiss();
            done.accept(s);
        };
        d.setOnShowListener(x -> {
            android.widget.Button b = d.getButton(AlertDialog.BUTTON_POSITIVE);
            b.setEnabled(!clean(t.getText().toString()).isEmpty());
            b.setOnClickListener(v -> ok.run());
            t.addTextChangedListener(new TextWatcher() {
                @Override public void beforeTextChanged(CharSequence s, int st, int c, int af) {}
                @Override public void onTextChanged(CharSequence s, int st, int b2, int c) {}
                @Override public void afterTextChanged(Editable e) {
                    b.setEnabled(!clean(e.toString()).isEmpty());
                }
            });
            t.requestFocus();
        });
        t.setOnEditorActionListener((v, action, ev) -> {
            if (action == EditorInfo.IME_ACTION_DONE) {
                ok.run();
                return true;
            }
            return false;
        });
        d.show();
        return d;
    }
}
