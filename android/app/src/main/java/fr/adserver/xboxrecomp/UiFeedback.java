package fr.adserver.xboxrecomp;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.SoundPool;
import android.view.HapticFeedbackConstants;
import android.view.View;

import java.util.ArrayList;
import java.util.EnumMap;
import java.util.List;

/**
 * The launcher's five UI sounds and their haptics: a move of the focus or a setting's value, a select, a back, an
 * error, a done (a game set up, a Prepare finished). The sounds are short WAVs in res/raw, made with soundcraft-cli
 * (tools/android/ui_sounds.sh), played through SoundPool at a low volume; the haptics are View.performHapticFeedback
 * (so the system's own « touch feedback » setting still applies). Either can be turned off in Settings › Launcher
 * (GameStore.uiSound / uiHaptics).
 */
final class UiFeedback {
    enum Cue {
        MOVE(R.raw.ui_move, 0.22f, HapticFeedbackConstants.CLOCK_TICK),
        SELECT(R.raw.ui_select, 0.35f, HapticFeedbackConstants.KEYBOARD_TAP),
        BACK(R.raw.ui_back, 0.30f, HapticFeedbackConstants.KEYBOARD_TAP),
        ERROR(R.raw.ui_error, 0.35f, HapticFeedbackConstants.REJECT),
        DONE(R.raw.ui_done, 0.40f, HapticFeedbackConstants.CONFIRM);

        final int raw, haptic;
        final float volume;

        Cue(int raw, float volume, int haptic) {
            this.raw = raw;
            this.volume = volume;
            this.haptic = haptic;
        }
    }

    private final Context context;
    private SoundPool pool;
    private final EnumMap<Cue, Integer> ids = new EnumMap<>(Cue.class);
    /** The cues played, newest last (the JVM tests read them). */
    final List<Cue> played = new ArrayList<>();

    UiFeedback(Context c) {
        context = c.getApplicationContext();
    }

    private void load() {
        if (pool != null)
            return;
        pool = new SoundPool.Builder().setMaxStreams(3)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
                .build();
        for (Cue cue : Cue.values())
            ids.put(cue, pool.load(context, cue.raw, 1));
    }

    /** Plays cue (when sounds are on) and its haptic on v (when haptics are on and v is not null). */
    void play(Cue cue, View v) {
        if (played.size() > 64)
            played.remove(0);
        played.add(cue);
        if (GameStore.uiSound(context)) {
            try {
                load();
                Integer id = ids.get(cue);
                if (id != null && id != 0)
                    pool.play(id, cue.volume, cue.volume, 1, 0, 1f);
            } catch (RuntimeException ignored) {   // no audio output: the UI carries on silently
            }
        }
        if (v != null && GameStore.uiHaptics(context))
            v.performHapticFeedback(cue.haptic, HapticFeedbackConstants.FLAG_IGNORE_VIEW_SETTING);
    }

    void release() {
        if (pool != null) {
            pool.release();
            pool = null;
            ids.clear();
        }
    }
}
