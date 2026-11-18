package fr.adserver.xrecomp;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * The launcher's Settings (XBA-41): a curated handful of RECOMP_* options, read from and written to
 * <external files dir>/recomp.cfg, which the runtime loads at start (src/platform/recomp_cfg.c). The file is
 * parsed the same way: one KEY=VALUE per line, blanks and # comments skipped, keys and values trimmed, the first
 * line for a key wins. Writing keeps every line the player wrote by hand: a managed key's first line is rewritten in
 * place (its later duplicates removed, as they would shadow nothing but would come back into force if the first went
 * away), a default value removes the key's lines, a new key is appended at the end.
 */
final class RecompConfig {
    static final String FILE_NAME = "recomp.cfg";
    static final String HEADER = "# Written by the launcher's Settings screen; lines you add yourself are kept.";

    /** One value of an option: the string for each of the option's keys, null = no line (the runtime default). */
    static final class Choice {
        final int label;
        final String[] values;

        Choice(int label, String... values) {
            this.label = label;
            this.values = values;
        }

        boolean isDefault() {
            for (String v : values) {
                if (v != null)
                    return false;
            }
            return true;
        }
    }

    enum Section { GAME, GRAPHICS, AUDIO, ADVANCED }

    static final class Option {
        final Section section;
        /** The row's label, and the description the Settings screen shows for the focused row. */
        final int title, detail;
        final String[] keys;
        final Choice[] choices;
        /** Offered only by an APK built with frame generation (-PxbaLsfg=1 with the bridge: R.bool.xba_lsfg). */
        boolean needsLsfg;
        /**
         * Read by every console's runtime (XBA-181: the GBA, PS1 and N64 ones too), not only the Xbox's: Settings shows
         * it under « All consoles », the others under « Xbox only ».
         */
        boolean shared;

        Option(Section section, int title, int detail, String[] keys, Choice... choices) {
            this.section = section;
            this.title = title;
            this.detail = detail;
            this.keys = keys;
            this.choices = choices;
        }

        Option lsfgOnly() {
            needsLsfg = true;
            return this;
        }

        Option shared() {
            shared = true;
            return this;
        }

        int defaultIndex() {
            for (int i = 0; i < choices.length; i++) {
                if (choices[i].isDefault())
                    return i;
            }
            return 0;
        }
    }

    /** The console's widescreen and 480p settings (XBA-121). */
    static final String ENV_WIDESCREEN = "RECOMP_WIDESCREEN";
    static final String ENV_480P = "RECOMP_480P";
    /** « 0 » stops every pad motor (XBA-181: the Xbox's emulated USB pad, the PS1 DualShock, the N64 Rumble Pak). */
    static final String ENV_RUMBLE = "RECOMP_RUMBLE";
    /** Rewind of the GBA, PS1 and N64 runtimes (XBA-194): « 1 » / « 0 », unset = on but for the N64; its memory in MB. */
    static final String ENV_REWIND = "RECOMP_REWIND";
    static final String ENV_REWIND_MB = "RECOMP_REWIND_MB";

    private static String[] keys(String... k) {
        return k;
    }

    /**
     * The options the Settings screen offers. Each one exists in the runtime (docs/runtime/environment-variables.md)
     * and matters to a player on Android; bring-up and debug switches stay out.
     */
    static final Option[] OPTIONS = {
        // GameLanguage (XBA-77): unset = the Android language when the Xbox has it, set by ShellActivity.
        new Option(Section.GAME, R.string.OPT_LANGUAGE, R.string.OPT_LANGUAGE_DETAIL,
                   keys(GameLanguage.ENV),
                   new Choice(R.string.VAL_LANG_SYSTEM, (String)null), new Choice(R.string.LANG_EN, "en"),
                   new Choice(R.string.LANG_FR, "fr"), new Choice(R.string.LANG_DE, "de"),
                   new Choice(R.string.LANG_ES, "es"), new Choice(R.string.LANG_IT, "it"),
                   new Choice(R.string.LANG_PT, "pt"), new Choice(R.string.LANG_JA, "ja"),
                   new Choice(R.string.LANG_KO, "ko"), new Choice(R.string.LANG_ZH, "zh")),
        // XBA-181: the pad's motors, for every console (the Xbox's USB pad, the PS1 DualShock, the N64 Rumble Pak).
        new Option(Section.GAME, R.string.OPT_RUMBLE, R.string.OPT_RUMBLE_DETAIL, keys(ENV_RUMBLE),
                   new Choice(R.string.VAL_ON, (String)null), new Choice(R.string.VAL_OFF, "0")).shared(),
        // XBA-194: rewind for every console (src/platform/recomp_rewind.h; the Xbox's, xbox_state_service.c): on by
        // default but for the N64 and the GameCube, whose captures cost more; the history's memory cap. A game can have
        // its own (GameSettings).
        new Option(Section.GAME, R.string.OPT_REWIND, R.string.OPT_REWIND_DETAIL, keys(ENV_REWIND),
                   new Choice(R.string.VAL_REWIND_AUTO, (String)null), new Choice(R.string.VAL_ON, "1"),
                   new Choice(R.string.VAL_OFF, "0")).shared(),
        new Option(Section.GAME, R.string.OPT_REWIND_MB, R.string.OPT_REWIND_MB_DETAIL, keys(ENV_REWIND_MB),
                   new Choice(R.string.VAL_MB_32, "32"), new Choice(R.string.VAL_MB_64, (String)null),
                   new Choice(R.string.VAL_MB_128, "128"), new Choice(R.string.VAL_MB_256, "256")).shared(),
        new Option(Section.GRAPHICS, R.string.OPT_VSYNC, R.string.OPT_VSYNC_DETAIL,
                   keys("RECOMP_PRESENT_VSYNC"),
                   new Choice(R.string.VAL_ON, (String)null), new Choice(R.string.VAL_OFF, "0")).shared(),
        // XBA-181: the GBA, PS1 and N64 picture (ConsoleVideo; a game can have its own, scale_<KEY> / filter_<KEY>).
        new Option(Section.GRAPHICS, R.string.OPT_CSCALE, R.string.OPT_CSCALE_DETAIL, keys(ConsoleVideo.ENV_SCALE),
                   new Choice(R.string.VAL_CSCALE_ASPECT, (String)null),
                   new Choice(R.string.VAL_CSCALE_INTEGER, ConsoleVideo.SCALE_INTEGER),
                   new Choice(R.string.VAL_CSCALE_STRETCH, ConsoleVideo.SCALE_STRETCH)).shared(),
        new Option(Section.GRAPHICS, R.string.OPT_CFILTER, R.string.OPT_CFILTER_DETAIL, keys(ConsoleVideo.ENV_FILTER),
                   new Choice(R.string.VAL_CFILTER_CONSOLE, (String)null),
                   new Choice(R.string.VAL_CFILTER_NEAREST, ConsoleVideo.FILTER_NEAREST),
                   new Choice(R.string.VAL_CFILTER_LINEAR, ConsoleVideo.FILTER_LINEAR)).shared(),
        // XBA-184: the PS1 and N64 internal resolution (ConsoleVideo; a game can have its own, upscale_<KEY>).
        new Option(Section.GRAPHICS, R.string.OPT_UPSCALE, R.string.OPT_UPSCALE_DETAIL, keys(ConsoleVideo.ENV_UPSCALE),
                   new Choice(R.string.VAL_UPSCALE_NATIVE, (String)null),
                   new Choice(R.string.VAL_UPSCALE_2X, ConsoleVideo.UPSCALE_2X),
                   new Choice(R.string.VAL_UPSCALE_4X, ConsoleVideo.UPSCALE_4X),
                   new Choice(R.string.VAL_UPSCALE_8X, ConsoleVideo.UPSCALE_8X)).shared(),
        // XBA-195: the GBA, PS1 and N64 picture shader, its strength, the CRT's curvature and the LCD's ghosting.
        new Option(Section.GRAPHICS, R.string.OPT_CSHADER, R.string.OPT_CSHADER_DETAIL, keys(ConsoleVideo.ENV_SHADER),
                   new Choice(R.string.VAL_CSHADER_NONE, (String)null),
                   new Choice(R.string.VAL_CSHADER_SHARP, ConsoleVideo.SHADER_SHARP),
                   new Choice(R.string.VAL_CSHADER_SCANLINES, ConsoleVideo.SHADER_SCANLINES),
                   new Choice(R.string.VAL_CSHADER_CRT, ConsoleVideo.SHADER_CRT),
                   new Choice(R.string.VAL_CSHADER_LCD, ConsoleVideo.SHADER_LCD),
                   new Choice(R.string.VAL_CSHADER_FXAA, ConsoleVideo.SHADER_FXAA),
                   new Choice(R.string.VAL_CSHADER_CAS, ConsoleVideo.SHADER_CAS)).shared(),
        new Option(Section.GRAPHICS, R.string.OPT_CSHADER_STRENGTH, R.string.OPT_CSHADER_STRENGTH_DETAIL,
                   keys(ConsoleVideo.ENV_SHADER_STRENGTH),
                   new Choice(R.string.VAL_CSHADER_STRENGTH_DEFAULT, (String)null),
                   new Choice(R.string.VAL_PERCENT_25, "25"), new Choice(R.string.VAL_PERCENT_50, "50"),
                   new Choice(R.string.VAL_PERCENT_75, "75"), new Choice(R.string.VAL_PERCENT_100, "100")).shared(),
        new Option(Section.GRAPHICS, R.string.OPT_CSHADER_CURVE, R.string.OPT_CSHADER_CURVE_DETAIL,
                   keys(ConsoleVideo.ENV_SHADER_CURVATURE),
                   new Choice(R.string.VAL_OFF, (String)null), new Choice(R.string.VAL_LEVEL_SLIGHT, "30"),
                   new Choice(R.string.VAL_LEVEL_STRONG, "70")).shared(),
        new Option(Section.GRAPHICS, R.string.OPT_CSHADER_GHOST, R.string.OPT_CSHADER_GHOST_DETAIL,
                   keys(ConsoleVideo.ENV_SHADER_GHOSTING),
                   new Choice(R.string.VAL_OFF, (String)null), new Choice(R.string.VAL_LEVEL_SLIGHT, "30"),
                   new Choice(R.string.VAL_LEVEL_STRONG, "60")).shared(),
        new Option(Section.GRAPHICS, R.string.OPT_GPU, R.string.OPT_GPU_DETAIL,
                   keys("RECOMP_GPU"),
                   new Choice(R.string.VAL_AUTO, (String)null), new Choice(R.string.VAL_GPU_CPU, "0"),
                   new Choice(R.string.VAL_GPU_GL, "1")),
        new Option(Section.GRAPHICS, R.string.OPT_GPU_FPS_CAP, R.string.OPT_GPU_FPS_CAP_DETAIL, keys("RECOMP_GPU_FPS_CAP"),
                   new Choice(R.string.VAL_AUTO, (String)null), new Choice(R.string.VAL_FPS_30, "30"),
                   new Choice(R.string.VAL_FPS_60, "60"), new Choice(R.string.VAL_OFF, "0")),
        new Option(Section.GRAPHICS, R.string.OPT_GPU_SCALE, R.string.OPT_GPU_SCALE_DETAIL, keys("RECOMP_GPU_SCALE"),
                   new Choice(R.string.VAL_AUTO, (String)null), new Choice(R.string.VAL_SCALE_1, "1"),
                   new Choice(R.string.VAL_SCALE_2, "2"), new Choice(R.string.VAL_SCALE_3, "3"),
                   new Choice(R.string.VAL_SCALE_FIT, "auto")),
        // Aspect modes (docs/technical/gpu-backend.md « Aspect modes »): auto = 16:9 where the title draws wide frames.
        new Option(Section.GRAPHICS, R.string.OPT_ASPECT, R.string.OPT_ASPECT_DETAIL,
                   keys("RECOMP_ASPECT"),
                   new Choice(R.string.VAL_AUTO, (String)null), new Choice(R.string.VAL_ASPECT_4_3, "4:3"),
                   new Choice(R.string.VAL_ASPECT_16_9, "16:9")),
        // The console's video settings (XBA-121), read by the title from the EEPROM and the AV capabilities
        // (src/kernel/kernel_xbox.c): off is the runtime default, so off writes no line.
        new Option(Section.GRAPHICS, R.string.OPT_WIDESCREEN, R.string.OPT_WIDESCREEN_DETAIL, keys(ENV_WIDESCREEN),
                   new Choice(R.string.VAL_OFF, (String)null), new Choice(R.string.VAL_ON, "1")),
        new Option(Section.GRAPHICS, R.string.OPT_480P, R.string.OPT_480P_DETAIL, keys(ENV_480P),
                   new Choice(R.string.VAL_OFF, (String)null), new Choice(R.string.VAL_ON, "1")),
        // Frame generation (docs/technical/framegen.md): RECOMP_LSFG_DIR, the app-private shader cache, is written by
        // « Choose Lossless.dll » (LSFG_DIR_KEY), not by this option. XBA-186: every console reads it (the GBA, PS1
        // and N64 window generates between the game's distinct pictures; a game can have its own, framegen_<KEY>).
        new Option(Section.GRAPHICS, R.string.OPT_LSFG, R.string.OPT_LSFG_DETAIL,
                   keys("RECOMP_LSFG", "RECOMP_LSFG_MULT", "RECOMP_LSFG_PERF"),
                   new Choice(R.string.VAL_OFF, null, null, null), new Choice(R.string.VAL_LSFG_2X, "1", "2", null),
                   new Choice(R.string.VAL_LSFG_2X_PERF, "1", "2", "1")).lsfgOnly().shared(),
        new Option(Section.GRAPHICS, R.string.OPT_TEX, R.string.OPT_TEX_DETAIL,
                   keys("RECOMP_TEX_BILINEAR", "RECOMP_TEX_ANISO"),
                   new Choice(R.string.VAL_TEX_SHARP, null, null), new Choice(R.string.VAL_TEX_SMOOTH, "1", null),
                   new Choice(R.string.VAL_TEX_ANISO8, null, "8"), new Choice(R.string.VAL_TEX_ANISO16, null, "16")),
        new Option(Section.AUDIO, R.string.OPT_HRTF, R.string.OPT_HRTF_DETAIL,
                   keys("RECOMP_APU_HRTF"),
                   new Choice(R.string.VAL_ON, (String)null), new Choice(R.string.VAL_OFF, "0")),
        new Option(Section.AUDIO, R.string.OPT_AUDIO_BUF, R.string.OPT_AUDIO_BUF_DETAIL,
                   keys("RECOMP_AUDIO_BUF_SAMPLES"),
                   new Choice(R.string.VAL_BUF_SHORT, "256"), new Choice(R.string.VAL_BUF_NORMAL, (String)null),
                   new Choice(R.string.VAL_BUF_LONG, "1024"), new Choice(R.string.VAL_BUF_LONGEST, "2048")).shared(),
        new Option(Section.ADVANCED, R.string.OPT_RASTER, R.string.OPT_RASTER_DETAIL,
                   keys("RECOMP_RASTER_THREADS"),
                   new Choice(R.string.VAL_AUTO, (String)null), new Choice(R.string.VAL_1, "1"),
                   new Choice(R.string.VAL_2, "2"), new Choice(R.string.VAL_4, "4"), new Choice(R.string.VAL_6, "6"),
                   new Choice(R.string.VAL_8, "8")),
        new Option(Section.ADVANCED, R.string.OPT_BIG_CORES, R.string.OPT_BIG_CORES_DETAIL, keys("RECOMP_BIG_CORES"),
                   new Choice(R.string.VAL_ON, (String)null), new Choice(R.string.VAL_OFF, "0")),
        new Option(Section.ADVANCED, R.string.OPT_SELFTEST, R.string.OPT_SELFTEST_DETAIL, keys("RECOMP_SHELL_SELFTEST"),
                   new Choice(R.string.VAL_OFF, (String)null), new Choice(R.string.VAL_ON, "1")),
    };

    /** The shader cache directory of the imported Lossless.dll (getFilesDir()/lsfg/<key>), for the runtime. */
    static final String LSFG_DIR_KEY = "RECOMP_LSFG_DIR";

    /** The options this APK offers (frame generation only when it is built in). */
    static List<Option> shownOptions(boolean lsfg) {
        List<Option> out = new ArrayList<>();
        for (Option o : OPTIONS) {
            if (!o.needsLsfg || lsfg)
                out.add(o);
        }
        return out;
    }

    /**
     * The options of one Settings tab, in the order shown (XBA-181): in Graphics and Audio, where both kinds are, the
     * ones every console reads first, then the Xbox's own (each group under its label); elsewhere as listed.
     */
    static List<Option> tabOptions(List<Option> shown, Section section) {
        List<Option> all = new ArrayList<>(), out = new ArrayList<>();
        for (Option o : shown) {
            if (o.section == section)
                all.add(o);
        }
        if (!grouped(all, section))
            return all;
        for (Option o : all)
            if (o.shared)
                out.add(o);
        for (Option o : all)
            if (!o.shared)
                out.add(o);
        return out;
    }

    /** The tab shows « All consoles » and « Xbox only » labels: Graphics or Audio with options of both kinds. */
    static boolean grouped(List<Option> tab, Section section) {
        if (section != Section.GRAPHICS && section != Section.AUDIO)
            return false;
        boolean any = false, xbox = false;
        for (Option o : tab) {
            if (o.section != section)
                continue;
            any |= o.shared;
            xbox |= !o.shared;
        }
        return any && xbox;
    }

    private final File file;
    /** The comment line put above the first key the launcher appends (HEADER for recomp.cfg). */
    private final String header;
    private final List<String> lines = new ArrayList<>();
    private boolean existed;

    private RecompConfig(File file, String header) {
        this.file = file;
        this.header = header;
    }

    /** Reads dir/recomp.cfg; a missing or unreadable file reads as empty (every option at its default). */
    static RecompConfig load(File dir) {
        return loadFile(new File(dir, FILE_NAME), HEADER);
    }

    /**
     * Reads another file of the same syntax (XBA-188: a game's own settings, GameSettings), header the comment put
     * above the first key appended.
     */
    static RecompConfig loadFile(File f, String header) {
        RecompConfig c = new RecompConfig(f, header);
        if (!c.file.isFile())
            return c;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(c.file),
                                                                         StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null)
                c.lines.add(line);
            c.existed = true;
        } catch (IOException ignored) {
            c.lines.clear();
        }
        return c;
    }

    File file() {
        return file;
    }

    /** The key of an active line ("KEY=VALUE", not blank or a comment), trimmed; null otherwise. */
    private static String keyOf(String line) {
        String s = line.trim();
        if (s.isEmpty() || s.charAt(0) == '#')
            return null;
        int eq = s.indexOf('=');
        return eq < 0 ? null : s.substring(0, eq).trim();
    }

    /** The value the runtime will see for key (its first line), or null when the file does not set it. */
    String get(String key) {
        for (String line : lines) {
            if (key.equals(keyOf(line))) {
                String s = line.trim();
                return s.substring(s.indexOf('=') + 1).trim();
            }
        }
        return null;
    }

    /** The index of the option's current choice, or -1 when the file holds a value the list does not offer. */
    int choiceIndex(Option o) {
        for (int i = 0; i < o.choices.length; i++) {
            boolean match = true;
            for (int k = 0; k < o.keys.length && match; k++) {
                String want = o.choices[i].values[k], have = get(o.keys[k]);
                match = want == null ? have == null : want.equals(have);
            }
            if (match)
                return i;
        }
        return -1;
    }

    /** The option's lines as the file holds them, "KEY=VALUE" joined by spaces; "" when none is set. */
    String rawValue(Option o) {
        StringBuilder sb = new StringBuilder();
        for (String k : o.keys) {
            String v = get(k);
            if (v != null)
                sb.append(sb.length() == 0 ? "" : "  ").append(k).append('=').append(v);
        }
        return sb.toString();
    }

    void set(Option o, Choice c) {
        for (int k = 0; k < o.keys.length; k++)
            set(o.keys[k], c.values[k]);
    }

    /** Sets key to value (null removes it), in place of its first line. */
    void set(String key, String value) {
        boolean placed = false;
        for (int i = 0; i < lines.size(); i++) {
            if (!key.equals(keyOf(lines.get(i))))
                continue;
            if (value != null && !placed) {
                lines.set(i, key + "=" + value);
                placed = true;
            } else {
                lines.remove(i--);
            }
        }
        if (value != null && !placed) {
            if (!lines.contains(header)) {
                if (!lines.isEmpty() && !lines.get(lines.size() - 1).trim().isEmpty())
                    lines.add("");
                lines.add(header);
            }
            lines.add(key + "=" + value);
        }
    }

    /** Every managed option back to its default; the player's own lines stay. */
    void resetAll() {
        for (Option o : OPTIONS)
            set(o, o.choices[o.defaultIndex()]);
    }

    /** True when the file sets key, to any value (an empty one too: « KEY= »). */
    boolean has(String key) {
        return get(key) != null;
    }

    /** The keys the file sets, in file order, each once. */
    List<String> keys() {
        List<String> out = new ArrayList<>();
        for (String line : lines) {
            String k = keyOf(line);
            if (k != null && !out.contains(k))
                out.add(k);
        }
        return out;
    }

    /** True when no option is away from its default. */
    boolean allDefault() {
        for (Option o : OPTIONS) {
            if (choiceIndex(o) != o.defaultIndex())
                return false;
        }
        return true;
    }

    /**
     * Writes the file through recomp.cfg.tmp and a rename, so that a game starting meanwhile reads the old or the
     * new file, never half of one. Nothing is created when there was no file and every option is at its default.
     */
    void save() throws IOException {
        boolean onlyHeader = true;
        for (String l : lines) {
            if (!l.trim().isEmpty() && !l.equals(header)) {
                onlyHeader = false;
                break;
            }
        }
        if (!existed && onlyHeader)
            return;
        File dir = file.getParentFile();
        if (dir != null && !(dir.mkdirs() || dir.isDirectory()))
            throw new IOException("cannot create " + dir);
        File tmp = new File(dir, file.getName() + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            for (String l : lines)
                w.write(l + "\n");
        }
        if (!tmp.renameTo(file)) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw new IOException("cannot replace " + file);
        }
        existed = true;
    }
}
