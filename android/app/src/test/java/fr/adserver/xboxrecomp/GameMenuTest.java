package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * The in-game menu (GameMenu): its triggers (Back, Guide, Select + Start held, the edge swipe), the input routing
 * while open (nothing reaches the game), the hold through a fake native bridge, every button released in the game
 * on open and close, Quit's confirmation, Back to launcher, the quick settings; and the drawer rendered over a
 * made-up game frame to build/screenshots/menu-*.png (en, fr, 1080p, 720p).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "en-w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class GameMenuTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private static final int PAD = 7;

    /** What would go to libmain and SDL. */
    static final class FakeHost implements GameMenu.Host {
        final List<Boolean> holds = new ArrayList<>();
        final List<KeyEvent> keys = new ArrayList<>();
        final List<MotionEvent> motions = new ArrayList<>();
        int cancels, overlayReleases, focusBacks, screenshots, shareLogs, launchers, quits, bottomChanges;
        int volume = -1;
        boolean gamepadBack, secondScreen = true;
        int[] stats = { 5994, 412 };

        boolean held() {
            return !holds.isEmpty() && holds.get(holds.size() - 1);
        }

        @Override public void hold(boolean on) { holds.add(on); }
        @Override public void sendKeyToGame(KeyEvent e) { keys.add(e); }
        @Override public void sendMotionToGame(MotionEvent e) { motions.add(MotionEvent.obtain(e)); }
        @Override public void cancelGameTouch(MotionEvent e) { cancels++; }
        @Override public void releaseOverlay() { overlayReleases++; }
        @Override public void restoreGameFocus() { focusBacks++; }
        @Override public boolean fromGamepad(KeyEvent e) { return gamepadBack && e.getDeviceId() == PAD; }
        @Override public void setVolume(int percent) { volume = percent; }
        @Override public int[] stats() { return stats; }
        @Override public boolean hasSecondScreen() { return secondScreen; }
        @Override public void bottomScreenChanged() { bottomChanges++; }
        @Override public void screenshot() { screenshots++; }
        @Override public void shareLog() { shareLogs++; }
        @Override public void backToLauncher() { launchers++; }
        @Override public void quit() { quits++; }
    }

    /** A stand-in for the game's frame (ours: gradients and shapes), so the dimming shows in the screenshots. */
    static final class FakeGame extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        FakeGame(Context c) {
            super(c);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            p.setShader(new LinearGradient(0, 0, 0, h, 0xFF2E6FB8, 0xFFE8B66A, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, p);
            p.setShader(null);
            p.setColor(0xFF2F5D2A);
            c.drawRect(0, h * 0.72f, w, h, p);
            p.setColor(0xFFF4E3B0);
            c.drawCircle(w * 0.78f, h * 0.24f, h * 0.09f, p);
            p.setColor(0xFF7A3B2E);
            c.drawRect(w * 0.42f, h * 0.48f, w * 0.58f, h * 0.72f, p);
            p.setColor(0xFFFFFFFF);
            p.setTextSize(h * 0.05f);
            c.drawText("GAME FRAME 640 x 480", w * 0.36f, h * 0.12f, p);
        }
    }

    private Activity activity;
    private FakeHost host;
    private GameMenu menu;

    @Before
    public void setUp() {
        build();
    }

    private void build() {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        activity.setContentView(new FakeGame(activity));
        host = new FakeHost();
        menu = new GameMenu(activity, host);
        menu.setAnimate(false);
        menu.attach();
        menu.onStart();
        idle(50);
    }

    @After
    public void tearDown() {
        menu.release();
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private static KeyEvent key(int action, int code) {
        return key(action, code, 1);
    }

    private static KeyEvent key(int action, int code, int device) {
        long t = SystemClock.uptimeMillis();
        return new KeyEvent(t, t, action, code, 0, 0, device, 0, 0, InputDevice.SOURCE_GAMEPAD);
    }

    private boolean down(int code) {
        return menu.dispatchKey(key(KeyEvent.ACTION_DOWN, code));
    }

    private boolean up(int code) {
        return menu.dispatchKey(key(KeyEvent.ACTION_UP, code));
    }

    private void press(int code) {
        down(code);
        up(code);
        idle(20);
    }

    private static MotionEvent stick(float x, float y, float hatX, float hatY) {
        MotionEvent.PointerProperties[] pp = { new MotionEvent.PointerProperties() };
        pp[0].id = 0;
        MotionEvent.PointerCoords[] pc = { new MotionEvent.PointerCoords() };
        pc[0].setAxisValue(MotionEvent.AXIS_X, x);
        pc[0].setAxisValue(MotionEvent.AXIS_Y, y);
        pc[0].setAxisValue(MotionEvent.AXIS_HAT_X, hatX);
        pc[0].setAxisValue(MotionEvent.AXIS_HAT_Y, hatY);
        pc[0].setAxisValue(MotionEvent.AXIS_RTRIGGER, 0.9f);
        long t = SystemClock.uptimeMillis();
        return MotionEvent.obtain(t, t, MotionEvent.ACTION_MOVE, 1, pp, pc, 0, 0, 1, 1, PAD, 0,
                                  InputDevice.SOURCE_JOYSTICK, 0);
    }

    private boolean touch(int action, float x, float y) {
        long t = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(t, t, action, x, y, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        return menu.dispatchTouch(e);
    }

    private File shot(String name) throws Exception {
        idle(400);
        File f = TestSupport.screenshot(activity.getWindow().getDecorView(), "menu-" + name);
        assertTrue(f.length() > 0);
        return f;
    }

    // ---- Triggers --------------------------------------------------------------------------------------------------

    @Test
    public void backOpensAndCloses() {
        assertTrue(down(KeyEvent.KEYCODE_BACK));          // never the game's
        assertTrue(menu.isOpen());
        assertEquals(List.of(true), host.holds);
        assertTrue(up(KeyEvent.KEYCODE_BACK));            // the opening press's release does not close it
        assertTrue(menu.isOpen());
        assertSame(menu.row(GameMenu.Item.RESUME), menu.focusedRow());
        press(KeyEvent.KEYCODE_BACK);
        assertFalse(menu.isOpen());
        assertEquals(List.of(true, false), host.holds);
        assertTrue(host.focusBacks >= 1);
    }

    @Test
    public void aGamepadsBackIsTheGamesSelect() {
        host.gamepadBack = true;
        assertFalse(menu.dispatchKey(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, PAD)));
        assertFalse(menu.dispatchKey(key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, PAD)));
        assertFalse(menu.isOpen());
        assertTrue(host.holds.isEmpty());
        // The system's Back (another device) still opens it.
        assertTrue(menu.dispatchKey(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, -1)));
        assertTrue(menu.isOpen());
    }

    @Test
    public void guideOpensAndStartResumes() {
        assertTrue(down(KeyEvent.KEYCODE_BUTTON_MODE));
        assertTrue(menu.isOpen());
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_MODE));
        assertTrue(menu.isOpen());
        press(KeyEvent.KEYCODE_BUTTON_START);
        assertFalse(menu.isOpen());
        assertFalse(host.held());
    }

    @Test
    public void selectStartHeldHalfASecond() {
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_SELECT));   // the game's too
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_START));
        idle(MenuTriggers.COMBO_MS - 100);
        assertFalse(menu.isOpen());
        idle(150);
        assertTrue(menu.isOpen());
        // Both were seen down by the game: released there at the open, their own releases swallowed.
        assertEquals(2, host.keys.size());
        for (KeyEvent k : host.keys)
            assertEquals(KeyEvent.ACTION_UP, k.getAction());
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_SELECT));
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_START));
        assertTrue(menu.isOpen());                          // Start's release did not resume
        assertEquals(2, host.keys.size());
    }

    @Test
    public void selectStartTooShortOrApart() {
        down(KeyEvent.KEYCODE_BUTTON_SELECT);
        down(KeyEvent.KEYCODE_BUTTON_START);
        idle(300);
        up(KeyEvent.KEYCODE_BUTTON_START);
        idle(500);
        assertFalse(menu.isOpen());
        // Start again: the 0.5 s count starts over.
        down(KeyEvent.KEYCODE_BUTTON_START);
        idle(300);
        assertFalse(menu.isOpen());
        idle(250);
        assertTrue(menu.isOpen());
    }

    @Test
    public void comboStateMachine() {
        MenuTriggers.Combo c = new MenuTriggers.Combo();
        assertTrue(c.key(KeyEvent.KEYCODE_BUTTON_SELECT, true, 1000));
        assertFalse(c.held());
        assertFalse(c.key(KeyEvent.KEYCODE_BUTTON_A, true, 1100));
        c.key(KeyEvent.KEYCODE_BUTTON_START, true, 1200);
        assertEquals(1700, c.dueAt());
        assertFalse(c.due(1699));
        assertTrue(c.due(1700));
        c.key(KeyEvent.KEYCODE_BUTTON_SELECT, false, 1300);
        assertEquals(-1, c.dueAt());
    }

    @Test
    public void edgeSwipeOpens() {
        int w = menu.root.getWidth();
        assertEquals(1920, w);
        assertFalse(touch(MotionEvent.ACTION_DOWN, 12, 500));   // the game sees it begin
        assertFalse(touch(MotionEvent.ACTION_MOVE, 40, 505));
        assertTrue(touch(MotionEvent.ACTION_MOVE, 12 + w * 0.07f, 510));
        assertTrue(menu.isOpen());
        assertEquals(1, host.cancels);                         // and then a cancel
        assertTrue(touch(MotionEvent.ACTION_MOVE, 300, 512));
        assertTrue(touch(MotionEvent.ACTION_UP, 300, 512));
        assertNull(menu.focusedRow());                         // a touch: no focus until a key comes
        // The system's back gesture finishing that swipe does not close it.
        assertTrue(down(KeyEvent.KEYCODE_BACK));
        assertTrue(up(KeyEvent.KEYCODE_BACK));
        assertTrue(menu.isOpen());
        idle(GameMenu.SWIPE_BACK_GUARD_MS);
        press(KeyEvent.KEYCODE_BACK);
        assertFalse(menu.isOpen());
    }

    @Test
    public void notAnEdgeSwipe() {
        // From the middle of the screen.
        touch(MotionEvent.ACTION_DOWN, 900, 500);
        assertFalse(touch(MotionEvent.ACTION_MOVE, 1300, 500));
        touch(MotionEvent.ACTION_UP, 1300, 500);
        // From the edge, but up the screen.
        touch(MotionEvent.ACTION_DOWN, 10, 800);
        assertFalse(touch(MotionEvent.ACTION_MOVE, 60, 500));
        assertFalse(touch(MotionEvent.ACTION_MOVE, 200, 400));
        touch(MotionEvent.ACTION_UP, 200, 400);
        assertFalse(menu.isOpen());
        assertEquals(0, host.cancels);
    }

    @Test
    public void panelMenuButtonToggles() {
        menu.toggle("panel");
        assertTrue(menu.isOpen());
        assertNull(menu.focusedRow());
        menu.toggle("panel");
        assertFalse(menu.isOpen());
        assertEquals(List.of(true, false), host.holds);
    }

    // ---- Routing and release ---------------------------------------------------------------------------------------

    @Test
    public void nothingReachesTheGameWhileOpen() {
        down(KeyEvent.KEYCODE_BACK);
        up(KeyEvent.KEYCODE_BACK);
        // (A, B, Start act on their release: tested on their own.)
        int[] codes = { KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1,
                        KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_THUMBL,
                        KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_W };
        for (int c : codes) {
            assertTrue("down " + c, down(c));
            assertTrue("up " + c, up(c));
        }
        assertTrue(menu.dispatchMotion(stick(0.9f, 0, 0, 0)));
        assertTrue(menu.dispatchMotion(stick(0, 0, 0, 0)));
        // Volume stays the system's.
        assertFalse(down(KeyEvent.KEYCODE_VOLUME_UP));
        assertTrue(host.keys.isEmpty());
        assertTrue(host.motions.isEmpty());
    }

    @Test
    public void openReleasesEverythingTheGameHeld() {
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_A));       // held in the game
        assertFalse(down(KeyEvent.KEYCODE_DPAD_LEFT));
        assertFalse(up(KeyEvent.KEYCODE_DPAD_LEFT));        // already let go
        assertFalse(menu.dispatchMotion(stick(0.8f, -0.6f, 1, 0)));
        int before = host.overlayReleases;
        menu.open("test");
        assertEquals(1, host.keys.size());
        KeyEvent k = host.keys.get(0);
        assertEquals(KeyEvent.KEYCODE_BUTTON_A, k.getKeyCode());
        assertEquals(KeyEvent.ACTION_UP, k.getAction());
        assertEquals(1, host.motions.size());
        MotionEvent m = host.motions.get(0);
        assertEquals(PAD, m.getDeviceId());
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_X), 0);
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_Y), 0);
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_HAT_X), 0);
        assertEquals(0f, m.getAxisValue(MotionEvent.AXIS_RTRIGGER), 0);
        assertTrue((m.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK);
        assertTrue(host.overlayReleases > before);
        // A's physical release comes while open: swallowed, it acts on nothing.
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_A));
        assertTrue(menu.isOpen());
        before = host.overlayReleases;
        menu.close();
        assertTrue(host.overlayReleases > before);
        assertEquals(1, host.keys.size());                  // nothing new to release on close
        // Closed again: keys go to the game.
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_A));
        assertFalse(up(KeyEvent.KEYCODE_BUTTON_A));
    }

    @Test
    public void bClosesOnItsReleaseOnly() {
        menu.open("test");
        assertTrue(down(KeyEvent.KEYCODE_BUTTON_B));
        assertTrue(menu.isOpen());
        assertTrue(up(KeyEvent.KEYCODE_BUTTON_B));
        assertFalse(menu.isOpen());
        // So the release went nowhere near the game, and the next B is the game's.
        assertFalse(down(KeyEvent.KEYCODE_BUTTON_B));
    }

    @Test
    public void stickAndHatNavigate() {
        menu.open("Back");
        assertSame(menu.row(GameMenu.Item.RESUME), menu.focusedRow());
        menu.dispatchMotion(stick(0, 0, 0, 1));             // hat down: one step at once
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        menu.dispatchMotion(stick(0, 0, 0, 1));             // the same direction again: no new step
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        idle(GameMenu.NAV_FIRST_MS + 10);                   // held: repeats (Robolectric's clock runs ahead
        Widgets.MenuRow after = menu.focusedRow();          // with the focus animations, so at least one)
        assertTrue(after != menu.row(GameMenu.Item.SETTINGS) && after != menu.row(GameMenu.Item.RESUME));
        menu.dispatchMotion(stick(0, 0, 0, 0));             // let go: no more steps
        idle(1000);
        assertSame(after, menu.focusedRow());
        menu.row(GameMenu.Item.CONTROLS).requestFocusFromTouch();
        menu.dispatchMotion(stick(0, -0.9f, 0, 0));         // left stick up
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        menu.dispatchMotion(stick(0, 0, 0, 0));
    }

    // ---- Hold, launcher, quit --------------------------------------------------------------------------------------

    @Test
    public void backToLauncherKeepsTheHoldUntilStopped() {
        menu.open("Back");
        menu.row(GameMenu.Item.LAUNCHER).performClick();
        assertFalse(menu.isOpen());
        assertEquals(1, host.launchers);
        assertTrue(host.held());                            // no frame runs between the menu and the background
        assertEquals(View.GONE, menu.drawer.getVisibility());
        menu.onStop();                                      // the background hold has taken over
        assertFalse(host.held());
        assertEquals(List.of(true, false), host.holds);
        menu.onResume();                                    // Play: back to the running game
        assertEquals(List.of(true, false), host.holds);
        assertFalse(menu.isOpen());
    }

    @Test
    public void quitNeedsASecondPress() {
        menu.open("Back");
        Widgets.MenuRow q = menu.row(GameMenu.Item.QUIT);
        q.performClick();
        assertEquals(0, host.quits);
        assertTrue(menu.quitArmed());
        assertEquals(activity.getString(R.string.MENU_QUIT_CONFIRM), q.title);
        q.performClick();
        assertEquals(1, host.quits);
    }

    @Test
    public void quitArmingRunsOut() {
        menu.open("Back");
        Widgets.MenuRow q = menu.row(GameMenu.Item.QUIT);
        q.performClick();
        idle(GameMenu.QUIT_ARM_MS + 100);
        assertFalse(menu.quitArmed());
        assertEquals(activity.getString(R.string.MENU_QUIT), q.title);
        q.performClick();
        assertEquals(0, host.quits);
        assertTrue(menu.quitArmed());
        // Moving away disarms it too.
        q.requestFocusFromTouch();
        assertTrue(menu.quitArmed());
        press(KeyEvent.KEYCODE_DPAD_UP);
        assertSame(menu.row(GameMenu.Item.LAUNCHER), menu.focusedRow());
        assertFalse(menu.quitArmed());
    }

    /** A game's theme (read once at game start, ShellActivity): the menu in its colours. */
    @Test
    public void themedMenu() throws Exception {
        menu.release();
        int[] px = new int[64 * 64];
        java.util.Arrays.fill(px, 0xFF2850DC);
        Theme.apply(new Theme.Colors(GamePalette.build(GamePalette.analysePixels(px), Double.NaN).colors));
        build();
        assertEquals(0xFFA7C2FF, Theme.ACCENT);
        menu.open("Back");
        idle(50);
        shot("themed");
        menu.close();
        menu.flash(activity.getString(R.string.PANEL_SHOT_SAVED, "20261110-101500.bmp"), true);
        shot("themed-flash");
    }

    @Test
    public void screenshotClosesFirst() {
        menu.open("Back");
        menu.row(GameMenu.Item.SCREENSHOT).performClick();
        assertFalse(menu.isOpen());
        idle(50);
        assertEquals(1, host.screenshots);
        assertFalse(host.held());
        menu.flash(activity.getString(R.string.PANEL_SHOT_SAVED, "20261110-101500.bmp"), true);
        assertNotNull(menu.flashText());
        idle(GameMenu.FLASH_MS + 100);
        assertNull(menu.flashText());
    }

    @Test
    public void shareLogKeepsTheMenu() {
        menu.open("Back");
        assertNull("Share log is on the Settings page", menu.row(GameMenu.Item.SHARE_LOG));
        menu.row(GameMenu.Item.SETTINGS).performClick();
        menu.row(GameMenu.Item.SHARE_LOG).performClick();
        assertEquals(1, host.shareLogs);
        assertTrue(menu.isOpen());
        assertTrue(host.held());
    }

    // ---- Pages and quick settings ----------------------------------------------------------------------------------

    @Test
    public void quickSettingsByPad() {
        menu.open("Back");
        press(KeyEvent.KEYCODE_DPAD_DOWN);
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(GameMenu.Page.SETTINGS, menu.page());
        assertSame(menu.row(GameMenu.Item.PERF), menu.focusedRow());
        // Performance overlay: on, live, remembered.
        press(KeyEvent.KEYCODE_DPAD_RIGHT);
        assertTrue(menu.perfOn());
        assertTrue(GameStore.perfOverlay(activity));
        assertEquals("59.9 fps · 16.7 ms", menu.perfText());
        // Volume: left steps down, A mutes and unmutes.
        press(KeyEvent.KEYCODE_DPAD_DOWN);
        press(KeyEvent.KEYCODE_DPAD_LEFT);
        press(KeyEvent.KEYCODE_DPAD_LEFT);
        assertEquals(80, host.volume);
        assertEquals(80, GameStore.gameVolume(activity));
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(0, host.volume);
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(80, host.volume);
        // Bottom screen: Panel -> Black, live.
        press(KeyEvent.KEYCODE_DPAD_DOWN);
        press(KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(GameStore.BottomScreen.BLACK, GameStore.bottomScreen(activity));
        assertEquals(1, host.bottomChanges);
        // B: back to the main page, on Settings; B again closes.
        press(KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(GameMenu.Page.MAIN, menu.page());
        assertTrue(menu.isOpen());
        assertSame(menu.row(GameMenu.Item.SETTINGS), menu.focusedRow());
        press(KeyEvent.KEYCODE_BUTTON_B);
        assertFalse(menu.isOpen());
        // The overlay stays over the game.
        assertNotNull(menu.perfText());
    }

    @Test
    public void savedSettingsApplyAtStart() {
        GameStore.setGameVolume(activity, 30);
        GameStore.setPerfOverlay(activity, true);
        menu.release();
        build();
        assertEquals(30, host.volume);
        assertTrue(menu.perfOn());
        assertNotNull(menu.perfText());
    }

    @Test
    public void noBottomScreenRowOnOneScreen() {
        host.secondScreen = false;
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull(menu.row(GameMenu.Item.BOTTOM));
        assertNotNull(menu.row(GameMenu.Item.VOLUME));
    }

    @Test
    public void noLanguageRowWithoutAGame() {
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNull(menu.row(GameMenu.Item.LANGUAGE));
    }

    @Test
    public void languageRowIsPerTitle() throws Exception {
        java.io.File dir = new java.io.File(activity.getFilesDir(), "lang-game");
        TestSupport.write(new java.io.File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(activity, dir.getPath(), "Game");
        menu.release();
        build();
        menu.open("Back");
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.LANGUAGE));
        assertNull(GameLanguage.titleChoice(activity, "4D5300D1"));
        menu.row(GameMenu.Item.LANGUAGE).performClick();
        assertEquals("en", GameLanguage.titleChoice(activity, "4D5300D1"));
        menu.row(GameMenu.Item.LANGUAGE).performClick();
        assertEquals("fr", GameLanguage.titleChoice(activity, "4D5300D1"));
        for (int i = 0; i < GameLanguage.CODES.length - 1; i++)
            menu.row(GameMenu.Item.LANGUAGE).performClick();
        assertNull(GameLanguage.titleChoice(activity, "4D5300D1"));
    }

    @Test
    public void controlsPage() {
        menu.open("Back");
        menu.row(GameMenu.Item.CONTROLS).performClick();
        assertEquals(GameMenu.Page.CONTROLS, menu.page());
        assertSame(menu.row(GameMenu.Item.BACK), menu.focusedRow());
        press(KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(GameMenu.Page.MAIN, menu.page());
        assertSame(menu.row(GameMenu.Item.CONTROLS), menu.focusedRow());
    }

    @Test
    public void slidesInAndOut() {
        menu.setAnimate(true);
        menu.open("Back");
        assertEquals(View.VISIBLE, menu.drawer.getVisibility());
        assertTrue(menu.drawer.getTranslationX() < 0);
        idle(GameMenu.ANIM_MS + 50);
        assertEquals(0f, menu.drawer.getTranslationX(), 0.5f);
        assertEquals(1f, menu.scrim.getAlpha(), 0.01f);
        menu.close();
        assertEquals(View.VISIBLE, menu.drawer.getVisibility());
        idle(GameMenu.ANIM_MS + 50);
        assertEquals(View.GONE, menu.drawer.getVisibility());
        assertEquals(View.GONE, menu.scrim.getVisibility());
    }

    @Test
    public void drawerIsAFlushSheet() {
        menu.open("Back");
        idle(100);
        float scale = menu.content.getScaleX();
        float visible = (menu.drawer.getRight() + menu.drawer.getTranslationX()) * scale;
        float frac = visible / menu.root.getWidth();
        assertTrue("drawer " + frac, frac > 0.27f && frac < 0.31f);   // 560 of 1920
        assertEquals(0, menu.drawer.getLeft());
    }

    @Test
    public void durationFormat() {
        assertEquals("0:00", GameMenu.hoursMinutes(5_400));
        assertEquals("0:42", GameMenu.hoursMinutes(42 * 60_000L + 30_000));
        assertEquals("1:02", GameMenu.hoursMinutes(3_723_000));
    }

    /** Calm: six text-only rows (Quit apart), B says Resume; Settings' page says Back and holds Share log. */
    @Test
    public void mainPageRows() {
        menu.open("Back");
        GameMenu.Item[] main = { GameMenu.Item.RESUME, GameMenu.Item.SETTINGS, GameMenu.Item.CONTROLS,
                                 GameMenu.Item.SCREENSHOT, GameMenu.Item.LAUNCHER, GameMenu.Item.QUIT };
        for (GameMenu.Item it : main) {
            assertNotNull(it.toString(), menu.row(it));
            assertNull(it + ": no subtitle", menu.row(it).subtitle);
        }
        assertNull(menu.row(GameMenu.Item.SHARE_LOG));
        assertEquals(activity.getString(R.string.MENU_RESUME), menu.backHintForTest());
        assertEquals(activity.getString(R.string.MENU_SETTINGS), menu.row(GameMenu.Item.SETTINGS).title);
        menu.row(GameMenu.Item.SETTINGS).performClick();
        assertNotNull(menu.row(GameMenu.Item.SHARE_LOG));
        assertEquals(activity.getString(R.string.MENU_HINT_BACK), menu.backHintForTest());
        // Quit's confirmation is the one row that says more where it is.
        press(KeyEvent.KEYCODE_BUTTON_B);
        menu.row(GameMenu.Item.QUIT).performClick();
        assertEquals(activity.getString(R.string.MENU_QUIT_CONFIRM_SUB), menu.row(GameMenu.Item.QUIT).subtitle);
    }

    // ---- Screenshots -----------------------------------------------------------------------------------------------

    @Test
    public void screenshots() throws Exception {
        GameStore.remember(activity, "/sdcard/games/test", "Test Title: The Long Name Edition");
        String[][] sizes = { { "1080", "w1920dp-h1080dp-land-mdpi" }, { "720", "w1280dp-h720dp-land-mdpi" } };
        for (String loc : new String[] { "en", "fr" }) {
            for (String[] size : sizes) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                menu.release();
                build();
                String sfx = loc + "-" + size[0];
                shot("closed-" + sfx);
                menu.open("Back");
                shot("main-" + sfx);
                menu.row(GameMenu.Item.QUIT).requestFocusFromTouch();
                menu.row(GameMenu.Item.QUIT).performClick();
                shot("quit-armed-" + sfx);
                menu.row(GameMenu.Item.SETTINGS).requestFocusFromTouch();
                press(KeyEvent.KEYCODE_BUTTON_A);
                press(KeyEvent.KEYCODE_DPAD_DOWN);
                press(KeyEvent.KEYCODE_DPAD_LEFT);
                press(KeyEvent.KEYCODE_DPAD_UP);
                press(KeyEvent.KEYCODE_DPAD_RIGHT);    // overlay on
                shot("settings-" + sfx);
                press(KeyEvent.KEYCODE_BUTTON_B);
                menu.row(GameMenu.Item.CONTROLS).performClick();
                shot("controls-" + sfx);
                press(KeyEvent.KEYCODE_BUTTON_B);
                press(KeyEvent.KEYCODE_BUTTON_B);
                menu.flash(activity.getString(R.string.PANEL_SHOT_SAVED, "20261110-101500.bmp"), true);
                shot("closed-overlay-" + sfx);
                GameStore.setPerfOverlay(activity, false);
                GameStore.setGameVolume(activity, 100);
            }
        }
    }
}
