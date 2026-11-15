package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import android.app.Application;
import android.view.KeyEvent;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * XBA-190: the hotkeys' model (src/platform/recomp_hotkeys.h's RECOMP_HOTKEYS text), the conflicts, the capture of a
 * chord from key events, the menu's combo on the player's chord, and where they are kept (recomp.cfg, the game's own
 * settings, the game's environment).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class HotkeysTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static Hotkeys.Chord chord(String t) {
        return Hotkeys.Chord.parse(t);
    }

    @Test
    public void defaultsAreTheFixedChords() {
        Hotkeys h = new Hotkeys();
        assertTrue(h.isDefault());
        assertEquals("", h.format());
        assertEquals("back+right_trigger", h.get(Hotkeys.Action.FF_HOLD).text());
        assertEquals("back+right_shoulder", h.get(Hotkeys.Action.QSAVE).text());
        assertEquals("back+left_shoulder", h.get(Hotkeys.Action.QLOAD).text());
        assertEquals("back+start", h.get(Hotkeys.Action.MENU).text());
        assertEquals("back+left_trigger", h.get(Hotkeys.Action.REWIND).text());   // XBA-194, held
        assertTrue(h.get(Hotkeys.Action.FF_TOGGLE).isNone());
        assertTrue(h.get(Hotkeys.Action.SLOT_NEXT).isNone());
    }

    @Test
    public void rewindAction() {
        // XBA-194: recomp_hotkeys.c's « rewind », in recomp_hotkeys.h's order (after slot_prev, before menu).
        assertSame(Hotkeys.Action.REWIND, Hotkeys.Action.of("rewind"));
        Hotkeys h = Hotkeys.parse("rewind:select+lb,qload:none");
        assertEquals(new Hotkeys.Chord(PadMapping.Source.BACK, PadMapping.Source.LEFT_SHOULDER),
                     h.get(Hotkeys.Action.REWIND));
        assertEquals("qload:none,rewind:back+left_shoulder", h.format());
        // Its chord does not clash with the others by default; it shows for every console with save states.
        Hotkeys d = new Hotkeys();
        for (Console c : new Console[] { Console.GBA, Console.PS1, Console.N64 }) {
            assertTrue(Hotkeys.actions(c).contains(Hotkeys.Action.REWIND));
            assertTrue(d.sharing(Hotkeys.Action.REWIND, c).isEmpty());
        }
        assertFalse(Hotkeys.actions(Console.XBOX).contains(Hotkeys.Action.REWIND));
        assertEquals(R.string.HK_REWIND, Hotkeys.label(Hotkeys.Action.REWIND));
    }

    @Test
    public void parseAndFormat() {
        Hotkeys h = Hotkeys.parse("qsave:select+y; ff_toggle=l3+r3 qload:none,slot_next:right,save1:f9,bogus:x");
        assertEquals(new Hotkeys.Chord(PadMapping.Source.BACK, PadMapping.Source.NORTH), h.get(Hotkeys.Action.QSAVE));
        assertTrue(h.get(Hotkeys.Action.QLOAD).isNone());
        assertEquals("dpad_right", h.get(Hotkeys.Action.SLOT_NEXT).text());
        // recomp_hotkeys.h's order, long names; the runtime's own entries kept as they are.
        assertEquals("ff_toggle:left_stick_click+right_stick_click,qsave:back+north,qload:none,slot_next:dpad_right,"
                     + "save1:f9,bogus:x", h.format());
        assertEquals(h.format(), Hotkeys.parse(h.format()).format());
        assertEquals(4, h.changedCount());
        // Bad chords are dropped (the default stays).
        Hotkeys bad = Hotkeys.parse("qsave:back+back,qload:back+nothing,ff_hold:a+b+x,menu");
        assertEquals("", bad.format());
        assertNull(chord("start+start"));
        assertTrue(chord("").isNone());
        assertTrue(chord("NONE").isNone());
        assertEquals(new Hotkeys.Chord(null, PadMapping.Source.RIGHT_TRIGGER), chord("rt"));
        // A chord back to its default leaves the text.
        h.set(Hotkeys.Action.QSAVE, Hotkeys.def(Hotkeys.Action.QSAVE));
        assertFalse(h.format().contains("qsave"));
        h.reset();
        assertTrue(h.isDefault());
    }

    @Test
    public void conflicts() {
        Hotkeys h = Hotkeys.parse("ff_toggle:right_shoulder+back,slot_next:south,menu:back+right_trigger");
        // Same buttons in either order: both happen in the runtime.
        assertEquals(Collections.singletonList(Hotkeys.Action.FF_TOGGLE), h.sharing(Hotkeys.Action.QSAVE, Console.GBA));
        assertEquals(Collections.singletonList(Hotkeys.Action.QSAVE), h.sharing(Hotkeys.Action.FF_TOGGLE, Console.GBA));
        // The menu's chord against the fast-forward hold.
        assertEquals(Collections.singletonList(Hotkeys.Action.MENU), h.sharing(Hotkeys.Action.FF_HOLD, Console.PS1));
        // An Xbox title has only the menu: nothing to share with.
        assertTrue(h.sharing(Hotkeys.Action.MENU, Console.XBOX).isEmpty());
        // A button alone the game's mapping uses (the GBA's A is south + north by default).
        int gba = Hotkeys.sourcesOf(new ConsolePadMap(Console.GBA));
        assertSame(PadMapping.Source.SOUTH, h.gameNeeds(Hotkeys.Action.SLOT_NEXT, gba));
        assertTrue(h.hasConflict(Hotkeys.Action.SLOT_NEXT, Console.GBA, gba));
        assertNull("a modifier keeps the button alone for the game", h.gameNeeds(Hotkeys.Action.QSAVE, gba));
        // Not mapped: no clash (the GBA has no L3).
        h.set(Hotkeys.Action.SLOT_NEXT, chord("l3"));
        assertNull(h.gameNeeds(Hotkeys.Action.SLOT_NEXT, gba));
        assertFalse(h.hasConflict(Hotkeys.Action.SLOT_NEXT, Console.GBA, gba));
        // The Xbox's mapping uses every button.
        int xbox = Hotkeys.sourcesOf(new PadMapping());
        assertSame(PadMapping.Source.LEFT_STICK_CLICK, h.gameNeeds(Hotkeys.Action.SLOT_NEXT, xbox));
        // The actions a console shows.
        assertEquals(Arrays.asList(Hotkeys.Action.values()), Hotkeys.actions(Console.N64));
        assertEquals(Collections.singletonList(Hotkeys.Action.MENU), Hotkeys.actions(Console.XBOX));
    }

    @Test
    public void keysAndSources() {
        for (PadMapping.Source s : PadMapping.Source.values()) {
            int k = Hotkeys.keyOf(s);
            if (k >= 0)
                assertSame(s.name(), s, Hotkeys.sourceOfKey(k));
        }
        assertEquals(-1, Hotkeys.keyOf(PadMapping.Source.LSTICK_UP));
        assertSame(PadMapping.Source.BACK, Hotkeys.sourceOfKey(KeyEvent.KEYCODE_BACK));   // a pad's Back is Select
        assertNull(Hotkeys.sourceOfKey(KeyEvent.KEYCODE_VOLUME_UP));
        assertSame(PadMapping.Source.RIGHT_TRIGGER, Hotkeys.source("RT"));
        assertSame(PadMapping.Source.BACK, Hotkeys.source("select"));
        assertNull(Hotkeys.source("none"));
    }

    /** « Press the buttons »: the first down is the modifier, the second makes the chord; one alone on its release. */
    @Test
    public void capture() {
        Hotkeys.Capture c = new Hotkeys.Capture();
        assertTrue(c.active());
        assertNull(c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true));
        assertNull("a repeat of the same button", c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true));
        assertNull("not a pad button", c.key(KeyEvent.KEYCODE_VOLUME_UP, true));
        assertEquals(chord("back+north"), c.key(KeyEvent.KEYCODE_BUTTON_Y, true));
        assertTrue("eats the releases", c.active());
        assertNull(c.key(KeyEvent.KEYCODE_BUTTON_X, true));   // a third: ignored, eaten
        assertNull(c.key(KeyEvent.KEYCODE_BUTTON_Y, false));
        assertNull(c.key(KeyEvent.KEYCODE_BUTTON_SELECT, false));
        assertTrue(c.active());
        assertNull(c.key(KeyEvent.KEYCODE_BUTTON_X, false));
        assertFalse(c.active());
        assertEquals(chord("back+north"), c.result());

        // One button alone: on its release.
        c = new Hotkeys.Capture();
        assertNull(c.key(KeyEvent.KEYCODE_BUTTON_THUMBL, true));
        assertEquals(chord("left_stick_click"), c.key(KeyEvent.KEYCODE_BUTTON_THUMBL, false));
        assertFalse(c.active());

        // Triggers as axes: Select held, then the right trigger past half.
        c = new Hotkeys.Capture();
        c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true);
        assertNull(c.triggers(0f, 0.3f));
        assertEquals(chord("back+right_trigger"), c.triggers(0f, 0.9f));
        c.key(KeyEvent.KEYCODE_BUTTON_SELECT, false);
        assertTrue(c.active());
        c.triggers(0f, 0f);
        assertFalse(c.active());
    }

    /** The menu's combo listens to the player's chord: two buttons, one alone, none, or Select + Start as a fallback. */
    @Test
    public void menuComboOnTheChord() {
        MenuTriggers.Combo c = new MenuTriggers.Combo();
        c.setChord(chord("left_shoulder+right_shoulder"));
        assertFalse(c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true, 0));
        assertFalse(c.key(KeyEvent.KEYCODE_BUTTON_START, true, 0));
        assertTrue(c.key(KeyEvent.KEYCODE_BUTTON_R1, true, 1000));
        assertFalse(c.held());
        assertTrue(c.key(KeyEvent.KEYCODE_BUTTON_L1, true, 1100));
        assertEquals(1100 + MenuTriggers.COMBO_MS, c.dueAt());
        c.key(KeyEvent.KEYCODE_BUTTON_R1, false, 1200);
        assertFalse(c.held());

        c.setChord(chord("right_stick_click"));   // one button held alone
        c.key(KeyEvent.KEYCODE_BUTTON_THUMBR, true, 2000);
        assertTrue(c.due(2000 + MenuTriggers.COMBO_MS));

        c.setChord(Hotkeys.Chord.NONE);
        assertFalse(c.enabled());
        assertFalse(c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true, 0));
        assertFalse(c.key(KeyEvent.KEYCODE_BUTTON_START, true, 0));
        assertFalse(c.held());

        c.setChord(chord("back+left_stick_up"));   // a stick direction has no key: Select + Start
        c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true, 0);
        c.key(KeyEvent.KEYCODE_BUTTON_START, true, 10);
        assertTrue(c.held());
    }

    /**
     * Kept in recomp.cfg for every game and in the game's own file; the game's environment exports its own text (« »:
     * the defaults for it, recomp.cfg's line skipped), nothing when it follows recomp.cfg; for an Xbox title too.
     */
    @Test
    public void storedAndExported() throws Exception {
        Application app = RuntimeEnvironment.getApplication();
        File ext = tmp.newFolder("ext");
        RecompConfig cfg = RecompConfig.load(ext);
        Hotkeys g = Hotkeys.parse("qsave:back+north");
        Hotkeys.storeGlobal(cfg, g);
        cfg.save();
        assertEquals("qsave:back+north", RecompConfig.load(ext).get(Hotkeys.KEY));
        assertEquals("qsave:back+north", Hotkeys.effective(app, ext, "GBA-ABCD").format());

        assertTrue(Hotkeys.storeGame(app, "gba-abcd", Hotkeys.parse("ff_toggle:back+east")));
        assertEquals("ff_toggle:back+east", Hotkeys.gameText(app, "GBA-ABCD"));
        assertEquals("ff_toggle:back+east", Hotkeys.effective(app, ext, "GBA-ABCD").format());
        RecompConfig game = GameSettings.load(app, "GBA-ABCD");
        GameSettings.Env env = GameSettings.runEnv(Console.GBA, RecompConfig.load(ext), game, Locale.ROOT, false);
        assertEquals("ff_toggle:back+east", env.value(Hotkeys.KEY));
        assertTrue(env.overridden.contains(Hotkeys.KEY));

        // The game's own defaults: exported as unset, recomp.cfg's line skipped for it.
        assertTrue(Hotkeys.storeGame(app, "GBA-ABCD", new Hotkeys()));
        env = GameSettings.runEnv(Console.GBA, RecompConfig.load(ext), GameSettings.load(app, "GBA-ABCD"), Locale.ROOT,
                                  false);
        assertNull(env.value(Hotkeys.KEY));
        assertTrue(env.value(GameSettings.SKIP_ENV).contains(Hotkeys.KEY));
        assertTrue(Hotkeys.effective(app, ext, "GBA-ABCD").isDefault());

        // Following every game's again: unset, recomp.cfg decides.
        assertTrue(Hotkeys.storeGame(app, "GBA-ABCD", null));
        assertNull(Hotkeys.gameText(app, "GBA-ABCD"));
        env = GameSettings.runEnv(Console.GBA, RecompConfig.load(ext), GameSettings.load(app, "GBA-ABCD"), Locale.ROOT,
                                  false);
        assertNull(env.value(Hotkeys.KEY));
        assertFalse(env.overridden.contains(Hotkeys.KEY));

        // An Xbox title exports its menu chord.
        Hotkeys.storeGame(app, "4D530004", Hotkeys.parse("menu:left_shoulder+right_shoulder"));
        env = GameSettings.runEnv(Console.XBOX, null, GameSettings.load(app, "4D530004"), Locale.ROOT, false);
        assertEquals("menu:left_shoulder+right_shoulder", env.value(Hotkeys.KEY));
        List<String> all = GameSettings.allExportedKeys();
        assertTrue(all.contains(Hotkeys.KEY));
    }
}
