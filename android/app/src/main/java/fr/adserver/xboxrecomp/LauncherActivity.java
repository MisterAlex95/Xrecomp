package fr.adserver.xboxrecomp;

import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.ShapeDrawable;
import android.graphics.drawable.shapes.OvalShape;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.TextUtils;
import android.text.format.DateUtils;
import android.text.format.Formatter;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.EnumMap;
import java.util.List;
import java.util.Locale;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.LongSupplier;

import fr.adserver.xboxrecomp.Widgets.Icon;
import fr.adserver.xboxrecomp.Widgets.MenuRow;

/**
 * The launcher (XBA-41, with the game picker of XBA-54 / XBA-40): the app's launcher entry, in its own process
 * (":launcher"), before SDL. A controller-first screen set, built in code on a 1920 x 1080 design grid scaled to
 * the screen (Theme, Widgets), with a small state machine of screens and a back history instead of fragments:
 *
 * - WELCOME (first run, no game set up) -> SOURCE (game folder or disc image) -> FOLDER_ACCESS (All files access
 *   explained, or copy) -> IMPORT (progress ring, Cancel) -> HOME;
 * - HOME: a console-style home: a hero (the game's title image or a monogram, its title, a status chip, a few facts
 *   and one big Play / Prepare) over a row of smaller tiles: Settings, Controls, Game data, About;
 * - SETTINGS: « Open the game directly next time », UI sounds and vibration, and a few RECOMP_* options saved to
 *   recomp.cfg (RecompConfig), applied at the next start;
 * - CONTROLS: the device's buttons and the Xbox controller buttons the game gets;
 * - GAME_DATA: where the game is, its size, choose another, delete the imported copy; ABOUT;
 * - PREPARE: the on-device Prepare's progress (dimmed after two idle minutes, for OLED screens).
 *
 * D-pad / stick move the focus (remembered per screen; one highlight glides from row to row), A selects, B goes back,
 * left / right change a setting, L1 / R1 jump between settings sections, Start plays; touch works too. Screens fade
 * and slide in; moves, selects, backs, errors and a finished Prepare have a short sound and a haptic (UiFeedback).
 * Messages go to a note line, not toasts. The choice is remembered in SharedPreferences (GameStore); ShellActivity
 * hands it to native code as RECOMP_GAME_DIR. See docs/android/app-shell.md, « The launcher ».
 */
public class LauncherActivity extends Activity {
    static final String TAG = "xboxrecomp";
    /** Open the source screen even when a game is set up (the « Change game » shortcut). */
    static final String ACTION_CHOOSE_GAME = "fr.adserver.xboxrecomp.CHOOSE_GAME";
    /** Open the home screen even when « Open the game directly » is on (the « Launcher » shortcut). */
    static final String ACTION_SHOW_LAUNCHER = "fr.adserver.xboxrecomp.SHOW_LAUNCHER";
    /** Set by ShellActivity when the native side found no default.xbe: the folder it looked in. */
    static final String EXTRA_NO_GAME_DATA = "fr.adserver.xboxrecomp.NO_GAME_DATA";
    /**
     * Set by ShellActivity when the native loader refused the title pack (PACK_INVALID / PACK_DLOPEN): why. The pack
     * is dropped and Home offers Prepare again.
     */
    static final String EXTRA_BAD_PACK = "fr.adserver.xboxrecomp.BAD_PACK";
    /**
     * Sent by ShellActivity when the title quick-rebooted into itself (HalReturnToFirmware after XLaunchNewImage of
     * its own default.xbe, e.g. Fable's Pause, Quit): end the game's process and start the game activity again. The
     * launch data waits in the save dir (kernel_reboot.c); the play session goes on. Explicit intents only.
     */
    static final String ACTION_RESTART_GAME = "fr.adserver.xboxrecomp.RESTART_GAME";

    private static final int REQ_TREE = 1;
    private static final int REQ_ISO = 2;
    private static final int REQ_ALL_FILES = 3;
    private static final long SPACE_MARGIN = 64L << 20;
    private static final long CONFIRM_MS = 5000;

    enum Screen {
        WELCOME, SOURCE, FOLDER_ACCESS, IMPORT, HOME, SETTINGS, CONTROLS, GAME_DATA, ABOUT, PREPARE, LIBRARY, STORAGE
    }

    /** The Preparing screen dims after this long without input (OLED burn-in); any key or touch wakes it. */
    static final long DIM_AFTER_MS = 120_000;
    /** The launcher's clock for the idle dim (uptime ms); the JVM tests put a fake one here. */
    static LongSupplier clock = SystemClock::uptimeMillis;

    /**
     * The on-device Prepare for the current game (docs/android/prepare.md): OFF when this APK has no toolchain or
     * the game is not READY (the launcher then behaves as before), PLAYABLE when its pack is valid.
     */
    enum Prep { OFF, NEEDED, RUNNING, QUEUED, FAILED, CANCELLED, INTERRUPTED, PLAYABLE }

    /** Free internal storage a Prepare needs (work files, objects, the pack). */
    static final long PREPARE_MIN_FREE = 2_000_000_000L;
    /** Below this much RAM the Prepare screen warns: about what Android reports for a « 6 GB » device. */
    static final long PREPARE_RAM_WARN = (5L << 30) + (512L << 20);
    /** A « running » status.json this fresh counts as alive even before the ":prepare" process shows up. */
    private static final long PREPARE_FRESH_MS = 15000;
    private static final long PREPARE_POLL_MS = 500;
    private static final int REQ_NOTIFICATIONS = 4;
    private static final int REQ_LSFG_DLL = 5;
    private static final int REQ_LIBRARY_TREE = 6;
    /** The library re-reads its entries' states (Prepare, queue, running game) every this many polls. */
    private static final int LIBRARY_POLL_EVERY = 4;

    /** The selected game's files: READY, ... ; LIBRARY: a library game whose files are not here (see its chip). */
    enum State { READY, NONE, MISSING, NO_ACCESS, WRONG, LIBRARY }

    private static final int NOTE_INFO = 0, NOTE_OK = 1, NOTE_WARN = 2, NOTE_ERROR = 3;

    // ---- The stage ----
    private TextView screenTitle, note;
    private Widgets.Chip chip;
    private FrameLayout body, host;
    private Widgets.Backdrop backdrop;
    private Widgets.Glide glide;
    private AnimatorSet screenAnim;
    private UiFeedback feedback;
    /** The panel on a second display (null until the stage is built, and when the game started at once). */
    private LauncherCompanion companion;
    /** True while a screen is being built: its first focus is not a « move ». */
    private boolean building;

    // ---- The game's identity (TitleArt) ----
    private String artTid;
    private Bitmap art;
    private int artColor = Theme.ACCENT;

    // ---- The game theme (GameTheme, docs/android/app-shell.md « Game theme ») ----
    private File heroFile;
    private Bitmap hero;
    private String themeLogged;
    /** Title IDs whose theme a worker bakes (or tried to), once per activity. */
    private final java.util.Set<String> themeBaking = new java.util.HashSet<>();
    /** Where a missing theme is baked: a thread of its own (tests run it in place: TestSupport.StockTheme). */
    static final java.util.concurrent.Executor THEME_THREAD = r -> new Thread(r, "xba-theme").start();
    static java.util.concurrent.Executor themeExecutor = THEME_THREAD;

    // ---- Home ----
    private MenuRow homePrimary;
    private Widgets.Meter homeMeter;
    private TextView homeProgress;

    // ---- The Preparing screen's idle dim ----
    private FrameLayout dimView;
    private TextView dimBig, dimSmall;
    private boolean dimmed;
    private long lastInputAt;
    private int swallowUp = -1;
    private LinearLayout bottomLeft, hints;
    private ShapeDrawable noteDot;
    private String noteText = "";
    private int noteKind;

    // ---- Screens, history, focus ----
    private Screen screen;
    private final ArrayDeque<Screen> history = new ArrayDeque<>();
    private final EnumMap<Screen, Integer> focusMemory = new EnumMap<>(Screen.class);
    private final List<View> focusables = new ArrayList<>();
    private View defaultFocus;
    private Runnable xAction, yAction, startAction, l1Action, r1Action;

    // ---- Detail panel of the current screen ----
    private LinearLayout detail;

    // ---- Game facts computed off the UI thread ----
    private String hashDir;
    private Boolean hashOk;
    private String sizeDir;
    private long sizeBytes = -1;
    private boolean deleting;
    private long deleteArmedAt;

    // ---- Settings ----
    private RecompConfig cfg;

    // ---- Import ----
    private Thread worker;
    private volatile boolean cancelRequested;
    private long lastUiUpdate;
    private int importLabel;
    private Screen importReturn = Screen.SOURCE;
    private Widgets.Ring ring;
    private TextView importHead, importBytes, importFile;
    private MenuRow cancelRow;
    private volatile String workerNote, chosenDir, chosenTitle;
    private volatile int workerNoteKind;

    // ---- Prepare ----
    private String titleIdDir, titleIdValue;
    private final android.os.Handler poller = new android.os.Handler(android.os.Looper.getMainLooper());
    private boolean resumed;
    private Prep homePrep;
    private PrepareProtocol.Status prepShown;
    private TextView prepHead, prepMsg, prepEta, prepCaption;
    private final List<TextView[]> prepSteps = new ArrayList<>();
    private MenuRow prepCancelRow;
    private long cancelArmedAt;
    private boolean cancelSent;
    private final Runnable pollTick = this::pollPrepare;

    // ---- The game library (docs/android/app-shell.md « Game library ») ----
    /** games.json: the records; selected is the game Home shows (the launched one is in the preferences). */
    private Library.Games games = new Library.Games();
    /** index.json: what the library folder holds, as last scanned. */
    private LibraryIndex index = new LibraryIndex();
    private java.util.Map<String, List<LibraryIndex.Item>> indexGames = new java.util.HashMap<>();
    /** The library screen's entries and cards, as built. */
    private List<LibraryScreen.Entry> libEntries = new ArrayList<>();
    private final List<GameCard> libCards = new ArrayList<>();
    private final java.util.Map<String, Bitmap> libArt = new java.util.HashMap<>();
    /** The focused card's title ID on the library ("" for the folder tile). */
    private String focusTid;
    private TextView libCaption;
    private int libPolls;
    private Thread scanner;
    private volatile boolean scanCancel;
    private volatile int scanFound;
    /** The tree picker was opened for the library folder (not for a game folder). */
    private boolean pickLibrary;
    /** Title IDs whose pack the shell refused in this process (GameState's « Pack invalid »). */
    private final java.util.Set<String> droppedPacks = new java.util.HashSet<>();
    /** The title being imported from the library (its chip), and whether a Prepare follows the import. */
    private volatile String importTid;
    private boolean importFromLibrary;
    private volatile String chosenSha;
    /** « Play » while another game runs: the first press warns, the second within CONFIRM_MS ends it. */
    private long switchArmedAt;
    /** The library chip Home was built with (a game whose files are not here), for the poll. */
    private GameState.Chip homeChip;
    /** Storage: sizes measured (« tid/KIND » -> bytes), the item armed for deletion. */
    private final java.util.Map<String, Long> storageSizes = new java.util.HashMap<>();
    private Thread storageMeasurer;
    private String storageArmedKey;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        publishShortcuts();

        Intent in = getIntent();
        String action = in.getAction();
        if (ACTION_RESTART_GAME.equals(action) && savedInstanceState == null) {
            restartGame();
            return;
        }
        boolean change = ACTION_CHOOSE_GAME.equals(action);
        boolean showLauncher = ACTION_SHOW_LAUNCHER.equals(action);
        String noData = in.getStringExtra(EXTRA_NO_GAME_DATA);
        String badPack = in.getStringExtra(EXTRA_BAD_PACK);
        boolean forced = change || showLauncher || noData != null || badPack != null;
        // Back from the game: its play session ends here (Home's « Play time »), then the library is read: the
        // one-game state becomes its first record the first time (Library.migrate moves no file).
        long sessionStart = GameStore.sessionStartedAt(this);
        String sessionTid = GameStore.sessionTid(this);
        boolean hadLibrary = Library.gamesFile(getFilesDir()).isFile();
        long now = System.currentTimeMillis();
        if (savedInstanceState == null && !gameRunning())
            GameStore.sessionEnd(this, now);
        else
            sessionStart = 0;
        games = Library.migrate(this);
        setIndex(LibraryIndex.read(Library.indexFile(getFilesDir())));
        if (sessionStart > 0 && sessionTid != null && hadLibrary) {
            Library.sessionEnd(games, sessionTid, sessionStart, now);
            writeGames();
        }
        if (savedInstanceState == null) {
            // The launcher opens on the game last launched (the preferences say which; its record may lack).
            String launched = GameStore.launchedTitleId(this);
            if (launched != null && games.get(launched) != null)
                games.selected = launched;
        }
        if (badPack != null && savedInstanceState == null)
            dropBadPack(badPack);
        if (!forced && savedInstanceState == null && gameReady() && !prepareBlocksPlay()) {
            /* The app icon while the game runs in the background: straight back to it (the « Launcher » shortcut
             * reaches this screen then). Otherwise only with « Open the game directly next time » on. */
            boolean running = gameRunning();
            if (running) {
                Log.i(TAG, "launcher: game running, bringing it back");
                resumeShell();
                return;
            }
            if (GameStore.autoStart(this) && selectedIsLaunched()) {
                Log.i(TAG, "launcher: game ready and « open directly » on, starting it");
                launchShell(false);
                return;
            }
        }

        boolean lib = libraryOn();
        String dir = GameStore.chosenDir(this);
        Log.i(TAG, "launcher: shown (" + (noData != null ? "NO_GAME_DATA in " + noData
                                          : badPack != null ? "pack refused: " + badPack
                                          : change ? "change game"
                                          : showLauncher ? "launcher shortcut"
                                          : dir != null && !gameReady() ? "remembered folder unusable: " + dir
                                          : lib ? "library, " + games.games.size() + " games"
                                          : gameReady() ? "home" : "no game set up") + ")");
        Screen first;
        boolean toPrepare = false;
        if (noData != null) {
            history.push(lib ? Screen.LIBRARY : Screen.HOME);
            first = lib ? Screen.HOME : Screen.SOURCE;
        } else if (change) {
            if (!lib)
                history.push(Screen.HOME);
            first = lib ? Screen.LIBRARY : Screen.SOURCE;
        } else if (badPack != null) {
            if (lib)
                history.push(Screen.LIBRARY);
            first = Screen.HOME;
        } else if (lib) {
            first = Screen.LIBRARY;
            Prep p = prep();
            if (p == Prep.RUNNING || p == Prep.FAILED) {   // the pending Prepare, its Home beneath
                history.push(Screen.LIBRARY);
                history.push(Screen.HOME);
                first = Screen.PREPARE;
            }
        } else if (state() == State.NONE) {
            first = Screen.WELCOME;
        } else {
            first = Screen.HOME;
            Prep p = prep();
            toPrepare = p == Prep.RUNNING || p == Prep.FAILED;
        }

        setUpWindow();
        feedback = new UiFeedback(this);
        syncTheme(first);       // the game's colours before any view is built: no flash of the stock green
        buildStage();
        // The second screen (AYN Thor, docs/android/app-shell.md « Dual screen »): Home's or the Prepare's panel.
        companion = new LauncherCompanion(this, new LauncherPanel.Actions() {
            @Override
            public void play() {
                onPlay();
            }

            @Override
            public void settings() {
                go(Screen.SETTINGS);
            }

            @Override
            public void changeGame() {
                onChangeGame();
            }
        });
        companion.libraryActions(new LibraryPanel.Actions() {
            @Override
            public void primary() {
                LibraryScreen.Entry e = focusedEntry();
                if (e != null)
                    libraryPrimary(e);
            }

            @Override
            public void rescan() {
                rescanLibrary();
            }

            @Override
            public void settings() {
                go(Screen.SETTINGS);
            }
        });

        show(first);
        if (noData != null)
            setNote(getString(R.string.NO_GAME_DATA, noData), NOTE_ERROR);
        else if (change && !lib && gameRunning())
            setNote(getString(R.string.GAME_RUNNING), NOTE_INFO);
        else if (badPack != null)
            setNote(getString(R.string.PACK_BAD, badPack), NOTE_ERROR);
        if (toPrepare)
            go(Screen.PREPARE);
        // The library folder through the Storage Access Framework: scanned at a cold start (and on Y) only.
        if (savedInstanceState == null && lib && Library.folderTree(this) != null && Library.folderPath(this) == null)
            startScan();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        poller.removeCallbacks(pollTick);
        poller.postDelayed(pollTick, PREPARE_POLL_MS);
    }

    @Override
    protected void onPause() {
        resumed = false;
        poller.removeCallbacks(pollTick);
        super.onPause();
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (companion != null)
            companion.start();
    }

    @Override
    protected void onStop() {
        if (companion != null)
            companion.stop();
        super.onStop();
    }

    LauncherCompanion companionForTest() {
        return companion;
    }

    @Override
    protected void onDestroy() {
        if (feedback != null)
            feedback.release();
        super.onDestroy();
    }

    /** For the JVM unit tests: the screen shown, and the Prepare state Home was built with (null: Play). */
    Screen screenForTest() {
        return screen;
    }

    /** The tests run in touch mode, where nothing is focused: this brings the screen's focus as a D-pad press does. */
    void forceFocusForTest() {
        restoreFocus(true);
    }

    View focusForTest() {
        return focused();
    }

    /** The focused view of the stage (what the Glide highlights), or null. */
    private View focused() {
        return host == null ? getCurrentFocus() : host.findFocus();
    }

    void goForTest(Screen s) {
        go(s);
    }

    UiFeedback feedbackForTest() {
        return feedback;
    }

    AnimatorSet screenAnimatorForTest() {
        return screenAnim;
    }

    Widgets.Glide glideForTest() {
        return glide;
    }

    Widgets.Backdrop backdropForTest() {
        return backdrop;
    }

    MenuRow homePrimaryForTest() {
        return homePrimary;
    }

    boolean dimmedForTest() {
        return dimmed;
    }

    boolean hasArtForTest() {
        return art != null;
    }

    Prep homePrepForTest() {
        return homePrep;
    }

    String noteForTest() {
        return noteText;
    }

    /** The library's cards as built (the « Library folder » tile last). */
    List<GameCard> libraryCardsForTest() {
        return libCards;
    }

    // ---- Window and stage ----------------------------------------------------------------------------------------

    /** Fullscreen, immersive, drawn under the display cutout (the stage keeps its content out of it). */
    private void setUpWindow() {
        Window w = getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Theme.BG_BOTTOM));
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        w.setAttributes(lp);
        hideSystemBars();
    }

    private void hideSystemBars() {
        /* PhoneWindow.getInsetsController() dereferences the decor view without creating it: called before
         * setContentView it throws a NullPointerException (the crash at start of the first launcher build).
         * getDecorView() creates the decor first. */
        getWindow().getDecorView();
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
    }

    private static LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    private static LinearLayout.LayoutParams lp(int w, int h, float weight) {
        return new LinearLayout.LayoutParams(w, h, weight);
    }

    private static final int MATCH = ViewGroup.LayoutParams.MATCH_PARENT, WRAP = ViewGroup.LayoutParams.WRAP_CONTENT;

    private LinearLayout column() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private LinearLayout rowLayout() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        return l;
    }

    private static View gap(android.content.Context c) {
        return new View(c);
    }

    /**
     * The parts every screen shares: top bar (mark, title, status chip), body, note line, bottom bar (hints). Built
     * again when the game theme changes (syncTheme): it holds no state of its own but the views.
     */
    private void buildStage() {
        if (screenAnim != null)
            screenAnim.cancel();
        host = new FrameLayout(this);
        backdrop = new Widgets.Backdrop();
        backdrop.setHero(hero);
        host.setBackground(backdrop);
        LinearLayout root = column();
        host.addView(root, new FrameLayout.LayoutParams(MATCH, MATCH));
        glide = new Widgets.Glide(this);
        host.addView(glide, new FrameLayout.LayoutParams(MATCH, MATCH));
        buildDim();
        host.getViewTreeObserver().addOnGlobalFocusChangeListener((was, now) -> {
            if (!building && was != null && (now instanceof MenuRow || now instanceof GameCard))
                feedback.play(UiFeedback.Cue.MOVE, now);
        });
        root.setPadding(Theme.MARGIN_X, Theme.MARGIN_TOP, Theme.MARGIN_X, Theme.MARGIN_BOTTOM);
        root.setClipChildren(false);

        LinearLayout top = rowLayout();
        top.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.BrandMark mark = new Widgets.BrandMark(this);
        LinearLayout.LayoutParams mlp = lp(64, 64);
        mlp.rightMargin = 26;
        top.addView(mark, mlp);
        screenTitle = Theme.oneLine(Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT));
        top.addView(screenTitle, lp(0, WRAP, 1));
        chip = new Widgets.Chip(this);
        top.addView(chip, lp(WRAP, WRAP));
        root.addView(top, lp(MATCH, Theme.TOP_BAR_H));

        LinearLayout.LayoutParams rlp = lp(MATCH, 2);
        rlp.bottomMargin = Theme.GAP;
        root.addView(new Widgets.Rule(this), rlp);

        body = new FrameLayout(this);
        body.setClipChildren(false);
        root.addView(body, lp(MATCH, 0, 1));

        note = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM));
        note.setGravity(Gravity.CENTER_VERTICAL);
        noteDot = new ShapeDrawable(new OvalShape());
        noteDot.setBounds(0, 0, 14, 14);
        note.setCompoundDrawables(noteDot, null, null, null);
        note.setCompoundDrawablePadding(20);
        note.setPadding(8, 0, 0, 0);
        root.addView(note, lp(MATCH, Theme.NOTE_H));

        LinearLayout bottom = rowLayout();
        bottom.setGravity(Gravity.CENTER_VERTICAL);
        bottomLeft = rowLayout();
        bottomLeft.setGravity(Gravity.CENTER_VERTICAL);
        bottom.addView(bottomLeft, lp(WRAP, MATCH));
        bottom.addView(gap(this), lp(0, 1, 1));
        hints = rowLayout();
        hints.setGravity(Gravity.CENTER_VERTICAL);
        bottom.addView(hints, lp(WRAP, MATCH));
        root.addView(bottom, lp(MATCH, Theme.BOTTOM_BAR_H));

        setContentView(new Widgets.Stage(this, host));
        setNote(null, NOTE_INFO);
    }

    private void setNote(String text, int kind) {
        noteText = text == null ? "" : text;
        noteKind = kind;
        paintNote();
        if (!noteText.isEmpty() && kind >= NOTE_WARN)
            Log.i(TAG, "launcher: " + noteText);
        if (!noteText.isEmpty() && kind == NOTE_ERROR && feedback != null)
            feedback.play(UiFeedback.Cue.ERROR, host);
    }

    private void paintNote() {
        if (note == null)
            return;
        int color = noteKind == NOTE_ERROR ? Theme.ERROR : noteKind == NOTE_WARN ? Theme.WARN
                    : noteKind == NOTE_OK ? Theme.OK : Theme.TEXT_DIM;
        note.setText(noteText);
        note.setTextColor(color);
        noteDot.getPaint().setColor(noteText.isEmpty() ? 0 : color);
        noteDot.invalidateSelf();
    }

    private void addHint(String glyph, int label, Runnable action) {
        Widgets.Hint h = new Widgets.Hint(this, glyph, getString(label), v -> action.run());
        LinearLayout.LayoutParams p = lp(WRAP, WRAP);
        p.leftMargin = 44;
        hints.addView(h, p);
    }

    /** A focusable the screen registers, in order: focus memory is an index in this list. */
    private <T extends View> T focusable(T v) {
        focusables.add(v);
        return v;
    }

    private MenuRow row(Icon icon, String title, String sub, Runnable click) {
        MenuRow r = new MenuRow(this);
        r.set(icon, title, sub);
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        if (click != null)
            r.setOnClickListener(v -> click.run());
        return focusable(r);
    }

    private FrameLayout panel() {
        FrameLayout f = new FrameLayout(this);
        f.setBackground(new Widgets.Panel());
        f.setPadding(56, 48, 56, 44);
        return f;
    }

    // ---- Navigation --------------------------------------------------------------------------------------------

    private void go(Screen s) {
        if (screen != null && screen != Screen.IMPORT)
            history.push(screen);
        show(s);
    }

    private void back() {
        feedback.play(UiFeedback.Cue.BACK, host);
        if (screen == Screen.IMPORT) {
            requestCancel();
            return;
        }
        if (screen == Screen.PREPARE && prepShown != null && prepShown.running()) {
            armCancelPrepare();
            return;
        }
        if (history.isEmpty()) {
            if (screen != Screen.LIBRARY && screen != Screen.WELCOME && libraryOn()) {
                show(Screen.LIBRARY, -1);   // Home of a library game: back to the library
                return;
            }
            finish();
            return;
        }
        show(history.pop(), -1);
    }

    private void show(Screen s) {
        show(s, 1);
    }

    /**
     * Builds screen s from scratch (state may have changed), clears the note, restores the screen's focus, and fades
     * it in sliding from the right (dir 1, forward), the left (-1, back), or not at all (0, a refresh).
     */
    private void show(Screen s, int dir) {
        if (screen != null) {
            int i = focusables.indexOf(host.findFocus());
            if (i >= 0)
                focusMemory.put(screen, i);
        }
        syncTheme(s);
        if (s != Screen.LIBRARY && backdrop != null)
            backdrop.setGlows(Theme.GLOW, Theme.GLOW2);   // the library's per-card glow stays there
        building = true;
        setDimmed(false);
        Screen was = screen;
        if (s != screen)
            lastInputAt = clock.getAsLong();
        screen = s;
        if (s != Screen.IMPORT && s != Screen.PREPARE)
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        body.removeAllViews();
        bottomLeft.removeAllViews();
        hints.removeAllViews();
        focusables.clear();
        defaultFocus = null;
        detail = null;
        xAction = yAction = startAction = l1Action = r1Action = null;
        chip.set("", 0);
        deleteArmedAt = 0;
        storageArmedKey = null;
        cancelArmedAt = 0;
        homePrep = null;
        prepShown = null;
        prepSteps.clear();
        prepHead = prepMsg = prepEta = prepCaption = null;
        prepCancelRow = null;
        homePrimary = null;
        homeMeter = null;
        homeProgress = null;
        homeChip = null;
        switchArmedAt = 0;
        libCards.clear();
        libCaption = null;
        setNote(null, NOTE_INFO);
        refreshIdentity();
        switch (s) {
        case WELCOME:
            buildWelcome();
            break;
        case SOURCE:
            buildSource();
            break;
        case FOLDER_ACCESS:
            buildFolderAccess();
            break;
        case IMPORT:
            buildImport();
            break;
        case HOME:
            buildHome();
            break;
        case SETTINGS:
            buildSettings();
            break;
        case CONTROLS:
            buildControls();
            break;
        case GAME_DATA:
            buildGameData();
            break;
        case ABOUT:
            buildAbout();
            break;
        case PREPARE:
            buildPrepare();
            break;
        case LIBRARY:
            buildLibrary();
            break;
        case STORAGE:
            buildStorage();
            break;
        }
        restoreFocus(false);
        // The library folder read as files: a (stat-only) rescan each time the library opens.
        if (s == Screen.LIBRARY && was != Screen.LIBRARY && Library.folderPath(this) != null)
            startScan();
        building = false;
        if (dir != 0)
            animateIn(dir);
    }

    /** The screen change: the body fades in and slides in by Theme.ANIM_SLIDE, the title fades in. */
    private void animateIn(int dir) {
        if (screenAnim != null)
            screenAnim.cancel();
        AnimatorSet set = new AnimatorSet();
        set.playTogether(ObjectAnimator.ofFloat(body, View.ALPHA, 0, 1),
                         ObjectAnimator.ofFloat(body, View.TRANSLATION_X, dir * Theme.ANIM_SLIDE, 0),
                         ObjectAnimator.ofFloat(screenTitle, View.ALPHA, 0, 1));
        set.setDuration(Theme.ANIM_SCREEN_MS);
        set.setInterpolator(new android.view.animation.DecelerateInterpolator());
        set.start();
        screenAnim = set;
    }

    /**
     * The game theme for the current game: from the cache (GameTheme.load), or baked by a worker when missing, after
     * which the screen is shown again. No game, no title ID or the player's « Off »: the stock colours. When the
     * colours or the hero change, the tokens are applied and the stage is built again (its paints hold the colours
     * they were made with), and the bottom screen's panel too; show() then builds the body. True when it changed.
     */
    private boolean syncTheme(Screen s) {
        // The library and Storage: always the stock colours (only the backdrop's glow follows the focused card).
        boolean stock = s == Screen.LIBRARY || s == Screen.STORAGE;
        String dir = stock ? null : gameDir();
        File gd = dir == null ? null : new File(dir);
        String tid = null;
        if (!stock && (gd != null && GameStore.findXbe(gd) != null || dir == null && state() == State.LIBRARY))
            tid = titleId();
        GameTheme g = null;
        GameTheme.Mode mode = tid == null ? null : GameTheme.mode(this, tid);
        if (tid != null && mode != GameTheme.Mode.OFF) {
            g = GameTheme.load(this, gd, tid);
            if (g == null)
                bakeTheme(gd != null ? gd : cachedXbeDir(), tid);
        }
        Theme.Colors want = g == null ? Theme.Colors.STOCK : g.colors;
        File wantHero = g == null ? null : g.heroFile;
        String line = g != null ? g.logLine()
                                : "launcher: theme " + (tid == null ? "none" : tid) + ": stock"
                                  + (mode == null ? "" : ", mode " + mode.key);
        if (!line.equals(themeLogged)) {
            themeLogged = line;
            Log.i(TAG, line);
        }
        boolean changed = !want.equals(Theme.current()) || !java.util.Objects.equals(wantHero, heroFile);
        if (!changed)
            return false;
        Theme.apply(want);
        heroFile = wantHero;
        hero = wantHero == null ? null : BitmapFactory.decodeFile(wantHero.getPath());
        if (host != null) {
            buildStage();
            if (companion != null)
                companion.screen().rebuild();
        }
        return true;
    }

    /** Bakes the title's theme off the UI thread (an older import, a cache made stale), then shows the screen again. */
    private void bakeTheme(File gameDir, String tid) {
        if (gameDir == null || !themeBaking.add(tid))
            return;
        String title = gameTitle(gameDir());
        android.content.Context app = getApplicationContext();
        themeExecutor.execute(() -> {
            GameTheme.ensure(app, gameDir, tid, title);
            poller.post(() -> {
                if (isFinishing() || isDestroyed() || screen == null || screen == Screen.IMPORT
                    || screen == Screen.PREPARE || screen == Screen.LIBRARY || screen == Screen.STORAGE)
                    return;   // live progress, or stock colours: the theme shows on the next screen
                artTid = null;   // the title image may have come from another source than $$XTIMAGE
                String keep = noteText;
                int keepKind = noteKind;
                show(screen, 0);
                noteText = keep;   // the note on screen stays (shown again without its sound or log)
                noteKind = keepKind;
                paintNote();
            });
        });
    }

    /** The game's title image (or monogram colour) for the current game. */
    private void refreshIdentity() {
        String dir = gameDir();
        String tid = titleId();
        String key = tid != null ? tid : dir;
        if (key == null) {
            artTid = null;
            art = null;
            artColor = Theme.ACCENT;
        } else if (!key.equals(artTid)) {
            artTid = key;
            if (tid == null) {
                art = null;
            } else if (dir != null) {
                art = TitleArt.load(getFilesDir(), GameStore.findXbe(new File(dir)), tid);
            } else {   // a library game whose files are not here: the scan's cached image
                File f = TitleArt.cacheFile(getFilesDir(), tid);
                art = f.isFile() ? BitmapFactory.decodeFile(f.getPath()) : null;
            }
            Log.i(TAG, "launcher: title image " + (art != null ? art.getWidth() + "x" + art.getHeight() : "none")
                       + " for " + key);
        }
        // The art tile's glow: the game theme's (stock: the accent); the monogram keeps the title ID's colour.
        if (key != null)
            artColor = art != null ? Theme.GLOW : TitleArt.monogramColor(key);
    }

    private View focusTarget() {
        Integer i = focusMemory.get(screen);
        if (i != null && i < focusables.size())
            return focusables.get(i);
        if (defaultFocus != null)
            return defaultFocus;
        return focusables.isEmpty() ? null : focusables.get(0);
    }

    private void restoreFocus(boolean fromTouch) {
        View v = focusTarget();
        if (v == null)
            return;
        if (fromTouch)
            v.requestFocusFromTouch();
        else
            v.requestFocus();
    }

    /**
     * Gamepad buttons, before the views: A selects the focused item, B / Back goes back (both on release, once), X,
     * Y, Start, L1, R1 do what the screen's hints say. A navigation key with nothing focused (after a touch) first
     * brings the screen's focus back. Left / right on a setting are the row's own (MenuRow.onKeyDown).
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (screen == null)
            return super.dispatchKeyEvent(e);
        int k = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        boolean once = e.getAction() == KeyEvent.ACTION_UP && !e.isCanceled();
        lastInputAt = clock.getAsLong();
        // The dimmed Preparing screen: the first key only wakes it (its release is swallowed too).
        if (dimmed) {
            if (down) {
                setDimmed(false);
                swallowUp = k;
            }
            return true;
        }
        if (swallowUp == k && !down) {
            swallowUp = -1;
            return true;
        }
        switch (k) {
        case KeyEvent.KEYCODE_BACK:
        case KeyEvent.KEYCODE_BUTTON_B:
        case KeyEvent.KEYCODE_ESCAPE:
            if (once)
                back();
            return true;
        case KeyEvent.KEYCODE_BUTTON_A:
            if (down && e.getRepeatCount() == 0 && focused() == null) {
                restoreFocus(true);
                return true;
            }
            if (once && focused() != null)
                focused().performClick();
            return true;
        case KeyEvent.KEYCODE_BUTTON_X:
            return runOnce(once, xAction);
        case KeyEvent.KEYCODE_BUTTON_Y:
            return runOnce(once, yAction);
        case KeyEvent.KEYCODE_BUTTON_START:
            return runOnce(once, startAction);
        case KeyEvent.KEYCODE_BUTTON_L1:
            return runOnce(once, l1Action);
        case KeyEvent.KEYCODE_BUTTON_R1:
            return runOnce(once, r1Action);
        case KeyEvent.KEYCODE_DPAD_UP:
        case KeyEvent.KEYCODE_DPAD_DOWN:
        case KeyEvent.KEYCODE_DPAD_LEFT:
        case KeyEvent.KEYCODE_DPAD_RIGHT:
        case KeyEvent.KEYCODE_DPAD_CENTER:
        case KeyEvent.KEYCODE_ENTER:
            if (down && focused() == null && !focusables.isEmpty()) {
                restoreFocus(true);
                return true;
            }
            break;
        default:
            break;
        }
        return super.dispatchKeyEvent(e);
    }

    private static boolean runOnce(boolean once, Runnable r) {
        if (once && r != null)
            r.run();
        return true;
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        lastInputAt = clock.getAsLong();
        if (dimmed) {
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN)
                setDimmed(false);
            return true;
        }
        return super.dispatchTouchEvent(e);
    }

    // ---- The Preparing screen's idle dim (OLED burn-in) --------------------------------------------------------

    /** A near-black layer over the stage: the overall percentage and the step, small and grey, moved each minute. */
    private void buildDim() {
        dimView = new FrameLayout(this);
        dimView.setBackgroundColor(0xFF000000);
        dimView.setVisibility(View.GONE);
        LinearLayout c = column();
        c.setGravity(Gravity.CENTER_HORIZONTAL);
        dimBig = Theme.text(this, 120, Theme.LIGHT, Theme.alpha(Theme.TEXT_DIM, 0.55f), "");
        c.addView(dimBig, lp(WRAP, WRAP));
        dimSmall = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.alpha(Theme.TEXT_FAINT, 0.8f), "");
        LinearLayout.LayoutParams sl = lp(WRAP, WRAP);
        sl.topMargin = 18;
        c.addView(dimSmall, sl);
        dimView.addView(c, new FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER));
        host.addView(dimView, new FrameLayout.LayoutParams(MATCH, MATCH));
    }

    private void setDimmed(boolean on) {
        if (dimView == null || dimmed == on)
            return;
        dimmed = on;
        dimView.setVisibility(on ? View.VISIBLE : View.GONE);
        WindowManager.LayoutParams wlp = getWindow().getAttributes();
        wlp.screenBrightness = on ? 0.02f : WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(wlp);
        if (on)
            paintDim();
        else
            lastInputAt = clock.getAsLong();
        Log.i(TAG, "launcher: preparing screen " + (on ? "dimmed (idle)" : "awake"));
    }

    /** The dim layer's text, from the Preparing screen's last status; its place shifts by the minute. */
    private void paintDim() {
        PrepareProtocol.Status s = prepShown;
        float frac = s == null || s.step <= 0 ? -1 : PrepareProtocol.overall(s.step, s.n, s.pct);
        dimBig.setText(frac >= 0 ? String.format(Locale.getDefault(), "%d%%", Math.round(frac * 100)) : "…");
        String step = s == null ? getString(R.string.PREPARE_STARTING) : PrepareService.stepName(this, s.id);
        if (s != null && s.eta >= 0)
            step += " · " + getString(R.string.PREPARE_ETA, PrepareProtocol.formatEta(s.eta));
        dimSmall.setText(step);
        long minute = clock.getAsLong() / 60_000;
        View c = (View)dimBig.getParent();
        c.setTranslationX((float)(Math.sin(minute * 1.7) * 220));
        c.setTranslationY((float)(Math.cos(minute * 1.3) * 140));
    }

    /** Polled with the status: dims the running Preparing screen after DIM_AFTER_MS without input. */
    private void checkDim(boolean running) {
        if (!running) {
            setDimmed(false);
            return;
        }
        if (dimmed)
            paintDim();
        else if (clock.getAsLong() - lastInputAt >= DIM_AFTER_MS)
            setDimmed(true);
    }

    // ---- Detail panel ------------------------------------------------------------------------------------------

    /** A panel on the right of a screen, refilled for the focused item. */
    private FrameLayout detailPanel() {
        FrameLayout p = panel();
        detail = column();
        p.addView(detail, new FrameLayout.LayoutParams(MATCH, MATCH));
        return p;
    }

    private void detailClear(String caps, String title, String text) {
        if (detail == null)
            return;
        detail.removeAllViews();
        if (caps != null)
            detail.addView(Theme.caps(this, caps, Theme.ACCENT), lp(MATCH, WRAP));
        TextView t = Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, title);
        LinearLayout.LayoutParams tl = lp(MATCH, WRAP);
        tl.topMargin = caps != null ? 18 : 0;
        tl.bottomMargin = 18;
        detail.addView(t, tl);
        if (text != null && !text.isEmpty())
            detail.addView(Theme.small(this, text), lp(MATCH, WRAP));
    }

    /** A « label   value » line in the detail panel. */
    private TextView detailKv(String label, String value) {
        LinearLayout l = rowLayout();
        TextView k = Theme.oneLine(Theme.text(this, Theme.T_BODY - 2, Theme.REGULAR, Theme.TEXT_FAINT, label));
        l.addView(k, lp(250, WRAP));
        TextView v = Theme.text(this, Theme.T_BODY - 2, Theme.REGULAR, Theme.TEXT, value);
        v.setMaxLines(2);
        v.setEllipsize(TextUtils.TruncateAt.END);
        l.addView(v, lp(0, WRAP, 1));
        LinearLayout.LayoutParams p = lp(MATCH, WRAP);
        p.topMargin = 12;
        detail.addView(l, p);
        return v;
    }

    private void detailFooter(String text, int color) {
        detail.addView(gap(this), lp(MATCH, 0, 1));
        detail.addView(new Widgets.Rule(this), lp(MATCH, 2));
        TextView t = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, color, text);
        t.setLineSpacing(0, 1.2f);
        LinearLayout.LayoutParams p = lp(MATCH, WRAP);
        p.topMargin = 18;
        detail.addView(t, p);
    }

    private void onFocus(View v, Runnable fill) {
        v.setOnFocusChangeListener((view, has) -> {
            if (has)
                fill.run();
        });
    }

    // ---- WELCOME -----------------------------------------------------------------------------------------------

    private void buildWelcome() {
        screenTitle.setText(R.string.WELCOME_TITLE);
        chip.set(getString(R.string.CHIP_OFFLINE), Theme.OK);
        LinearLayout h = rowLayout();

        LinearLayout left = column();
        left.setGravity(Gravity.CENTER_VERTICAL);
        left.addView(Theme.caps(this, getString(R.string.app_name), Theme.ACCENT), lp(MATCH, WRAP));
        TextView hero = Theme.text(this, Theme.T_HERO, Theme.MEDIUM, Theme.TEXT, getString(R.string.WELCOME_HERO));
        LinearLayout.LayoutParams hl = lp(MATCH, WRAP);
        hl.topMargin = 20;
        hl.bottomMargin = 28;
        left.addView(hero, hl);
        left.addView(Theme.body(this, getString(R.string.WELCOME_TEXT)), lp(MATCH, WRAP));
        MenuRow start = row(Icon.ARROW, getString(R.string.WELCOME_START), getString(R.string.WELCOME_START_SUB),
                            () -> go(Screen.SOURCE));
        LinearLayout.LayoutParams sl = lp(640, Theme.ROW_H);
        sl.topMargin = 48;
        left.addView(start, sl);
        defaultFocus = start;
        LinearLayout.LayoutParams ll = lp(0, MATCH, 1);
        ll.rightMargin = 80;
        h.addView(left, ll);

        FrameLayout p = panel();
        LinearLayout steps = column();
        steps.addView(Theme.caps(this, getString(R.string.WELCOME_STEPS), Theme.ACCENT), lp(MATCH, WRAP));
        int[][] items = { { R.string.STEP1, R.string.STEP1_SUB }, { R.string.STEP2, R.string.STEP2_SUB },
                          { R.string.STEP3, R.string.STEP3_SUB } };
        Icon[] icons = { Icon.FOLDER, Icon.COPY, Icon.PLAY };
        for (int i = 0; i < items.length; i++) {
            MenuRow r = new MenuRow(this);
            r.set(icons[i], getString(items[i][0]), getString(items[i][1]));
            r.subMaxLines = 2;
            r.setFocusable(false);
            r.setClickable(false);
            LinearLayout.LayoutParams rl = lp(MATCH, 150);
            rl.topMargin = i == 0 ? 20 : 4;
            steps.addView(r, rl);
        }
        p.addView(steps);
        h.addView(p, lp(820, MATCH));

        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("A", R.string.HINT_START, () -> go(Screen.SOURCE));
        addHint("B", R.string.HINT_QUIT, this::back);
    }

    // ---- SOURCE and FOLDER_ACCESS --------------------------------------------------------------------------------

    /** The left half of a choice screen: a heading and an explanation in a panel. */
    private LinearLayout explainPanel(LinearLayout parent, int width, String caps, String head, String text) {
        FrameLayout p = panel();
        LinearLayout c = column();
        c.addView(Theme.caps(this, caps, Theme.ACCENT), lp(MATCH, WRAP));
        TextView t = Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, head);
        LinearLayout.LayoutParams tl = lp(MATCH, WRAP);
        tl.topMargin = 18;
        tl.bottomMargin = 22;
        c.addView(t, tl);
        c.addView(Theme.small(this, text), lp(MATCH, WRAP));
        p.addView(c);
        LinearLayout.LayoutParams pl = lp(width, MATCH);
        pl.rightMargin = Theme.GAP;
        parent.addView(p, pl);
        return c;
    }

    private MenuRow card(LinearLayout cards, Icon icon, int title, String sub, String badge, int badgeColor,
                         Runnable click) {
        MenuRow r = row(icon, getString(title), sub, click);
        boolean three = screen == Screen.SOURCE;   // three cards: a little shorter, so that all fit
        r.subMaxLines = three ? 3 : 4;
        r.setBadge(badge, badgeColor);
        LinearLayout.LayoutParams l = lp(MATCH, three ? Theme.CARD_H - 34 : Theme.CARD_H);
        l.bottomMargin = three ? 16 : 24;
        cards.addView(r, l);
        return r;
    }

    private void buildSource() {
        screenTitle.setText(R.string.SOURCE_TITLE);
        chip.set(getString(R.string.CHIP_OFFLINE), Theme.OK);
        LinearLayout h = rowLayout();
        LinearLayout ex = explainPanel(h, 700, getString(R.string.SOURCE_CAPS), getString(R.string.SOURCE_HEAD),
                                       getString(R.string.SOURCE_TEXT));
        TextView free = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_FAINT,
                                   getString(R.string.FREE_SPACE_APP, size(freeSpace())));
        LinearLayout.LayoutParams fl = lp(MATCH, WRAP);
        fl.topMargin = 26;
        ex.addView(free, fl);

        LinearLayout cards = column();
        boolean all = Environment.isExternalStorageManager();
        MenuRow folder = card(cards, Icon.FOLDER, R.string.CARD_FOLDER, getString(R.string.CARD_FOLDER_SUB),
                              getString(all ? R.string.BADGE_IN_PLACE : R.string.BADGE_FOLDER_ASK),
                              all ? Theme.OK : Theme.WARN, this::onChooseFolder);
        card(cards, Icon.DISC, R.string.CARD_ISO, getString(R.string.CARD_ISO_SUB),
             getString(R.string.BADGE_FREE, size(freeSpace())), Theme.TEXT_DIM, this::openIsoPicker);
        String libPath = Library.folderPath(this);
        card(cards, Icon.SWAP, R.string.LIB_CARD, getString(R.string.LIB_CARD_SUB),
             Library.folderTree(this) == null ? getString(R.string.LIB_BADGE_NEW)
             : libPath != null ? libPath : getString(R.string.LIB_BADGE_SET), Theme.ACCENT,
             this::onChooseLibraryFolder);
        defaultFocus = folder;
        h.addView(cards, lp(0, MATCH, 1));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("A", R.string.HINT_CHOOSE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private void buildFolderAccess() {
        screenTitle.setText(R.string.ALL_FILES_TITLE);
        LinearLayout h = rowLayout();
        explainPanel(h, 820, getString(pickLibrary ? R.string.LIB_CARD : R.string.CARD_FOLDER),
                     getString(R.string.ALL_FILES_TITLE),
                     getString(pickLibrary ? R.string.LIB_ALL_FILES_EXPLAIN : R.string.ALL_FILES_EXPLAIN));
        LinearLayout cards = column();
        MenuRow grant = card(cards, Icon.KEY, R.string.ALL_FILES_GRANT, getString(R.string.ALL_FILES_GRANT_SUB),
                             getString(R.string.BADGE_RECOMMENDED), Theme.OK, this::askAllFilesAccess);
        if (pickLibrary)
            card(cards, Icon.FOLDER, R.string.LIB_SAF, getString(R.string.LIB_SAF_SUB), null, 0,
                 this::openTreePicker);
        else
            card(cards, Icon.COPY, R.string.ALL_FILES_COPY, getString(R.string.ALL_FILES_COPY_SUB), null, 0,
                 this::openTreePicker);
        defaultFocus = grant;
        h.addView(cards, lp(0, MATCH, 1));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("A", R.string.HINT_CHOOSE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private void clickFocused() {
        View f = focused();
        if (f != null)
            f.performClick();
        else
            restoreFocus(true);
    }

    // ---- IMPORT ------------------------------------------------------------------------------------------------

    private void buildImport() {
        screenTitle.setText(R.string.IMPORT_TITLE);
        chip.set(getString(R.string.CHIP_SCREEN_ON), Theme.OK);
        LinearLayout h = rowLayout();
        FrameLayout left = new FrameLayout(this);
        ring = new Widgets.Ring(this);
        left.addView(ring, new FrameLayout.LayoutParams(620, 620, Gravity.CENTER));
        h.addView(left, lp(760, MATCH));

        FrameLayout p = panel();
        LinearLayout c = column();
        c.addView(Theme.caps(this, getString(R.string.IMPORT_CAPS), Theme.ACCENT), lp(MATCH, WRAP));
        importHead = Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, getString(importLabel));
        LinearLayout.LayoutParams hl = lp(MATCH, WRAP);
        hl.topMargin = 18;
        hl.bottomMargin = 26;
        c.addView(importHead, hl);
        importBytes = Theme.text(this, Theme.T_BODY + 4, Theme.MEDIUM, Theme.TEXT, "");
        c.addView(importBytes, lp(MATCH, WRAP));
        importFile = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM, "");
        importFile.setSingleLine(true);
        importFile.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        LinearLayout.LayoutParams fl = lp(MATCH, WRAP);
        fl.topMargin = 14;
        c.addView(importFile, fl);
        TextView free = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_FAINT,
                                   getString(R.string.FREE_SPACE_APP, size(freeSpace())));
        LinearLayout.LayoutParams frl = lp(MATCH, WRAP);
        frl.topMargin = 14;
        c.addView(free, frl);
        c.addView(gap(this), lp(MATCH, 0, 1));
        c.addView(Theme.body(this, getString(R.string.IMPORT_KEEPS)), lp(MATCH, WRAP));
        cancelRow = row(Icon.BACK, getString(R.string.CANCEL), getString(R.string.CANCEL_SUB), this::requestCancel);
        LinearLayout.LayoutParams cl = lp(MATCH, Theme.ROW_H);
        cl.topMargin = 24;
        c.addView(cancelRow, cl);
        p.addView(c);
        h.addView(p, lp(0, MATCH, 1));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        defaultFocus = cancelRow;
        addHint("B", R.string.CANCEL, this::requestCancel);
        ring.set(-1, "", getString(R.string.IMPORT_STARTING), "");
    }

    private void requestCancel() {
        if (worker == null || !worker.isAlive() || cancelRequested)
            return;
        cancelRequested = true;
        if (cancelRow != null)
            cancelRow.setDim(true);
        setNote(getString(R.string.CANCELING), NOTE_INFO);
    }

    // ---- HOME --------------------------------------------------------------------------------------------------

    /**
     * A console home: the hero (the game's art, its title, a status chip, a lead line, a few facts and one big Play
     * or Prepare) over a row of four smaller tiles (Settings, Controls, Game data, About). The top bar only says
     * « Home », so the title shows once.
     */
    private void buildHome() {
        State st = state();
        if (st == State.LIBRARY) {
            buildHomeNotHere();
            return;
        }
        String dir = gameDir();
        boolean running = gameRunning();
        screenTitle.setText(R.string.HOME_TITLE);
        Prep prep = prep();
        PrepareProtocol.Status ps = prep == Prep.OFF || prep == Prep.PLAYABLE ? null : readPrepStatus();
        boolean preparing = prep != Prep.OFF && prep != Prep.PLAYABLE;
        checkHashLater(dir);
        sizeLater(dir);

        LinearLayout col = column();
        LinearLayout hero = rowLayout();
        hero.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.ArtTile tile = new Widgets.ArtTile(this);
        String title = st == State.NONE ? getString(R.string.NO_GAME_HEAD) : gameTitle(dir);
        if (st == State.NONE)
            tile.setEmpty();
        else if (art != null)
            tile.setArt(art, artColor);
        else
            tile.setMonogram(TitleArt.initials(title), artColor);
        hero.addView(tile, lp(Theme.ART, Theme.ART));

        LinearLayout info = column();
        String tid = st == State.NONE ? null : titleId();
        if (tid != null)
            info.addView(Theme.caps(this, getString(R.string.HOME_TITLE_ID, tid), Theme.TEXT_FAINT), lp(MATCH, WRAP));
        TextView big = Theme.text(this, Theme.T_HERO, Theme.MEDIUM, Theme.TEXT, title);
        big.setMaxLines(2);
        big.setEllipsize(TextUtils.TruncateAt.END);
        big.setLineSpacing(0, 1.05f);
        LinearLayout.LayoutParams bl = lp(MATCH, WRAP);
        bl.topMargin = tid != null ? 14 : 0;
        info.addView(big, bl);

        LinearLayout chips = rowLayout();
        chips.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.Chip status = new Widgets.Chip(this);
        if (preparing)
            setPrepChip(status, prep);
        else
            status.set(getString(stateLabel(st)), stateColor(st));
        chips.addView(status, lp(WRAP, WRAP));
        if (companion != null) {
            companion.changeLabel(libraryOn() ? getString(R.string.LIB_TITLE) : null);
            companion.art(st == State.NONE ? null : art);
            companion.home(st == State.NONE ? "" : title, status.label(), status.color(),
                           st == State.READY && !preparing);
        }
        if (running && st == State.READY && selectedIsLaunched()) {
            Widgets.Chip run = new Widgets.Chip(this);
            run.set(getString(R.string.CHIP_RUNNING), Theme.OK);
            LinearLayout.LayoutParams rl = lp(WRAP, WRAP);
            rl.leftMargin = 16;
            chips.addView(run, rl);
        }
        LinearLayout.LayoutParams cl = lp(MATCH, WRAP);
        cl.topMargin = 22;
        info.addView(chips, cl);

        String lead;
        int leadColor = Theme.TEXT_DIM;
        if (st == State.READY && preparing) {
            switch (prep) {
            case RUNNING:
                lead = getString(R.string.HOME_LEAD_PREPARING);
                break;
            case FAILED:
                lead = getString(R.string.HOME_LEAD_FAILED);
                break;
            default:
                lead = getString(R.string.HOME_LEAD_PREPARE);
                break;
            }
        } else if (st == State.READY) {
            lead = getString(running && selectedIsLaunched() ? R.string.PLAY_DETAIL_RUNNING : R.string.HOME_LEAD_READY);
        } else {
            lead = stateText(st, dir);
            leadColor = st == State.NONE ? Theme.TEXT_DIM : st == State.WRONG ? Theme.ERROR : Theme.WARN;
        }
        TextView leadV = Theme.text(this, Theme.T_BODY, Theme.REGULAR, leadColor, lead);
        leadV.setMaxLines(2);
        leadV.setEllipsize(TextUtils.TruncateAt.END);
        leadV.setLineSpacing(0, 1.2f);
        LinearLayout.LayoutParams ll = lp(MATCH, WRAP);
        ll.topMargin = 22;
        info.addView(leadV, ll);

        LinearLayout facts = homeFacts(st, prep, ps);
        if (facts != null) {
            LinearLayout.LayoutParams fl = lp(MATCH, WRAP);
            fl.topMargin = 26;
            info.addView(facts, fl);
        }

        MenuRow play;
        if (st == State.NONE || st == State.MISSING || st == State.NO_ACCESS) {
            play = row(Icon.FOLDER, getString(st == State.NONE ? R.string.CHOOSE_GAME : R.string.CHOOSE_AGAIN),
                       getString(R.string.CHOOSE_GAME_SUB), () -> go(Screen.SOURCE));
        } else if (preparing && prep == Prep.QUEUED) {
            play = row(Icon.ARROW, queuedLabel(tid), getString(R.string.LIB_QUEUED_SUB), this::onPlay);
            homePrep = prep;
        } else if (preparing) {
            int t = prep == Prep.RUNNING ? R.string.PREPARING : prep == Prep.FAILED ? R.string.PREPARE_SEE_WHY
                    : R.string.PREPARE;
            play = row(prep == Prep.FAILED ? Icon.INFO : Icon.ARROW, getString(t), prepSub(prep, ps), this::onPlay);
            homePrep = prep;
        } else {
            String playSub = st != State.READY ? getString(R.string.PLAY_SUB_UNAVAILABLE, getString(stateLabel(st)))
                             : running && selectedIsLaunched() ? getString(R.string.PLAY_SUB_RUNNING)
                             : running ? getString(R.string.LIB_PLAY_SUB_SWITCH, runningTitle())
                             : getString(R.string.PLAY_SUB);
            play = row(Icon.PLAY, getString(R.string.PLAY), playSub, this::onPlay);
            play.setDim(st != State.READY);
        }
        play.setPrimary(true);
        homePrimary = play;
        LinearLayout.LayoutParams pl = lp(Theme.PRIMARY_W, Theme.PRIMARY_H);
        pl.topMargin = 30;
        pl.leftMargin = -12;   // the row's own inset: its outline lines up with the texts above
        info.addView(play, pl);

        LinearLayout.LayoutParams il = lp(0, WRAP, 1);
        il.leftMargin = 52;
        hero.addView(info, il);
        col.addView(hero, lp(MATCH, 0, 1));

        LinearLayout tiles = rowLayout();
        MenuRow settings = homeTile(tiles, Icon.SLIDERS, R.string.SETTINGS, settingsSummary(), Screen.SETTINGS);
        homeTile(tiles, Icon.PAD, R.string.CONTROLS, getString(R.string.CONTROLS_SUB), Screen.CONTROLS);
        homeTile(tiles, Icon.DISC, R.string.GAME_DATA, dir == null ? getString(stateLabel(st))
                 : sizeText(dir) + " · " + sourceShort(dir), Screen.GAME_DATA);
        MenuRow about = homeTile(tiles, Icon.INFO, R.string.ABOUT, getString(R.string.ABOUT_SUB, version()),
                                 Screen.ABOUT);
        ((LinearLayout.LayoutParams)settings.getLayoutParams()).leftMargin = -12;
        ((LinearLayout.LayoutParams)about.getLayoutParams()).rightMargin = -12;
        LinearLayout.LayoutParams tl = lp(MATCH, Theme.TILE_H);
        tl.topMargin = 28;
        col.addView(tiles, tl);
        body.addView(col, new FrameLayout.LayoutParams(MATCH, MATCH));

        defaultFocus = play;
        startAction = this::onPlay;
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", libraryOn() && history.isEmpty() ? R.string.LIB_TITLE : history.isEmpty() ? R.string.HINT_QUIT
                                                                                                : R.string.HINT_BACK,
                this::back);
        boolean choose = st == State.NONE || st == State.MISSING || st == State.NO_ACCESS;
        addHint("START", choose ? R.string.HINT_CHOOSE : preparing ? R.string.PREPARE : R.string.PLAY, this::onPlay);

        if (preparing && prep == Prep.INTERRUPTED)
            setNote(getString(R.string.PREPARE_INTERRUPTED), NOTE_WARN);
        else if (running && selectedIsLaunched())
            setNote(getString(R.string.GAME_RUNNING_HOME), NOTE_INFO);
        else if (running && st == State.READY)
            setNote(getString(R.string.LIB_OTHER_RUNNING, runningTitle()), NOTE_INFO);
    }

    private MenuRow homeTile(LinearLayout tiles, Icon icon, int title, String sub, Screen target) {
        MenuRow r = row(icon, getString(title), sub, () -> go(target));
        r.setCompact();
        LinearLayout.LayoutParams p = lp(0, MATCH, 1);
        p.leftMargin = 6;
        p.rightMargin = 6;
        tiles.addView(r, p);
        return r;
    }

    /** One fact of the hero: a caps label over its value. */
    private TextView homeFact(LinearLayout facts, String label, String value, int color) {
        LinearLayout c = column();
        c.addView(Theme.caps(this, label, Theme.TEXT_FAINT), lp(WRAP, WRAP));
        TextView v = Theme.oneLine(Theme.text(this, Theme.T_BODY + 3, Theme.MEDIUM, color, value));
        LinearLayout.LayoutParams vl = lp(WRAP, WRAP);
        vl.topMargin = 10;
        c.addView(v, vl);
        LinearLayout.LayoutParams p = lp(WRAP, WRAP);
        p.rightMargin = 64;
        facts.addView(c, p);
        return v;
    }

    /** The hero's facts for the state: play stats, the pack, what a Prepare needs, or its progress; null: none. */
    private LinearLayout homeFacts(State st, Prep prep, PrepareProtocol.Status ps) {
        if (st != State.READY)
            return null;
        LinearLayout f = rowLayout();
        switch (prep) {
        case RUNNING: {
            LinearLayout c = column();
            homeMeter = new Widgets.Meter(this);
            float frac = ps != null && ps.step > 0 ? PrepareProtocol.overall(ps.step, ps.n, ps.pct) : 0;
            homeMeter.set(frac, Theme.ACCENT);
            c.addView(homeMeter, lp(Theme.PRIMARY_W - 24, 10));
            homeProgress = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT,
                                                    ps == null ? getString(R.string.PREPARE_STARTING)
                                                               : progressLine(ps)));
            LinearLayout.LayoutParams pl = lp(MATCH, WRAP);
            pl.topMargin = 16;
            c.addView(homeProgress, pl);
            f.addView(c, lp(MATCH, WRAP));
            return f;
        }
        case FAILED:
            homeFact(f, getString(R.string.KV_CODE), ps == null || ps.code.isEmpty() ? "?" : ps.code, Theme.ERROR);
            homeFact(f, getString(R.string.KV_INTERNAL_FREE), size(internalFree()), Theme.TEXT);
            return f;
        case NEEDED:
        case CANCELLED:
        case INTERRUPTED: {
            homeFact(f, getString(R.string.KV_ESTIMATE), estimate(), Theme.TEXT);
            long free = internalFree();
            homeFact(f, getString(R.string.KV_INTERNAL_FREE), size(free),
                     free < PREPARE_MIN_FREE ? Theme.ERROR : Theme.TEXT);
            long ram = Toolchain.totalRam(this);
            homeFact(f, getString(R.string.KV_RAM), ram > 0 ? size(ram) : "?",
                     ram > 0 && ram < PREPARE_RAM_WARN ? Theme.WARN : Theme.TEXT);
            return f;
        }
        default:
            break;
        }
        Library.Record rec = rec();
        long last = rec != null ? rec.lastPlayed : GameStore.lastPlayed(this), now = System.currentTimeMillis();
        homeFact(f, getString(R.string.KV_LAST_PLAYED), lastPlayedText(last, now), Theme.TEXT);
        homeFact(f, getString(R.string.KV_PLAY_TIME), playTimeText(rec != null ? rec.playMs : GameStore.playTime(this)),
                 Theme.TEXT);
        String tid = titleId();
        if (prep == Prep.PLAYABLE && tid != null) {
            String[] pack = packFacts(tid);
            homeFact(f, getString(R.string.KV_PREPARED), pack[0], Theme.TEXT);
            homeFact(f, getString(R.string.KV_PACK_SIZE), pack[1], Theme.TEXT);
        } else {
            String dir = gameDir();
            homeFact(f, getString(R.string.KV_SIZE), dir == null ? "?" : sizeText(dir), Theme.TEXT);
        }
        return f;
    }

    private String lastPlayedText(long last, long now) {
        return last <= 0 ? getString(R.string.VAL_NEVER)
                         : DateUtils.getRelativeTimeSpanString(last, now, DateUtils.MINUTE_IN_MILLIS).toString();
    }

    /** « 45 min », « 3 h 05 », « — » for none. */
    private String playTimeText(long ms) {
        long min = ms / 60_000;
        if (min <= 0)
            return ms > 0 ? getString(R.string.PLAYTIME_MIN, 1) : getString(R.string.VAL_NONE);
        if (min < 60)
            return getString(R.string.PLAYTIME_MIN, (int)min);
        return getString(R.string.PLAYTIME_H, (int)(min / 60), String.format(Locale.ROOT, "%02d", min % 60));
    }

    /** The pack's prepared date (pack.json's built_at, else the file's date) and its library's size. */
    private String[] packFacts(String tid) {
        File dir = Pack.packDir(getFilesDir(), tid);
        File json = new File(dir, Pack.JSON);
        long when = json.lastModified();
        long bytes = -1;
        try {
            JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 20));
            String at = o.optString("built_at", "");
            if (!at.isEmpty()) {
                try {
                    when = java.time.Instant.parse(at).toEpochMilli();
                } catch (java.time.format.DateTimeParseException ignored) {
                }
            }
            String lib = o.optString("lib", "");
            File so = new File(dir, lib);
            if (!lib.isEmpty() && so.isFile())
                bytes = so.length();
        } catch (IOException | JSONException ignored) {
        }
        java.util.Locale loc = getResources().getConfiguration().getLocales().get(0);
        String date = when > 0 ? java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM, loc).format(when)
                               : "?";
        return new String[] { date, bytes >= 0 ? size(bytes) : "?" };
    }

    private void setPrepChip(Widgets.Chip c, Prep p) {
        switch (p) {
        case RUNNING:
            c.set(getString(R.string.CHIP_PREPARING), Theme.OK);
            break;
        case QUEUED:
            c.set(getString(R.string.CHIP_QUEUED), Theme.ACCENT);
            break;
        case FAILED:
            c.set(getString(R.string.CHIP_PREPARE_FAILED), Theme.ERROR);
            break;
        default:
            c.set(getString(R.string.CHIP_NEEDS_PREPARE), Theme.WARN);
            break;
        }
    }
    private void onPlay() {
        State st = state();
        if (st == State.LIBRARY) {
            libraryAction(currentEntry());
            return;
        }
        if (st == State.READY) {
            Prep p = prep();
            if (p == Prep.RUNNING || p == Prep.FAILED) {
                go(Screen.PREPARE);
                return;
            }
            if (p == Prep.QUEUED) {   // A takes it out of the queue (a new start would only keep its place)
                dequeuePrepare(titleId());
                return;
            }
            if (p != Prep.OFF && p != Prep.PLAYABLE) {
                startPrepare();
                return;
            }
            if (!confirmSwitch())
                return;
            launchShell(false);
            return;
        }
        if (st == State.WRONG) {
            setNote(getString(R.string.WRONG_GAME_HOME), NOTE_ERROR);
            return;
        }
        String why = stateText(st, gameDir());
        go(Screen.SOURCE);
        setNote(why, NOTE_WARN);
    }

    private String settingsSummary() {
        RecompConfig c = RecompConfig.load(GameStore.externalFilesDir(this));
        List<String> changed = new ArrayList<>();
        for (RecompConfig.Option o : RecompConfig.shownOptions(lsfgBuilt())) {
            if (c.choiceIndex(o) != o.defaultIndex())
                changed.add(getString(o.title) + " " + choiceLabel(c, o).toLowerCase(Locale.getDefault()));
        }
        if (changed.isEmpty())
            return getString(R.string.SETTINGS_SUB_DEFAULT);
        return TextUtils.join(" · ", changed);
    }

    // ---- SETTINGS ----------------------------------------------------------------------------------------------

    private final List<Integer> sectionStarts = new ArrayList<>();

    private void buildSettings() {
        screenTitle.setText(R.string.SETTINGS);
        cfg = RecompConfig.load(GameStore.externalFilesDir(this));
        sectionStarts.clear();

        LinearLayout h = rowLayout();
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setClipToPadding(false);
        scroll.setPadding(0, 0, 0, 24);
        LinearLayout list = column();
        list.setPadding(0, 4, 0, 4);
        scroll.addView(list, new FrameLayout.LayoutParams(MATCH, WRAP));

        sectionHeader(list, R.string.SECTION_LAUNCHER);
        Runnable autoDetail = launcherToggle(list, R.string.OPEN_DIRECTLY, R.string.OPEN_DIRECTLY_SUMMARY,
                                             R.string.OPEN_DIRECTLY_DETAIL, R.string.OPEN_DIRECTLY_FOOTER,
                                             GameStore::autoStart, GameStore::setAutoStart,
                                             R.string.OPEN_DIRECTLY_ON, R.string.OPEN_DIRECTLY_OFF);
        launcherToggle(list, R.string.UI_SOUND, R.string.UI_SOUND_SUMMARY, R.string.UI_SOUND_DETAIL,
                       R.string.UI_FEEDBACK_FOOTER, GameStore::uiSound, GameStore::setUiSound, R.string.UI_SOUND_ON,
                       R.string.UI_SOUND_OFF);
        launcherToggle(list, R.string.UI_HAPTICS, R.string.UI_HAPTICS_SUMMARY, R.string.UI_HAPTICS_DETAIL,
                       R.string.UI_FEEDBACK_FOOTER, GameStore::uiHaptics, GameStore::setUiHaptics,
                       R.string.UI_HAPTICS_ON, R.string.UI_HAPTICS_OFF);
        if (DualScreen.available(this, CompanionScreen.ownDisplayId(this)))
            bottomScreenRow(list);
        String dir = gameDir();
        if (dir != null && state() != State.NONE && GameStore.findXbe(new File(dir)) != null && titleId() != null)
            gameThemeRow(list, titleId(), gameTitle(dir));

        RecompConfig.Section section = null;
        List<MenuRow> optionRows = new ArrayList<>();
        List<RecompConfig.Option> shown = RecompConfig.shownOptions(lsfgBuilt());
        lsfgOff = lsfgForcedOff();
        for (RecompConfig.Option o : shown) {
            if (o.section != section) {
                section = o.section;
                sectionHeader(list, sectionTitle(section));
            }
            MenuRow r = settingRow(list, getString(o.title), getString(o.summary));
            String[] labels = new String[o.choices.length + 1];
            for (int i = 0; i < o.choices.length; i++)
                labels[i] = getString(o.choices[i].label);
            labels[o.choices.length] = getString(R.string.VAL_CUSTOM);
            r.setChoices(labels);
            optionRows.add(r);
            refreshOptionRow(r, o);
            r.cycler = d -> stepOption(r, o, d);
            r.setOnClickListener(v -> stepOption(r, o, 1));
            onFocus(r, () -> optionDetail(o));
            if (o.needsLsfg && lsfgOff) {
                // The game's pack forces it off (pack.json env_force RECOMP_LSFG=0): the setting stays, for the
                // other games, but says it does nothing for this one.
                r.setSubtitle(getString(R.string.LSFG_TITLE_UNAVAILABLE));
                r.setDim(true);
            }
            if (o.needsLsfg)
                lsfgDllRow(list);
        }

        View spacer = gap(this);
        list.addView(spacer, lp(MATCH, 20));
        MenuRow reset = row(Icon.RESET, getString(R.string.RESET), getString(R.string.RESET_SUB), null);
        reset.setOnClickListener(v -> resetSettings(optionRows, shown));
        list.addView(reset, lp(MATCH, Theme.ROW_H_SMALL));
        onFocus(reset, () -> {
            detailClear(null, getString(R.string.RESET), getString(R.string.RESET_DETAIL));
            detailFooter(getString(R.string.SETTINGS_FOOTER, cfg.file().getAbsolutePath()), Theme.TEXT_FAINT);
        });

        LinearLayout.LayoutParams sl = lp(920, MATCH);
        sl.rightMargin = Theme.GAP;
        h.addView(scroll, sl);
        h.addView(detailPanel(), lp(0, MATCH, 1));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        autoDetail.run();

        l1Action = () -> jumpSection(-1);
        r1Action = () -> jumpSection(1);
        yAction = () -> resetSettings(optionRows, shown);
        addHint("L1/R1", R.string.HINT_SECTION, () -> jumpSection(1));
        addHint("Y", R.string.HINT_DEFAULTS, () -> resetSettings(optionRows, shown));
        addHint("A", R.string.HINT_CHANGE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
        if (gameRunning())
            setNote(getString(R.string.SETTINGS_GAME_RUNNING), NOTE_INFO);
    }

    private int sectionTitle(RecompConfig.Section s) {
        switch (s) {
        case GAME:
            return R.string.SECTION_GAME;
        case GRAPHICS:
            return R.string.SECTION_GRAPHICS;
        case AUDIO:
            return R.string.SECTION_AUDIO;
        default:
            return R.string.SECTION_ADVANCED;
        }
    }

    private void sectionHeader(LinearLayout list, int title) {
        TextView t = Theme.caps(this, getString(title), Theme.TEXT_FAINT);
        t.setPadding(36, 0, 0, 0);
        LinearLayout.LayoutParams p = lp(MATCH, WRAP);
        p.topMargin = list.getChildCount() == 0 ? 4 : 30;
        p.bottomMargin = 8;
        list.addView(t, p);
        sectionStarts.add(focusables.size());
    }

    /** A setting row: its press steps the value, which plays the « move » tick instead of « select ». */
    private MenuRow settingRow(LinearLayout list, String title, String sub) {
        MenuRow r = row(null, title, sub, null);
        r.onPress = null;
        list.addView(r, lp(MATCH, Theme.ROW_H_SMALL));
        return r;
    }

    /**
     * « Bottom screen: Panel / Black / Off » (GameStore.bottomScreen), only on a device with a second display: what
     * the launcher and the game show there (docs/android/app-shell.md « Dual screen »). Applied at once.
     */
    private void bottomScreenRow(LinearLayout list) {
        GameStore.BottomScreen[] modes = GameStore.BottomScreen.values();
        int[] labels = { R.string.VAL_PANEL, R.string.VAL_BLACK, R.string.VAL_OFF };
        String[] names = new String[modes.length];
        for (int i = 0; i < modes.length; i++)
            names[i] = getString(labels[i]);
        MenuRow r = settingRow(list, getString(R.string.BOTTOM_SCREEN), getString(R.string.BOTTOM_SCREEN_SUMMARY));
        r.setChoices(names);
        Runnable fill = () -> {
            detailClear(getString(R.string.SECTION_LAUNCHER), getString(R.string.BOTTOM_SCREEN),
                        getString(R.string.BOTTOM_SCREEN_DETAIL));
            detailKv(getString(R.string.KV_VALUE), names[GameStore.bottomScreen(this).ordinal()]);
            detailFooter(getString(R.string.UI_FEEDBACK_FOOTER), Theme.TEXT_FAINT);
        };
        Runnable refresh = () -> {
            int i = GameStore.bottomScreen(this).ordinal();
            r.setValue(names[i], i, modes.length);
        };
        refresh.run();
        r.cycler = d -> {
            int n = modes.length, i = ((GameStore.bottomScreen(this).ordinal() + d) % n + n) % n;
            GameStore.setBottomScreen(this, modes[i]);
            refresh.run();
            fill.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            if (companion != null)
                companion.screen().refresh();
            setNote(getString(R.string.BOTTOM_SCREEN_SAVED, names[i]), NOTE_OK);
            Log.i(TAG, "launcher: bottom screen " + modes[i]);
        };
        r.setOnClickListener(v -> r.cycler.step(1));
        onFocus(r, fill);
    }

    /**
     * Settings › Launcher › Game theme, per title: « Art and colours », « Colours only », « Off ». Applies at once
     * here (the stage is built again in the new colours), in the game's menu at its next start.
     */
    private void gameThemeRow(LinearLayout list, String tid, String title) {
        GameTheme.Mode[] modes = GameTheme.Mode.values();
        int[] labels = { R.string.VAL_THEME_FULL, R.string.VAL_THEME_COLORS, R.string.VAL_THEME_OFF };
        String[] names = new String[modes.length];
        for (int i = 0; i < modes.length; i++)
            names[i] = getString(labels[i]);
        MenuRow r = settingRow(list, getString(R.string.GAME_THEME), getString(R.string.GAME_THEME_SUMMARY));
        r.setChoices(names);
        Runnable fill = () -> {
            detailClear(getString(R.string.SECTION_LAUNCHER), getString(R.string.GAME_THEME),
                        getString(R.string.GAME_THEME_DETAIL, title));
            detailKv(getString(R.string.KV_VALUE), names[GameTheme.mode(this, tid).ordinal()]);
            GameTheme g = GameTheme.cached(this, new File(gameDir()), tid);
            if (g != null)
                detailKv(getString(R.string.KV_IMAGE), g.source.equals("none")
                                                         ? getString(R.string.GAME_THEME_SOURCE_NONE) : g.source);
            detailFooter(getString(R.string.GAME_THEME_FOOTER), Theme.TEXT_FAINT);
        };
        int cur = GameTheme.mode(this, tid).ordinal();
        r.setValue(names[cur], cur, modes.length);
        r.cycler = d -> {
            int n = modes.length, i = ((GameTheme.mode(this, tid).ordinal() + d) % n + n) % n;
            GameTheme.setMode(this, tid, modes[i]);
            feedback.play(UiFeedback.Cue.MOVE, r);
            Log.i(TAG, "launcher: game theme " + tid + " " + modes[i].key);
            show(Screen.SETTINGS, 0);   // syncTheme: the new colours, the focus stays on this row
            setNote(getString(R.string.GAME_THEME_SAVED, names[i]), NOTE_OK);
        };
        r.setOnClickListener(v -> r.cycler.step(1));
        onFocus(r, fill);
    }

    /** An on / off launcher preference as a setting row; returns its detail filler (shown when focused). */
    private Runnable launcherToggle(LinearLayout list, int title, int summary, int detailText, int footer,
                                    Function<android.content.Context, Boolean> get,
                                    BiFunction<android.content.Context, Boolean, Boolean> set, int onNote,
                                    int offNote) {
        MenuRow r = settingRow(list, getString(title), getString(summary));
        r.setChoices(new String[] { getString(R.string.VAL_ON), getString(R.string.VAL_OFF) });
        Runnable fill = () -> {
            detailClear(getString(R.string.SECTION_LAUNCHER), getString(title), getString(detailText));
            detailKv(getString(R.string.KV_VALUE), getString(get.apply(this) ? R.string.VAL_ON : R.string.VAL_OFF));
            detailFooter(getString(footer), Theme.TEXT_FAINT);
        };
        Runnable refresh = () -> {
            boolean on = get.apply(this);
            r.setValue(getString(on ? R.string.VAL_ON : R.string.VAL_OFF), on ? 1 : 0, 2);
        };
        refresh.run();
        r.cycler = d -> {
            set.apply(this, !get.apply(this));
            refresh.run();
            fill.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            setNote(getString(get.apply(this) ? onNote : offNote), NOTE_OK);
            Log.i(TAG, "launcher: " + getResources().getResourceEntryName(title) + " "
                       + (get.apply(this) ? "on" : "off"));
        };
        r.setOnClickListener(v -> r.cycler.step(1));
        onFocus(r, fill);
        return fill;
    }

    private void jumpSection(int dir) {
        int cur = focusables.indexOf(focused());
        int target = -1;
        if (dir > 0) {
            for (int s : sectionStarts) {
                if (s > cur) {
                    target = s;
                    break;
                }
            }
        } else {
            for (int i = sectionStarts.size() - 1; i >= 0; i--) {
                if (sectionStarts.get(i) < cur) {
                    target = sectionStarts.get(i);
                    break;
                }
            }
        }
        if (target >= 0 && target < focusables.size())
            focusables.get(target).requestFocusFromTouch();
    }

    private String choiceLabel(RecompConfig c, RecompConfig.Option o) {
        int i = c.choiceIndex(o);
        return i < 0 ? getString(R.string.VAL_CUSTOM) : getString(o.choices[i].label);
    }

    private void refreshOptionRow(MenuRow r, RecompConfig.Option o) {
        int i = cfg.choiceIndex(o);
        r.setValue(choiceLabel(cfg, o), i, o.choices.length);
    }

    private void stepOption(MenuRow r, RecompConfig.Option o, int dir) {
        int n = o.choices.length, i = cfg.choiceIndex(o);
        int next = i < 0 ? o.defaultIndex() : ((i + dir) % n + n) % n;
        cfg.set(o, o.choices[next]);
        saveSettings();
        refreshOptionRow(r, o);
        optionDetail(o);
        feedback.play(UiFeedback.Cue.MOVE, r);
    }

    private void saveSettings() {
        try {
            cfg.save();
            Log.i(TAG, "launcher: settings saved to " + cfg.file());
            setNote(getString(gameRunning() ? R.string.SETTINGS_SAVED_RUNNING : R.string.SETTINGS_SAVED), NOTE_OK);
        } catch (IOException e) {
            Log.w(TAG, "launcher: cannot save settings", e);
            setNote(getString(R.string.SETTINGS_SAVE_FAILED, String.valueOf(e.getMessage())), NOTE_ERROR);
        }
    }

    private void resetSettings(List<MenuRow> rows, List<RecompConfig.Option> shown) {
        cfg.resetAll();
        saveSettings();
        for (int i = 0; i < shown.size(); i++)
            refreshOptionRow(rows.get(i), shown.get(i));
        setNote(getString(R.string.RESET_DONE), NOTE_OK);
    }

    private void optionDetail(RecompConfig.Option o) {
        detailClear(getString(sectionTitle(o.section)), getString(o.title), getString(o.detail));
        if (o.needsLsfg && lsfgOff) {
            TextView t = Theme.text(this, Theme.T_SMALL, Theme.MEDIUM, Theme.WARN,
                                    getString(R.string.LSFG_TITLE_UNAVAILABLE_DETAIL));
            LinearLayout.LayoutParams p = lp(MATCH, WRAP);
            p.topMargin = 12;
            detail.addView(t, p);
        }
        List<String> names = new ArrayList<>();
        for (RecompConfig.Choice c : o.choices)
            names.add(getString(c.label) + (c.isDefault() ? " " + getString(R.string.DEFAULT_MARK) : ""));
        detailKv(getString(R.string.KV_CHOICES), TextUtils.join(" · ", names));
        String raw = cfg.rawValue(o);
        detailKv(getString(R.string.KV_CFG), raw.isEmpty() ? getString(R.string.CFG_NOTHING) : raw);
        detailFooter(getString(R.string.APPLIES_NEXT_START), Theme.TEXT_FAINT);
    }

    // ---- Frame generation: « Choose Lossless.dll » ---------------------------------------------------------------

    /** Tests: stands in for R.bool.xba_lsfg when set (the unit tests' APK is built without frame generation). */
    static Boolean lsfgBuiltOverride;

    /** The selected game's pack forces frame generation off (read when Settings is built). */
    private boolean lsfgOff;

    /** True when this APK has frame generation (-PxbaLsfg=1 with the Vulkan bridge, docs/technical/framegen.md). */
    private boolean lsfgBuilt() {
        return lsfgBuiltOverride != null ? lsfgBuiltOverride : getResources().getBoolean(R.bool.xba_lsfg);
    }

    /**
     * The selected game's pack forces frame generation off (pack.json env_force RECOMP_LSFG=0, from its title
     * profile): the runtime sets it over recomp.cfg, so the Settings row says it is unavailable for this game.
     */
    private boolean lsfgForcedOff() {
        String tid = titleId();
        return tid != null && Pack.forcesOff(getFilesDir(), tid, "RECOMP_LSFG");
    }

    /** getFilesDir()/lsfg: the shader cache base (excluded from backups and from LogProvider). */
    private File lsfgBase() {
        return new File(getFilesDir(), "lsfg");
    }

    /**
     * The state of the imported shaders, from the cache directory recomp.cfg names (RECOMP_LSFG_DIR): one line for
     * the row and the detail panel. kind gets a NOTE_* colour.
     */
    private String lsfgStatus(int[] kind) {
        String dir = cfg == null ? null : cfg.get(RecompConfig.LSFG_DIR_KEY);
        kind[0] = NOTE_INFO;
        if (dir == null || dir.isEmpty() || !new File(dir).isDirectory())
            return getString(R.string.LSFG_NO_DLL);
        long[] m;
        try {
            m = LsfgNative.readManifest(dir);
        } catch (UnsatisfiedLinkError e) {
            return getString(R.string.LSFG_NO_DLL);
        }
        return lsfgStatusOf(m, kind);
    }

    private String lsfgStatusOf(long[] m, int[] kind) {
        if (m == null || m[LsfgNative.STATUS] != 0) {
            kind[0] = NOTE_ERROR;
            return getString(R.string.LSFG_UNSUPPORTED);
        }
        boolean full = m[LsfgNative.READY] != 0, perf = m[LsfgNative.READY + 1] != 0;
        if (full || perf) {
            kind[0] = NOTE_OK;
            return getString(full ? R.string.LSFG_READY : R.string.LSFG_READY_PERF_ONLY);
        }
        if (m[LsfgNative.DXBC] >= m[LsfgNative.NEEDED] || m[LsfgNative.DXBC + 1] >= m[LsfgNative.NEEDED + 1]) {
            kind[0] = NOTE_WARN;
            return getString(R.string.LSFG_NO_TRANSLATOR);
        }
        kind[0] = NOTE_ERROR;
        return getString(R.string.LSFG_UNSUPPORTED);
    }

    /** « Choose Lossless.dll », under the frame generation option. */
    private void lsfgDllRow(LinearLayout list) {
        int[] kind = new int[1];
        String status = lsfgStatus(kind);
        MenuRow r = row(Icon.KEY, getString(R.string.LSFG_CHOOSE_DLL), status, this::openLsfgDllPicker);
        list.addView(r, lp(MATCH, Theme.ROW_H_SMALL));
        onFocus(r, () -> {
            detailClear(getString(R.string.SECTION_GRAPHICS), getString(R.string.LSFG_CHOOSE_DLL),
                        getString(R.string.LSFG_CHOOSE_DLL_DETAIL));
            detailKv(getString(R.string.KV_STATUS), status);
            String dir = cfg.get(RecompConfig.LSFG_DIR_KEY);
            detailKv(getString(R.string.KV_CFG), dir == null ? getString(R.string.CFG_NOTHING)
                                                             : RecompConfig.LSFG_DIR_KEY + "=" + dir);
            detailFooter(getString(R.string.LSFG_LEGAL_FOOTER), Theme.TEXT_FAINT);
        });
    }

    private void openLsfgDllPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try {
            startActivityForResult(i, REQ_LSFG_DLL);
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.PICKER_MISSING), NOTE_ERROR);
        }
    }

    private void onLsfgDllPicked(Uri uri) {
        String name = displayName(uri);
        Log.i(TAG, "launcher: Lossless.dll picked (" + name + ")");
        if (!name.toLowerCase(Locale.ROOT).endsWith(".dll")) {
            setNote(getString(R.string.LSFG_NOT_A_DLL, name), NOTE_ERROR);
            return;
        }
        runWorker(R.string.LSFG_IMPORTING, () -> importLsfgDll(uri, name));
    }

    /**
     * Worker: the DLL is read through the picker's fd only (never copied into the app's storage); its shaders go to
     * getFilesDir()/lsfg/<key> (libxba_import.so), and recomp.cfg gets RECOMP_LSFG_DIR when they are usable.
     */
    private void importLsfgDll(Uri uri, String name) throws IOException {
        long[] m;
        String[] dir = new String[1], err = new String[1];
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
            if (pfd == null)
                throw new IOException("cannot open " + name);
            File base = lsfgBase();
            if (!base.isDirectory() && !base.mkdirs())
                throw new IOException("cannot create " + base);
            m = LsfgNative.importDll(pfd.getFd(), base.getAbsolutePath(), dir, err);
        }
        int[] kind = new int[1];
        String status;
        if (m == null || (m[LsfgNative.STATUS] != 0 && m[LsfgNative.NEEDED] == 0)) {
            status = getString(R.string.LSFG_UNSUPPORTED);
            kind[0] = NOTE_ERROR;
            Log.w(TAG, "launcher: Lossless.dll import failed: " + err[0]);
        } else {
            status = lsfgStatusOf(new long[] { 0, m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10],
                                               m[11], m[12], m[13] }, kind);
            if (m[LsfgNative.STATUS] != 0 && kind[0] == NOTE_OK)
                kind[0] = NOTE_WARN;   // one variant usable, the other failed to translate
        }
        if (kind[0] == NOTE_OK || kind[0] == NOTE_WARN) {
            RecompConfig c = RecompConfig.load(GameStore.externalFilesDir(this));
            if (dir[0] != null && !dir[0].isEmpty()) {
                c.set(RecompConfig.LSFG_DIR_KEY, dir[0]);
                c.save();
            }
        }
        Log.i(TAG, "launcher: Lossless.dll import: " + status);
        workerNoteKind = kind[0];
        workerNote = status;
    }

    // ---- GAME_DATA ---------------------------------------------------------------------------------------------

    private void buildGameData() {
        screenTitle.setText(R.string.GAME_DATA);
        State st = state();
        String dir = gameDir();
        chip.set(getString(stateLabel(st)), stateColor(st));
        sizeLater(dir);

        LinearLayout h = rowLayout();
        LinearLayout menu = column();
        menu.addView(Theme.caps(this, getString(R.string.GAME_DATA_CAPS), Theme.TEXT_FAINT), lp(MATCH, WRAP));
        boolean lib = libraryOn();
        MenuRow change = lib ? row(Icon.SWAP, getString(R.string.LIB_TITLE), getString(R.string.LIB_GO_SUB),
                                   this::openLibrary)
                             : row(Icon.SWAP, getString(dir == null ? R.string.CHOOSE_GAME : R.string.CHANGE_GAME),
                                   getString(R.string.CHANGE_GAME_SUB), () -> go(Screen.SOURCE));
        LinearLayout.LayoutParams first = lp(MATCH, Theme.ROW_H);
        first.topMargin = 22;
        menu.addView(change, first);
        onFocus(change, () -> dataChangeDetail(dir));
        defaultFocus = change;

        if (dir != null && ownedCopy(dir)) {
            MenuRow del = row(Icon.TRASH, getString(R.string.DELETE_COPY), getString(R.string.DELETE_COPY_SUB), null);
            del.setOnClickListener(v -> onDelete(del, dir));
            menu.addView(del, lp(MATCH, Theme.ROW_H));
            onFocus(del, () -> dataDeleteDetail(dir));
        }
        // The game's other stored things (pack, Prepare files, caches, saves), each deleted on its own (Storage).
        String tid = titleId();
        Library.Record r = rec();
        String key = tid != null ? tid : r != null ? r.titleId : null;
        if (key != null) {
            List<Storage.Item> items = new ArrayList<>();
            for (Storage.Item it : Storage.items(storageDirs(), key, r)) {
                if (it.kind != Storage.Kind.DATA)
                    items.add(it);
            }
            for (Storage.Item it : items)
                menu.addView(storageRow(it, gameTitle(dir)), lp(MATCH, Theme.ROW_H_SMALL));
            storageMeasureLater(items);
        }
        LinearLayout.LayoutParams ml = lp(860, MATCH);
        ml.rightMargin = Theme.GAP;
        h.addView(menu, ml);
        h.addView(detailPanel(), lp(0, MATCH, 1));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        dataChangeDetail(dir);
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private void dataFacts(String dir) {
        if (dir == null) {
            detailKv(getString(R.string.KV_STATUS), getString(stateLabel(state())));
            return;
        }
        detailKv(getString(R.string.KV_GAME), gameTitle(dir));
        detailKv(getString(R.string.KV_FOLDER), dir);
        detailKv(getString(R.string.KV_SOURCE), sourceLabel(dir));
        detailKv(getString(R.string.KV_SIZE), sizeText(dir));
        detailKv(getString(R.string.KV_FREE), size(freeSpace()));
        if (ownedCopy(dir) && sizeBytes >= 0 && dir.equals(sizeDir)) {
            Widgets.Meter m = new Widgets.Meter(this);
            long free = freeSpace();
            m.set(sizeBytes / (float)Math.max(1, sizeBytes + free), Theme.ACCENT);
            LinearLayout.LayoutParams p = lp(MATCH, 10);
            p.topMargin = 26;
            detail.addView(m, p);
        }
    }

    private void dataChangeDetail(String dir) {
        detailClear(getString(R.string.GAME_DATA), getString(dir == null ? R.string.CHOOSE_GAME
                                                                          : R.string.CHANGE_GAME),
                    getString(R.string.CHANGE_GAME_DETAIL));
        dataFacts(dir);
    }

    private void dataDeleteDetail(String dir) {
        detailClear(getString(R.string.GAME_DATA), getString(R.string.DELETE_COPY),
                    getString(R.string.DELETE_COPY_DETAIL, dir));
        dataFacts(dir);
    }

    /** Two presses within CONFIRM_MS: the first arms, the second deletes (on a background thread). */
    private void onDelete(MenuRow row, String dir) {
        if (deleting)
            return;
        long now = SystemClock.uptimeMillis();
        if (deleteArmedAt == 0 || now - deleteArmedAt > CONFIRM_MS) {
            deleteArmedAt = now;
            row.setBadge(getString(R.string.DELETE_CONFIRM_BADGE), Theme.WARN);
            setNote(getString(R.string.DELETE_CONFIRM), NOTE_WARN);
            return;
        }
        deleteArmedAt = 0;
        deleting = true;
        row.setBadge(getString(R.string.DELETING), Theme.TEXT_DIM);
        setNote(getString(R.string.DELETING), NOTE_INFO);
        if (dir.equals(launchedDir()))
            endRunningGame();   // only the game that runs from these files
        Log.i(TAG, "launcher: deleting the imported copy " + dir);
        boolean inLibrary = indexGames.containsKey(titleId());
        new Thread(() -> {
            String tid = Pack.xbeTitleId(GameStore.findXbe(new File(dir)));
            GameStore.deleteTree(new File(dir));
            /* Its title image, hero and colours (docs/android/app-shell.md « Game theme ») go only when the title
             * also left the library folder: the library still shows it there (docs « Game library »). */
            if (!inLibrary)
                GameTheme.clear(this, tid);
            if (dir.equals(GameStore.chosenDir(this)))
                GameStore.forget(this);
            runOnUiThread(() -> {
                deleting = false;
                sizeDir = null;
                hashDir = null;
                titleIdDir = null;
                artTid = null;
                for (Library.Record r : games.games.values()) {
                    if (dir.equals(r.dataDir)) {
                        r.dataDir = "";
                        r.dataKind = "";
                    }
                }
                writeGames();
                if (tid != null)
                    libArt.remove(tid);
                history.clear();
                focusMemory.remove(Screen.HOME);
                show(libraryOn() ? (rec() != null ? Screen.HOME : Screen.LIBRARY)
                                 : state() == State.NONE ? Screen.WELCOME : Screen.HOME);
                setNote(getString(R.string.DELETED), NOTE_OK);
            });
        }, "xba-delete").start();
    }

    // ---- PREPARE (the on-device Prepare, docs/android/prepare.md) -----------------------------------------------

    /** The title ID of the current game's default.xbe (its certificate), read once per folder; null if none. */
    private String titleId() {
        String dir = gameDir();
        if (dir == null) {
            // A library game whose files are not here: its record's key (its Prepare, art and theme go by it).
            Library.Record r = rec();
            return r != null && r.dataDir.isEmpty() && !Library.isLocalKey(r.titleId) ? r.titleId : null;
        }
        if (!dir.equals(titleIdDir)) {
            titleIdDir = dir;
            titleIdValue = Pack.xbeTitleId(GameStore.findXbe(new File(dir)));
        }
        return titleIdValue;
    }

    private PrepareProtocol.Status readPrepStatus() {
        String tid = titleId();
        return tid == null ? null : PrepareProtocol.readStatus(PrepareService.statusFile(this, tid));
    }

    /** The ":prepare" process is there (a running status.json from a dead process means interrupted). */
    private boolean prepareProcessAlive() {
        ActivityManager am = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> procs = am == null ? null : am.getRunningAppProcesses();
        if (procs == null)
            return false;
        String name = getPackageName() + ":prepare";
        for (ActivityManager.RunningAppProcessInfo p : procs) {
            if (name.equals(p.processName))
                return true;
        }
        return false;
    }

    private boolean statusAlive(PrepareProtocol.Status s) {
        return s != null && s.running()
               && (System.currentTimeMillis() - s.updatedAt < PREPARE_FRESH_MS || prepareProcessAlive());
    }

    private Prep prep() {
        if (state() != State.READY || !Toolchain.available(this))
            return Prep.OFF;
        String tid = titleId();
        if (tid == null)
            return Prep.OFF;
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(this, tid));
        if (statusAlive(s))
            return Prep.RUNNING;
        if (s != null && s.queued())   // waiting behind another title's Prepare; no :prepare process = cut off
            return prepareProcessAlive() ? Prep.QUEUED : Prep.INTERRUPTED;
        if (Pack.check(getFilesDir(), tid) == Pack.Check.VALID && packMatchesSelected(tid))
            return Prep.PLAYABLE;
        if (s == null)
            return Prep.NEEDED;
        switch (s.state) {
        case PrepareProtocol.FAILED:
            return Prep.FAILED;
        case PrepareProtocol.CANCELLED:
            return Prep.CANCELLED;
        case PrepareProtocol.RUNNING:
            return Prep.INTERRUPTED;
        default:
            return Prep.NEEDED;   // done, but the pack is gone or stale (another shell ABI): prepare again
        }
    }

    /** The pack of tid was built from the selected game's default.xbe (as far as both say: LibraryScreen). */
    private boolean packMatchesSelected(String tid) {
        Library.Record r = rec();
        return r == null || LibraryScreen.packMatches(getFilesDir(), tid, r.xbeSha256);
    }

    /**
     * The shell could not load the pack (EXTRA_BAD_PACK): drop it, so Home offers Prepare again instead of a Play that
     * fails the same way. This check is lighter than the native one (no dlopen), hence passed it.
     */
    private void dropBadPack(String why) {
        String tid = titleId();
        Log.w(TAG, "launcher: pack " + tid + " refused by the shell (" + why + "), dropped");
        if (tid != null) {
            Pack.drop(getFilesDir(), tid);
            droppedPacks.add(tid);
        }
    }

    /** « Open the game directly » does not skip a Prepare that is needed. */
    private boolean prepareBlocksPlay() {
        Prep p = prep();
        return p != Prep.OFF && p != Prep.PLAYABLE;
    }

    /** « about 20–30 min »: from the cores and the RAM; a guess until tools.prepare gives its own eta. */
    private String estimate() {
        int cores = Runtime.getRuntime().availableProcessors();
        long ram = Toolchain.totalRam(this);
        int lo, hi;
        if (ram > 0 && ram < PREPARE_RAM_WARN) {
            lo = 40;
            hi = 60;
        } else if (cores >= 8) {
            lo = 20;
            hi = 30;
        } else {
            lo = 30;
            hi = 45;
        }
        return getString(R.string.PREPARE_ESTIMATE, lo, hi);
    }

    /** The subtitle of Home's big button in a Prepare state (the hero's facts carry the numbers). */
    private String prepSub(Prep p, PrepareProtocol.Status s) {
        switch (p) {
        case RUNNING:
            return getString(R.string.HOME_SUB_PREPARING);
        case FAILED:
            return getString(R.string.HOME_SUB_FAILED);
        case CANCELLED:
        case INTERRUPTED:
            return getString(R.string.HOME_SUB_RESUME);
        default:
            return getString(R.string.HOME_SUB_PREPARE);
        }
    }

    /** « copy », « in place », « recomp.cfg »: the source in a word, for Home's Game data tile. */
    private String sourceShort(String dir) {
        if (ownedCopy(dir))
            return getString(R.string.SOURCE_SHORT_COPY);
        if (fromCfg())
            return "recomp.cfg";
        return getString(R.string.SOURCE_SHORT_IN_PLACE);
    }

    /** « Compile · 42 % · about 12 min left ». */
    private String progressLine(PrepareProtocol.Status s) {
        String line = PrepareService.stepName(this, s.id);
        int pct = s.step > 0 ? Math.round(PrepareProtocol.overall(s.step, s.n, s.pct) * 100) : s.pct;
        line += " · " + String.format(Locale.getDefault(), "%d%%", pct);
        if (s.eta >= 0)
            line += " · " + getString(R.string.PREPARE_ETA, PrepareProtocol.formatEta(s.eta));
        return line;
    }

    private long internalFree() {
        return getFilesDir().getUsableSpace();
    }

    /** Checks the space, then starts PrepareService and shows the Preparing screen. */
    private void startPrepare() {
        // SOURCE: the game's files, or, before an import, the library's cached default.xbe (tools.prepare needs
        // only the XBE).
        String tid = titleId(), dir = gameDir();
        if (dir == null && state() == State.LIBRARY) {
            File x = cachedXbeDir();
            dir = x == null ? null : x.getAbsolutePath();
        }
        if (tid == null || dir == null)
            return;
        long free = internalFree();
        if (free < PREPARE_MIN_FREE) {
            setNote(getString(R.string.PREPARE_NO_SPACE, size(PREPARE_MIN_FREE), size(free)), NOTE_ERROR);
            return;
        }
        if (Build.VERSION.SDK_INT >= 33
            && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
               != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            try {
                requestPermissions(new String[] { android.Manifest.permission.POST_NOTIFICATIONS }, REQ_NOTIFICATIONS);
            } catch (RuntimeException ignored) {
            }
        }
        // A fresh status first, so the Preparing screen never shows the previous run's outcome.
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = tid;
        s.startedAt = s.updatedAt = System.currentTimeMillis();
        s.msg = getString(R.string.PREPARE_STARTING);
        s.log = PrepareService.logFile(this, tid).getAbsolutePath();
        File sf = PrepareService.statusFile(this, tid);
        try {
            PrepareProtocol.writeStatus(sf, s);
            startForegroundService(PrepareService.startIntent(this, dir, tid, gameTitle(gameDir())));
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "launcher: cannot start the Prepare", e);
            s.state = PrepareProtocol.FAILED;
            s.code = "START_FAILED";
            s.msg = String.valueOf(e.getMessage());
            try {
                PrepareProtocol.writeStatus(sf, s);
            } catch (IOException ignored) {
            }
            setNote(getString(R.string.PREPARE_START_FAILED, String.valueOf(e.getMessage())), NOTE_ERROR);
            return;
        }
        Log.i(TAG, "launcher: prepare started for " + tid + " (" + dir + ")");
        cancelSent = false;
        go(Screen.PREPARE);
        long ram = Toolchain.totalRam(this);
        if (ram > 0 && ram < PREPARE_RAM_WARN)
            setNote(getString(R.string.PREPARE_LOW_RAM, size(ram)), NOTE_WARN);
    }

    private static final String[] PREP_IDS = { PrepareProtocol.STEP_INSTALL, "identify", "analyse", "lift",
                                               "compile", "pack" };

    private void buildPrepare() {
        String dir = gameDir();
        screenTitle.setText(R.string.PREPARE_TITLE);
        LinearLayout h = rowLayout();
        FrameLayout left = new FrameLayout(this);
        ring = new Widgets.Ring(this);
        left.addView(ring, new FrameLayout.LayoutParams(620, 620, Gravity.CENTER));
        h.addView(left, lp(760, MATCH));

        FrameLayout p = panel();
        LinearLayout c = column();
        c.addView(Theme.caps(this, getString(R.string.PREPARE_CAPS), Theme.ACCENT), lp(MATCH, WRAP));
        prepHead = Theme.oneLine(Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, gameTitle(dir)));
        if (companion != null)
            companion.preparing(titleId(), gameTitle(dir));
        LinearLayout.LayoutParams hl = lp(MATCH, WRAP);
        hl.topMargin = 18;
        c.addView(prepHead, hl);
        // The time left, prominent; what the player may do meanwhile, under it.
        prepEta = Theme.oneLine(Theme.text(this, Theme.T_HEAD, Theme.MEDIUM, Theme.ACCENT, ""));
        LinearLayout.LayoutParams el = lp(MATCH, WRAP);
        el.topMargin = 14;
        c.addView(prepEta, el);
        prepCaption = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_FAINT, "");
        prepCaption.setMaxLines(2);
        prepCaption.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams capl = lp(MATCH, WRAP);
        capl.topMargin = 10;
        capl.bottomMargin = 16;
        c.addView(prepCaption, capl);

        int[] names = { R.string.STEP_INSTALL, R.string.STEP_IDENTIFY, R.string.STEP_ANALYSE, R.string.STEP_LIFT,
                        R.string.STEP_COMPILE, R.string.STEP_PACK };
        int[] subs = { R.string.STEP_INSTALL_SUB, R.string.STEP_IDENTIFY_SUB, R.string.STEP_ANALYSE_SUB,
                       R.string.STEP_LIFT_SUB, R.string.STEP_COMPILE_SUB, R.string.STEP_PACK_SUB };
        for (int i = 0; i < names.length; i++) {
            LinearLayout r = rowLayout();
            r.setGravity(Gravity.CENTER_VERTICAL);
            TextView mark = Theme.text(this, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT_FAINT, "○");
            mark.setGravity(Gravity.CENTER);
            r.addView(mark, lp(48, WRAP));
            TextView name = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT_DIM,
                                                     getString(names[i])));
            r.addView(name, lp(300, WRAP));
            TextView sub = Theme.oneLine(Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_FAINT,
                                                    getString(subs[i])));
            r.addView(sub, lp(0, WRAP, 1));
            TextView pct = Theme.text(this, Theme.T_SMALL, Theme.MEDIUM, Theme.TEXT_FAINT, "");
            pct.setGravity(Gravity.END);
            r.addView(pct, lp(110, WRAP));
            LinearLayout.LayoutParams rl = lp(MATCH, WRAP);
            rl.topMargin = i == 0 ? 0 : 3;
            c.addView(r, rl);
            prepSteps.add(new TextView[] { mark, name, sub, pct });
        }

        prepMsg = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM, "");
        prepMsg.setMaxLines(1);
        prepMsg.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams ml = lp(MATCH, WRAP);
        ml.topMargin = 16;
        c.addView(prepMsg, ml);
        c.addView(gap(this), lp(MATCH, 0, 1));

        PrepareProtocol.Status s = readPrepStatus();
        boolean running = s == null || statusAlive(s);
        if (running) {
            prepCancelRow = row(Icon.BACK, getString(R.string.CANCEL), getString(R.string.PREPARE_CANCEL_SUB),
                                this::armCancelPrepare);
            c.addView(prepCancelRow, lp(MATCH, Theme.ROW_H));
            defaultFocus = prepCancelRow;
            yAction = this::leavePrepare;
            addHint("Y", R.string.HINT_MENU, this::leavePrepare);
            addHint("B", R.string.CANCEL, this::armCancelPrepare);
            chip.set(getString(R.string.CHIP_PREPARING), Theme.OK);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            boolean cancelled = PrepareProtocol.CANCELLED.equals(s.state) || PrepareProtocol.RUNNING.equals(s.state);
            LinearLayout actions = rowLayout();
            MenuRow retry = row(Icon.RESET, getString(cancelled ? R.string.PREPARE_RESUME : R.string.PREPARE_RETRY),
                                getString(R.string.PREPARE_RETRY_SUB), this::startPrepare);
            LinearLayout.LayoutParams al = lp(0, Theme.ROW_H, 1);
            al.rightMargin = 24;
            actions.addView(retry, al);
            MenuRow share = row(Icon.COPY, getString(R.string.SHARE_LOG), getString(R.string.SHARE_LOG_SUB),
                                this::shareLog);
            actions.addView(share, lp(0, Theme.ROW_H, 1));
            c.addView(actions, lp(MATCH, Theme.ROW_H));
            defaultFocus = retry;
            yAction = this::shareLog;
            addHint("A", R.string.HINT_SELECT, this::clickFocused);
            addHint("Y", R.string.SHARE_LOG, this::shareLog);
            addHint("B", R.string.HINT_BACK, this::back);
            chip.set(getString(cancelled ? R.string.CHIP_NEEDS_PREPARE : R.string.CHIP_PREPARE_FAILED),
                     cancelled ? Theme.WARN : Theme.ERROR);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
        p.addView(c);
        h.addView(p, lp(0, MATCH, 1));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        paintPrepare(s);
    }

    /** Y on the Preparing screen: back to the menu, the Prepare carries on. */
    private void leavePrepare() {
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (history.isEmpty())
            show(Screen.HOME);
        else
            show(history.pop());
    }

    /** Fills the Preparing screen from a status (null: just started). */
    private void paintPrepare(PrepareProtocol.Status s) {
        prepShown = s;
        if (ring == null || prepHead == null)
            return;
        boolean running = s == null || statusAlive(s);
        boolean failed = s != null && !running && PrepareProtocol.FAILED.equals(s.state);
        boolean stopped = s != null && !running && !failed;
        int cur = s == null ? 0 : indexOfStep(s.id);
        float frac = s == null ? -1 : s.step > 0 ? PrepareProtocol.overall(s.step, s.n, s.pct) : -1;
        String big = frac >= 0 ? String.format(Locale.getDefault(), "%d%%", Math.round(frac * 100)) : "";
        String label = s == null ? getString(R.string.PREPARE_STARTING) : PrepareService.stepName(this, s.id);
        String sub = s != null && s.step > 0 ? getString(R.string.PREPARE_STEP_OF, s.step, s.n)
                     : "";
        if (running && s != null && s.pct > 0)
            sub = (sub.isEmpty() ? "" : sub + " · ") + getString(R.string.PREPARE_STEP_PCT, s.pct);
        if (failed) {
            label = getString(R.string.PREPARE_FAILED_HEAD);
            sub = s.code;
        } else if (stopped) {
            label = getString(R.string.PREPARE_CANCELLED_HEAD);
            sub = "";
        }
        ring.setColor(failed ? Theme.ERROR : stopped ? Theme.WARN : Theme.OK);
        ring.set(running ? frac : Math.max(0, frac), big, label, sub);
        for (int i = 0; i < prepSteps.size(); i++) {
            TextView[] v = prepSteps.get(i);
            boolean done = i < cur || (s != null && PrepareProtocol.DONE.equals(s.state));
            boolean now = i == cur && !done;
            int color = now ? (failed ? Theme.ERROR : stopped ? Theme.WARN : Theme.OK)
                        : done ? Theme.TEXT : Theme.TEXT_FAINT;
            v[0].setText(done ? "✓" : now ? (failed ? "✕" : "●") : "○");
            v[0].setTextColor(done ? Theme.OK : color);
            v[1].setTextColor(now ? (failed ? Theme.ERROR : Theme.TEXT) : done ? Theme.TEXT_DIM : Theme.TEXT_FAINT);
            v[1].setTypeface(now ? Theme.MEDIUM : Theme.REGULAR);
            v[2].setTextColor(now ? Theme.TEXT_DIM : Theme.TEXT_FAINT);
            v[3].setText(now && running && s != null && s.pct > 0
                         ? String.format(Locale.getDefault(), "%d%%", s.pct) : "");
            v[3].setTextColor(color);
        }
        if (failed) {
            prepHead.setText(getString(R.string.PREPARE_FAILED_HEAD));
            prepHead.setTextColor(Theme.ERROR);
            String why = s.msg;
            if (PrepareRunner.KILLED.equals(s.code))
                why = getString(R.string.PREPARE_HINT_KILLED);
            else if ("NO_SPACE".equals(s.code))
                why = getString(R.string.PREPARE_HINT_NO_SPACE, size(internalFree()))
                      + (why.isEmpty() ? "" : " " + why);
            prepMsg.setText(getString(R.string.PREPARE_CODE_LINE, s.code) + (why.isEmpty() ? "" : " · " + why));
            prepMsg.setTextColor(Theme.TEXT);
            prepMsg.setMaxLines(2);
            prepEta.setText(getString(R.string.PREPARE_ETA_FAILED));
            prepEta.setTextColor(Theme.ERROR);
            prepCaption.setVisibility(View.GONE);   // Y « Share log » says it, and the error needs the room
        } else if (stopped) {
            prepHead.setText(getString(R.string.PREPARE_CANCELLED_HEAD));
            prepHead.setTextColor(Theme.TEXT);
            prepMsg.setText(getString(PrepareProtocol.RUNNING.equals(s.state) ? R.string.PREPARE_INTERRUPTED
                                                                              : R.string.PREPARE_CANCELLED_TEXT));
            prepEta.setText(getString(R.string.PREPARE_ETA_STOPPED));
            prepEta.setTextColor(Theme.WARN);
            prepCaption.setVisibility(View.GONE);
            prepMsg.setMaxLines(2);
        } else {
            prepMsg.setText(s == null || s.msg.isEmpty() ? getString(R.string.PREPARE_STARTING) : s.msg);
            prepEta.setText(s != null && s.eta >= 0 ? getString(R.string.PREPARE_ETA_BIG,
                                                                PrepareProtocol.formatEta(s.eta))
                                                    : getString(R.string.PREPARE_ETA_UNKNOWN));
            prepEta.setTextColor(Theme.ACCENT);
            prepCaption.setText(getString(R.string.PREPARE_KEEPS));
            prepCaption.setVisibility(View.VISIBLE);
        }
        if (dimmed)
            paintDim();
    }

    private static int indexOfStep(String id) {
        for (int i = 0; i < PREP_IDS.length; i++) {
            if (PREP_IDS[i].equals(id))
                return i;
        }
        return 0;
    }

    /** Twice within CONFIRM_MS: the first press arms, the second sends ACTION_CANCEL to the service. */
    private void armCancelPrepare() {
        if (cancelSent)
            return;
        long now = SystemClock.uptimeMillis();
        if (cancelArmedAt == 0 || now - cancelArmedAt > CONFIRM_MS) {
            cancelArmedAt = now;
            if (prepCancelRow != null)
                prepCancelRow.setBadge(getString(R.string.PREPARE_CANCEL_BADGE), Theme.WARN);
            setNote(getString(R.string.PREPARE_CANCEL_CONFIRM), NOTE_WARN);
            return;
        }
        cancelArmedAt = 0;
        cancelSent = true;
        String tid = titleId();
        if (prepCancelRow != null) {
            prepCancelRow.setBadge(getString(R.string.CANCELING), Theme.TEXT_DIM);
            prepCancelRow.setDim(true);
        }
        setNote(getString(R.string.CANCELING), NOTE_INFO);
        Log.i(TAG, "launcher: prepare cancel");
        try {
            startService(PrepareService.cancelIntent(this, tid));
        } catch (RuntimeException e) {
            Log.w(TAG, "launcher: cannot reach the Prepare service", e);
        }
    }

    /** « Share log »: prepare.log through LogProvider, to whatever app the player picks. */
    private void shareLog() {
        String tid = titleId();
        if (tid == null || !PrepareService.logFile(this, tid).isFile()) {
            setNote(getString(R.string.SHARE_LOG_NONE), NOTE_INFO);
            return;
        }
        Uri uri = LogProvider.uriFor(this, tid);
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_STREAM, uri)
                .putExtra(Intent.EXTRA_SUBJECT, getString(R.string.SHARE_LOG_SUBJECT, gameTitle(gameDir()), tid))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        send.setClipData(android.content.ClipData.newRawUri("prepare.log", uri));
        try {
            startActivity(Intent.createChooser(send, getString(R.string.SHARE_LOG)));
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.SHARE_LOG_NO_APP), NOTE_ERROR);
        }
    }

    /** Every PREPARE_POLL_MS while resumed: status.json into the Preparing screen, or into Home's row. */
    private void pollPrepare() {
        if (!resumed)
            return;
        poller.postDelayed(pollTick, PREPARE_POLL_MS);
        if (screen == Screen.PREPARE) {
            PrepareProtocol.Status s = readPrepStatus();
            boolean wasRunning = prepShown == null || statusAlive(prepShown);
            boolean isRunning = s == null || statusAlive(s);
            if (s != null && PrepareProtocol.DONE.equals(s.state)) {
                onPrepareDone();
                return;
            }
            if (wasRunning != isRunning) {
                cancelSent = false;
                show(Screen.PREPARE);   // running -> failed / cancelled: the actions change
                return;
            }
            paintPrepare(s);
            checkDim(isRunning);
        } else if (screen == Screen.HOME && homeChip != null) {
            LibraryScreen.Entry e = ++libPolls % LIBRARY_POLL_EVERY == 0 ? currentEntry() : null;
            if (e != null && e.chip != homeChip)
                refreshKeepingFocus();
        } else if (screen == Screen.LIBRARY && !scanning()) {
            if (++libPolls % LIBRARY_POLL_EVERY == 0 && libraryChanged())
                refreshKeepingFocus();
        } else if (screen == Screen.HOME && homePrep != null) {
            Prep now = prep();
            if (now != homePrep) {
                refreshKeepingFocus();
                if (now == Prep.PLAYABLE)
                    setNote(getString(R.string.READY_TO_PLAY, gameTitle(gameDir())), NOTE_OK);
                return;
            }
            if (now == Prep.RUNNING) {
                PrepareProtocol.Status s = readPrepStatus();
                if (s != null && homeMeter != null && homeProgress != null) {
                    homeMeter.set(s.step > 0 ? PrepareProtocol.overall(s.step, s.n, s.pct) : 0, Theme.ACCENT);
                    homeProgress.setText(progressLine(s));
                }
            }
        }
    }

    /** The Prepare finished: Home with Play focused, the « done » sound and haptic, « TITLE is ready ». */
    private void onPrepareDone() {
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        String tid = titleId();
        boolean ok = tid != null && Pack.check(getFilesDir(), tid) == Pack.Check.VALID;
        Log.i(TAG, "launcher: prepare done, pack " + (ok ? "valid" : "not valid"));
        history.clear();
        focusMemory.remove(Screen.HOME);
        show(Screen.HOME);
        if (ok) {
            restoreFocus(true);   // Play, even after a touch: the next press of A plays
            feedback.play(UiFeedback.Cue.DONE, host);
            setNote(getString(state() == State.LIBRARY ? R.string.LIB_PREPARED_IMPORT : R.string.READY_TO_PLAY,
                              gameTitle(gameDir())), NOTE_OK);
        }
    }

    // ---- LIBRARY (docs/android/app-shell.md « Game library ») ------------------------------------------------

    /** The game library is on: a library folder is set, or the launcher knows two games or more. */
    private boolean libraryOn() {
        return Library.folderTree(this) != null || games.games.size() >= 2;
    }

    private void setIndex(LibraryIndex x) {
        index = x;
        indexGames = x.games();
    }

    private void writeGames() {
        try {
            Library.write(Library.gamesFile(getFilesDir()), games);
        } catch (IOException e) {
            Log.w(TAG, "launcher: library: cannot write games.json", e);
        }
    }

    /** The library folder can be read: All files access for a path, the persisted permission for a tree. */
    private boolean folderAccessible() {
        String tree = Library.folderTree(this);
        if (tree == null)
            return true;
        if (Library.folderPath(this) != null)
            return Environment.isExternalStorageManager();
        for (android.content.UriPermission p : getContentResolver().getPersistedUriPermissions()) {
            if (p.isReadPermission() && p.getUri().toString().equals(tree))
                return true;
        }
        return false;
    }

    private LibraryScreen.Env libEnv() {
        LibraryScreen.Env e = new LibraryScreen.Env();
        e.toolchain = Toolchain.available(this);
        e.accessible = folderAccessible();
        e.prepareProcess = prepareProcessAlive();
        e.runningTid = gameRunning() ? launchedTid() : null;
        e.importingTid = worker != null && worker.isAlive() ? importTid : null;
        e.queue = PrepareProtocol.readQueue(PrepareProtocol.queueFile(getFilesDir()));
        e.packDropped = droppedPacks;
        return e;
    }

    /** The selected game as a library entry (its chip), or null. */
    private LibraryScreen.Entry currentEntry() {
        Library.Record r = games.selected();
        return r == null ? null : LibraryScreen.entry(getFilesDir(), r.titleId, games, index, libEnv());
    }

    /** The library's focused card's entry, else the selected game's. */
    private LibraryScreen.Entry focusedEntry() {
        if (screen == Screen.LIBRARY) {
            for (LibraryScreen.Entry e : libEntries) {
                if (e.tid.equals(focusTid))
                    return e;
            }
            return null;
        }
        return currentEntry();
    }

    /** The folder of the selected game's cached default.xbe (the scan's copy), or null. */
    private File cachedXbeDir() {
        Library.Record r = games.selected();
        if (r == null)
            return null;
        LibraryIndex.Item it = LibraryIndex.active(indexGames.get(r.titleId), r.source);
        File x = it == null ? null : it.xbeFile(getFilesDir());
        return x == null || !x.isFile() ? null : x.getParentFile();
    }

    /** Opens the library (« Library » from Home, Game data or the bottom screen). */
    private void openLibrary() {
        history.clear();
        show(Screen.LIBRARY, -1);
    }

    /** e becomes the selected game (a record is made for a game seen in the folder only). */
    private void selectEntry(LibraryScreen.Entry e) {
        boolean isNew = games.get(e.tid) == null;
        Library.Record r = LibraryScreen.recordFor(e, System.currentTimeMillis());
        if (isNew)
            games.put(r);
        if (isNew || !e.tid.equals(games.selected)) {
            games.selected = e.tid;
            writeGames();
            switchArmedAt = 0;
            focusMemory.remove(Screen.HOME);
        }
        titleIdDir = null;
        artTid = null;
    }

    /** A on a card: the game's Home, in its colours. */
    private void openGame(LibraryScreen.Entry e) {
        selectEntry(e);
        Log.i(TAG, "launcher: library: open " + e.tid + " (" + e.title() + ", " + e.chip + ")");
        go(Screen.HOME);
    }

    /** Start on a card (or the bottom screen's big button): the game's primary action, without opening Home. */
    private void libraryPrimary(LibraryScreen.Entry e) {
        selectEntry(e);
        onPlay();
    }

    /** The primary action of a library game whose files are not here (State.LIBRARY). */
    private void libraryAction(LibraryScreen.Entry e) {
        if (e == null)
            return;
        switch (e.chip) {
        case PREPARING:
            go(Screen.PREPARE);
            break;
        case QUEUED:
            dequeuePrepare(e.tid);
            break;
        case NOT_IMPORTED:
        case NEEDS_COPYING:
            importEntry(e);
            break;
        case NO_FILE_ACCESS:
            onChooseLibraryFolder();
            break;
        default:
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            break;
        }
    }

    /** The primary action's label for e (the bottom screen's big button); null when there is none. */
    private String primaryLabel(LibraryScreen.Entry e) {
        switch (e.chip) {
        case READY:
        case RUNNING:
            return getString(R.string.PLAY);
        case NEEDS_PREPARING:
        case PACK_INVALID:
            return getString(R.string.PREPARE);
        case PREPARE_FAILED:
            return getString(R.string.PREPARE_SEE_WHY);
        case PREPARING:
            return getString(R.string.PREPARING);
        case QUEUED:
            return LibraryScreen.chipText(this, e);
        case NOT_IMPORTED:
            return getString(R.string.LIB_IMPORT);
        case NEEDS_COPYING:
            return getString(R.string.LIB_COPY);
        case NO_FILE_ACCESS:
            return getString(R.string.LIB_CHOOSE_FOLDER);
        default:
            return null;
        }
    }

    /** « Queued (2) »: the place of tid in the Prepare queue. */
    private String queuedLabel(String tid) {
        int pos = tid == null ? 0 : PrepareProtocol.readQueue(PrepareProtocol.queueFile(getFilesDir())).position(tid);
        return pos > 0 ? getString(R.string.LIB_QUEUED_N, pos) : getString(R.string.CHIP_QUEUED);
    }

    /** A on « Queued (n) »: the title leaves the Prepare queue (PrepareService marks it cancelled). */
    private void dequeuePrepare(String tid) {
        if (tid == null)
            return;
        try {
            startService(PrepareService.cancelIntent(this, tid));
        } catch (RuntimeException e) {
            Log.w(TAG, "launcher: cannot reach the Prepare service", e);
        }
        Log.i(TAG, "launcher: prepare " + tid + " taken out of the queue");
        setNote(getString(R.string.LIB_DEQUEUED), NOTE_INFO);
        Screen at = screen;
        poller.postDelayed(() -> {
            if (!isFinishing() && screen == at && (at == Screen.HOME || at == Screen.LIBRARY))
                refreshKeepingFocus();
        }, 600);
    }

    /**
     * Import a library game: its disc image extracted (a file, or a document of the library folder), or its folder
     * copied (seen through the Storage Access Framework only), into games/<TID>; a needed Prepare follows.
     */
    private void importEntry(LibraryScreen.Entry e) {
        LibraryIndex.Item it = e.active;
        String tree = Library.folderTree(this);
        if (it == null) {
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            return;
        }
        File f = LibraryIndex.locFile(it.loc);
        String docId = it.loc.startsWith("doc:") ? it.loc.substring(4) : null;
        if (f == null && (docId == null || tree == null)) {
            setNote(getString(R.string.LIB_SOURCE_GONE_NOTE, e.title()), NOTE_WARN);
            return;
        }
        Log.i(TAG, "launcher: library: import " + e.tid + " from " + it.loc);
        importTid = e.tid;
        importFromLibrary = true;
        if (LibraryIndex.KIND_FOLDER.equals(it.kind)) {
            if (docId == null) {   // a folder read as files is played in place: nothing to copy
                importFromLibrary = false;
                return;
            }
            Uri t = Uri.parse(tree);
            runWorker(R.string.COPYING, () -> copyTree(t, docId, false));
            return;
        }
        String name = it.name;
        runWorker(R.string.EXTRACTING, () -> {
            ParcelFileDescriptor pfd = f != null
                    ? ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
                    : getContentResolver().openFileDescriptor(
                              DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree), docId), "r");
            if (pfd == null)
                throw new IOException("cannot open " + name);
            extractIso(pfd, name);
        });
    }

    /** The library folder was picked: remembered (path with All files access, else the tree), then scanned. */
    private void onLibraryTreePicked(Uri tree) {
        try {
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
        String path = Environment.isExternalStorageManager() ? GameStore.treeUriToPath(tree) : null;
        if (path != null && !new File(path).isDirectory())
            path = null;
        String before = Library.folderTree(this);
        Library.setFolder(this, tree.toString(), path);
        Log.i(TAG, "launcher: library: folder " + tree + (path != null ? " = " + path : " (Storage Access Framework)"));
        pickLibrary = false;
        if (!tree.toString().equals(before))
            setIndex(new LibraryIndex());   // another folder: its own items (the scan still reuses what it can)
        focusTid = null;
        history.clear();
        show(Screen.LIBRARY);   // a path is scanned as the library opens
        if (path == null)
            startScan();
    }

    /** Y: scan the library folder again (or choose one). */
    private void rescanLibrary() {
        if (Library.folderTree(this) == null) {
            onChooseLibraryFolder();
            return;
        }
        startScan();
        setNote(getString(R.string.LIB_SCANNING_NOTE), NOTE_INFO);
    }

    /** The scan on a worker thread; the screen is built again when it ends. */
    private void startScan() {
        if (scanner != null && scanner.isAlive())
            return;
        LibraryScanner.Tree t = LibraryScanner.folderTree(this);
        if (t == null)
            return;
        scanCancel = false;
        scanFound = 0;
        android.content.Context app = getApplicationContext();
        LibraryIndex before = index;
        scanner = new Thread(() -> {
            LibraryScanner.Result r = null;
            try {
                r = LibraryScanner.scanLibrary(app, t, item -> {
                    scanFound++;
                    postScanProgress();
                }, () -> scanCancel);
            } catch (RuntimeException | LinkageError e) {
                Log.w(TAG, "launcher: library: scan failed", e);
            }
            LibraryScanner.Result res = r;
            runOnUiThread(() -> onScanDone(res, before));
        }, "xba-scan");
        scanner.start();
        paintScanCaption();
    }

    Thread scannerForTest() {
        return scanner;
    }

    private volatile long scanPaintedAt;

    private void postScanProgress() {
        long now = SystemClock.uptimeMillis();
        if (now - scanPaintedAt < 250)
            return;
        scanPaintedAt = now;
        runOnUiThread(this::paintScanCaption);
    }

    private boolean scanning() {
        return scanner != null && scanner.isAlive();
    }

    private void onScanDone(LibraryScanner.Result r, LibraryIndex before) {
        if (isFinishing() || isDestroyed())
            return;
        if (r != null && !r.cancelled) {
            setIndex(r.index);
            // A title that left the folder and has no files here: its cached art and theme go (docs « Game
            // library »). Not when the folder could not be read at all (root unknown): it may only be unmounted.
            if (!r.index.root.isEmpty()) {
                for (String tid : before.games().keySet()) {
                    Library.Record rr = games.get(tid);
                    if (!indexGames.containsKey(tid) && (rr == null || !rr.hasData())) {
                        GameTheme.clear(this, tid);
                        libArt.remove(tid);
                    }
                }
            }
            libArt.values().removeIf(java.util.Objects::isNull);   // art the scan made shows now
            if (libEntries.isEmpty())
                focusTid = null;   // the first games found: the focus goes to the selected one, not the folder tile
        }
        if (screen == Screen.LIBRARY)
            refreshKeepingFocus();
        else if (screen == Screen.HOME && state() == State.LIBRARY)
            refreshKeepingFocus();
    }

    /** Under the grid: the scan's progress, an empty library's hint, the « Other files » line. */
    private void paintScanCaption() {
        if (libCaption == null)
            return;
        List<String> parts = new ArrayList<>();
        if (scanning())
            parts.add(getString(R.string.LIB_SCANNING, scanFound));
        else if (Library.folderTree(this) == null)
            parts.add(getString(R.string.LIB_NO_FOLDER));
        else if (libEntries.isEmpty())
            parts.add(getString(R.string.LIB_EMPTY));
        List<LibraryIndex.Item> others = index.others();
        if (!others.isEmpty()) {
            List<String> names = new ArrayList<>();
            for (int i = 0; i < others.size() && i < 3; i++)
                names.add(others.get(i).name);
            parts.add(getString(R.string.LIB_OTHERS, others.size(), TextUtils.join(", ", names)
                                                                   + (others.size() > 3 ? ", …" : "")));
        }
        libCaption.setText(TextUtils.join("\n", parts));
    }

    /**
     * The library: the grid of games (left), the focused game's details (right), the « Library folder » tile last.
     * Stock colours; the backdrop's glow follows the focused card (its cached theme).
     */
    private void buildLibrary() {
        screenTitle.setText(R.string.LIB_TITLE);
        libEntries = LibraryScreen.entries(getFilesDir(), games, index, libEnv());
        int n = libEntries.size();
        chip.set(getResources().getQuantityString(R.plurals.LIB_GAMES, n, n), Theme.OK);
        Library.Record sel = games.selected();
        String want = focusTid != null ? focusTid : sel != null ? sel.titleId : null;
        GameCard focusCard = null;
        for (LibraryScreen.Entry e : libEntries) {
            GameCard c = new GameCard(this);
            c.setGame(LibraryScreen.art(getFilesDir(), libArt, e), e.tid, e.title(), LibraryScreen.chipText(this, e),
                      LibraryScreen.chipColor(e.chip));
            c.setDim(e.chip == GameState.Chip.SOURCE_GONE || e.chip == GameState.Chip.DATA_MISSING
                     || e.chip == GameState.Chip.NO_FILE_ACCESS);
            c.setOnClickListener(v -> {
                feedback.play(UiFeedback.Cue.SELECT, v);
                openGame(e);
            });
            onFocus(c, () -> libFocus(e));
            libCards.add(focusable(c));
            if (e.tid.equals(want))
                focusCard = c;
        }
        GameCard folder = new GameCard(this);
        String path = Library.folderPath(this), tree = Library.folderTree(this);
        folder.setTile(Icon.FOLDER, getString(R.string.LIB_FOLDER_TILE),
                       tree == null ? getString(R.string.LIB_FOLDER_NONE)
                       : path != null ? new File(path).getName() : getString(R.string.LIB_FOLDER_SAF));
        folder.setOnClickListener(v -> {
            feedback.play(UiFeedback.Cue.SELECT, v);
            onChooseLibraryFolder();
        });
        onFocus(folder, this::libFolderFocus);
        libCards.add(focusable(folder));
        if (focusCard == null && (libEntries.isEmpty() || "".equals(focusTid)))
            focusCard = folder;
        if (focusCard == null)
            focusCard = libCards.get(0);

        libCaption = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_FAINT, "");
        libCaption.setPadding(28, 0, 0, 0);
        libCaption.setLineSpacing(0, 1.2f);
        paintScanCaption();
        LinearLayout h = rowLayout();
        LinearLayout.LayoutParams gl = lp(0, MATCH, 1);
        gl.rightMargin = Theme.GAP;
        h.addView(LibraryScreen.grid(this, libCards, libCaption), gl);
        h.addView(detailPanel(), lp(600, MATCH));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));

        focusMemory.remove(Screen.LIBRARY);   // the focus follows the title, not a place in the grid
        defaultFocus = focusCard;
        if (focusCard == folder) {
            libFolderFocus();
        } else {
            for (LibraryScreen.Entry e : libEntries) {
                if (libCards.get(libEntries.indexOf(e)) == focusCard) {
                    libFocus(e);
                    break;
                }
            }
        }
        startAction = () -> {
            LibraryScreen.Entry e = focusedEntry();
            if (e != null)
                libraryPrimary(e);
            else
                onChooseLibraryFolder();
        };
        yAction = this::rescanLibrary;
        xAction = () -> go(Screen.STORAGE);
        addHint("A", R.string.LIB_HINT_OPEN, this::clickFocused);
        addHint("START", R.string.LIB_HINT_PRIMARY, startAction);
        addHint("Y", R.string.LIB_RESCAN, this::rescanLibrary);
        addHint("X", R.string.LIB_STORAGE, () -> go(Screen.STORAGE));
        addHint("B", R.string.HINT_QUIT, this::back);
        if (gameRunning())
            setNote(getString(R.string.LIB_RUNNING_NOTE, runningTitle()), NOTE_INFO);
    }

    /** A card got the focus: the details, the backdrop's glow, the bottom screen. */
    private void libFocus(LibraryScreen.Entry e) {
        focusTid = e.tid;
        int[] g = LibraryScreen.glows(this, e);
        if (backdrop != null)
            backdrop.setGlows(g[0], g[1]);
        libDetail(e);
        if (companion != null)
            companion.library(e.title(), LibraryScreen.chipText(this, e), LibraryScreen.chipColor(e.chip),
                              LibraryScreen.art(getFilesDir(), libArt, e), primaryLabel(e));
    }

    private void libDetail(LibraryScreen.Entry e) {
        detailClear(e.realTid() ? getString(R.string.HOME_TITLE_ID, e.tid) : getString(R.string.LIB_TITLE),
                    e.title(), libLead(e));
        detailKv(getString(R.string.KV_STATUS), LibraryScreen.chipText(this, e));
        LibraryIndex.Item it = e.active;
        if (it != null) {
            String src = it.name + (e.variants.size() > 1 ? " · " + getString(R.string.LIB_VERSIONS, e.variants.size())
                                                         : "");
            detailKv(getString(R.string.KV_SOURCE), src);
            String region = LibraryScreen.regionText(this, it.region);
            if (!region.isEmpty())
                detailKv(getString(R.string.LIB_KV_REGION), region + " · " + LibraryScreen.versionText(it.version)
                                                            + (it.disc > 1 ? " · " + getString(R.string.LIB_DISC, it.disc)
                                                                           : ""));
            if (LibraryIndex.KIND_ISO.equals(it.kind))
                detailKv(getString(R.string.LIB_KV_IMAGE), size(it.size));
        }
        if (e.record != null && !e.record.dataDir.isEmpty())
            detailKv(getString(R.string.KV_FOLDER), e.record.dataDir);
        long now = System.currentTimeMillis();
        if (e.record != null) {
            detailKv(getString(R.string.KV_LAST_PLAYED), lastPlayedText(e.record.lastPlayed, now));
            detailKv(getString(R.string.KV_PLAY_TIME), playTimeText(e.record.playMs));
        }
        if (e.realTid() && Pack.check(getFilesDir(), e.tid) == Pack.Check.VALID)
            detailKv(getString(R.string.KV_PREPARED), packFacts(e.tid)[0]);
        String action = primaryLabel(e);
        detailFooter(action == null ? getString(R.string.LIB_FOOTER_OPEN)
                                    : getString(R.string.LIB_FOOTER, action), Theme.TEXT_FAINT);
    }

    /** One line on what a library game's state means for the player. */
    private String libLead(LibraryScreen.Entry e) {
        switch (e.chip) {
        case RUNNING:
            return getString(R.string.PLAY_DETAIL_RUNNING);
        case READY:
            return getString(R.string.HOME_LEAD_READY);
        case NEEDS_PREPARING:
            return getString(R.string.HOME_LEAD_PREPARE);
        case PREPARING:
            return getString(R.string.HOME_LEAD_PREPARING);
        case PREPARE_FAILED:
            return getString(R.string.HOME_LEAD_FAILED);
        case PACK_INVALID:
            return getString(R.string.LIB_LEAD_PACK_INVALID);
        case QUEUED:
            return getString(R.string.PREPARE_QUEUED_NOTE);
        case IMPORTING:
            return getString(R.string.LIB_LEAD_IMPORTING);
        case NOT_IMPORTED:
            return getString(R.string.LIB_LEAD_NOT_IMPORTED);
        case NEEDS_COPYING:
            return getString(R.string.LIB_LEAD_NEEDS_COPYING);
        case NO_FILE_ACCESS:
            return getString(R.string.LIB_LEAD_NO_ACCESS);
        case SOURCE_GONE:
            return getString(R.string.LIB_LEAD_SOURCE_GONE);
        default:
            return getString(R.string.LIB_LEAD_DATA_MISSING);
        }
    }

    /** The « Library folder » tile got the focus. */
    private void libFolderFocus() {
        focusTid = "";
        if (backdrop != null)
            backdrop.setGlows(Theme.GLOW, Theme.GLOW2);
        String tree = Library.folderTree(this), path = Library.folderPath(this);
        detailClear(getString(R.string.LIB_TITLE), getString(R.string.LIB_FOLDER_TILE),
                    getString(R.string.LIB_FOLDER_DETAIL));
        detailKv(getString(R.string.KV_FOLDER), tree == null ? getString(R.string.LIB_FOLDER_NONE)
                                                             : path != null ? path : Uri.parse(tree).getPath());
        long at = Library.scannedAt(this);
        if (tree != null)
            detailKv(getString(R.string.LIB_KV_SCANNED), at <= 0 ? getString(R.string.VAL_NEVER)
                                                                 : lastPlayedText(at, System.currentTimeMillis()));
        detailKv(getString(R.string.LIB_KV_GAMES), String.valueOf(libEntries.size()));
        int others = index.others().size();
        if (others > 0)
            detailKv(getString(R.string.LIB_KV_OTHERS), String.valueOf(others));
        if (index.truncated)
            detailKv(getString(R.string.KV_STATUS), getString(R.string.LIB_TRUNCATED));
        detailFooter(getString(R.string.LIB_FOLDER_FOOTER), Theme.TEXT_FAINT);
        if (companion != null)
            companion.library("", tree == null ? getString(R.string.LIB_FOLDER_NONE)
                                               : getString(R.string.LIB_FOLDER_TILE), Theme.TEXT_DIM, null, null);
    }

    /**
     * Home of a library game whose files are not here: its art (the scan's cached image), title and state, the
     * primary action (Import, Copy, Preparing, Queued (n), Choose the folder again) and, before an import, « Prepare
     * now » from the cached default.xbe.
     */
    private void buildHomeNotHere() {
        LibraryScreen.Entry e = currentEntry();
        screenTitle.setText(R.string.HOME_TITLE);
        homeChip = e.chip;
        String title = e.title();
        String chipText = LibraryScreen.chipText(this, e);
        int chipColor = LibraryScreen.chipColor(e.chip);

        LinearLayout col = column();
        LinearLayout hero = rowLayout();
        hero.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.ArtTile tile = new Widgets.ArtTile(this);
        if (art != null)
            tile.setArt(art, artColor);
        else
            tile.setMonogram(TitleArt.initials(title), artColor);
        hero.addView(tile, lp(Theme.ART, Theme.ART));

        LinearLayout info = column();
        if (e.realTid())
            info.addView(Theme.caps(this, getString(R.string.HOME_TITLE_ID, e.tid), Theme.TEXT_FAINT), lp(MATCH, WRAP));
        TextView big = Theme.text(this, Theme.T_HERO, Theme.MEDIUM, Theme.TEXT, title);
        big.setMaxLines(2);
        big.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams bl = lp(MATCH, WRAP);
        bl.topMargin = 14;
        info.addView(big, bl);
        Widgets.Chip status = new Widgets.Chip(this);
        status.set(chipText, chipColor);
        LinearLayout.LayoutParams cl = lp(WRAP, WRAP);
        cl.topMargin = 22;
        info.addView(status, cl);
        TextView leadV = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, libLead(e));
        leadV.setMaxLines(2);
        leadV.setEllipsize(TextUtils.TruncateAt.END);
        leadV.setLineSpacing(0, 1.2f);
        LinearLayout.LayoutParams ll = lp(MATCH, WRAP);
        ll.topMargin = 22;
        info.addView(leadV, ll);

        LibraryIndex.Item it = e.active;
        LinearLayout facts = rowLayout();
        if (it != null) {
            homeFact(facts, getString(R.string.KV_SOURCE), it.name, Theme.TEXT);
            if (LibraryIndex.KIND_ISO.equals(it.kind))
                homeFact(facts, getString(R.string.LIB_KV_IMAGE), size(it.size), Theme.TEXT);
        }
        homeFact(facts, getString(R.string.KV_FREE), size(freeSpace()), Theme.TEXT);
        GameState.Facts f = e.facts;
        Prep p = GameState.prep(f.toolchain, f.pack == Pack.Check.VALID && f.packMatches, f.prepare, f.prepareFresh,
                                f.prepareProcess);
        if (p == Prep.PLAYABLE)
            homeFact(facts, getString(R.string.KV_PREPARED), packFacts(e.tid)[0], Theme.OK);
        LinearLayout.LayoutParams fl = lp(MATCH, WRAP);
        fl.topMargin = 26;
        info.addView(facts, fl);

        MenuRow play;
        switch (e.chip) {
        case NOT_IMPORTED:
            play = row(Icon.COPY, getString(R.string.LIB_IMPORT),
                       getString(it != null && LibraryIndex.KIND_ISO.equals(it.kind) ? R.string.LIB_IMPORT_SUB
                                                                                    : R.string.LIB_COPY_SUB),
                       this::onPlay);
            break;
        case NEEDS_COPYING:
            play = row(Icon.COPY, getString(R.string.LIB_COPY), getString(R.string.LIB_COPY_SUB), this::onPlay);
            break;
        case NO_FILE_ACCESS:
            play = row(Icon.KEY, getString(R.string.LIB_CHOOSE_FOLDER), getString(R.string.LIB_CHOOSE_FOLDER_SUB),
                       this::onPlay);
            break;
        case PREPARING:
            play = row(Icon.ARROW, getString(R.string.PREPARING), getString(R.string.HOME_SUB_PREPARING),
                       this::onPlay);
            break;
        case QUEUED:
            play = row(Icon.ARROW, queuedLabel(e.tid), getString(R.string.LIB_QUEUED_SUB), this::onPlay);
            break;
        default:
            play = row(Icon.FOLDER, getString(R.string.LIB_FOLDER_TILE), getString(R.string.LIB_SOURCE_GONE_SUB),
                       this::onChooseLibraryFolder);
            break;
        }
        play.setPrimary(true);
        homePrimary = play;
        LinearLayout actions = rowLayout();
        LinearLayout.LayoutParams pl = lp(Theme.PRIMARY_W, Theme.PRIMARY_H);
        pl.leftMargin = -12;
        actions.addView(play, pl);
        // « Prepare now »: tools.prepare needs only default.xbe, so a game can be prepared (or queued) before its
        // gigabytes are imported.
        boolean canPrepare = f.toolchain && e.realTid() && cachedXbeDir() != null
                             && (e.chip == GameState.Chip.NOT_IMPORTED || e.chip == GameState.Chip.NEEDS_COPYING);
        if (canPrepare && (p == Prep.NEEDED || p == Prep.CANCELLED || p == Prep.INTERRUPTED || p == Prep.FAILED)) {
            boolean failed = p == Prep.FAILED;
            MenuRow now = row(failed ? Icon.INFO : Icon.ARROW,
                              getString(failed ? R.string.PREPARE_SEE_WHY : R.string.LIB_PREPARE_NOW),
                              getString(failed ? R.string.HOME_SUB_FAILED : R.string.LIB_PREPARE_NOW_SUB),
                              failed ? () -> go(Screen.PREPARE) : this::startPrepare);
            LinearLayout.LayoutParams nl = lp(0, Theme.PRIMARY_H, 1);
            nl.leftMargin = 12;
            actions.addView(now, nl);
        }
        LinearLayout.LayoutParams al = lp(MATCH, Theme.PRIMARY_H);
        al.topMargin = 30;
        info.addView(actions, al);

        LinearLayout.LayoutParams il = lp(0, WRAP, 1);
        il.leftMargin = 52;
        hero.addView(info, il);
        col.addView(hero, lp(MATCH, 0, 1));

        LinearLayout tiles = rowLayout();
        MenuRow settings = homeTile(tiles, Icon.SLIDERS, R.string.SETTINGS, settingsSummary(), Screen.SETTINGS);
        homeTile(tiles, Icon.PAD, R.string.CONTROLS, getString(R.string.CONTROLS_SUB), Screen.CONTROLS);
        homeTile(tiles, Icon.DISC, R.string.GAME_DATA, chipText, Screen.GAME_DATA);
        MenuRow about = homeTile(tiles, Icon.INFO, R.string.ABOUT, getString(R.string.ABOUT_SUB, version()),
                                 Screen.ABOUT);
        ((LinearLayout.LayoutParams)settings.getLayoutParams()).leftMargin = -12;
        ((LinearLayout.LayoutParams)about.getLayoutParams()).rightMargin = -12;
        LinearLayout.LayoutParams tl = lp(MATCH, Theme.TILE_H);
        tl.topMargin = 28;
        col.addView(tiles, tl);
        body.addView(col, new FrameLayout.LayoutParams(MATCH, MATCH));

        if (companion != null) {
            companion.changeLabel(getString(R.string.LIB_TITLE));
            companion.art(art);
            companion.home(title, chipText, chipColor, primaryLabel(e) != null);
        }
        defaultFocus = play;
        startAction = this::onPlay;
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", history.isEmpty() ? R.string.LIB_TITLE : R.string.HINT_BACK, this::back);
        addHint("START", R.string.LIB_HINT_PRIMARY, this::onPlay);
    }

    /** The library's entries changed state (a Prepare moved on, the queue, a game ended): true. */
    private boolean libraryChanged() {
        List<LibraryScreen.Entry> now = LibraryScreen.entries(getFilesDir(), games, index, libEnv());
        if (now.size() != libEntries.size())
            return true;
        for (int i = 0; i < now.size(); i++) {
            LibraryScreen.Entry a = now.get(i), b = libEntries.get(i);
            if (!a.tid.equals(b.tid) || a.chip != b.chip || a.queuePos != b.queuePos)
                return true;
        }
        return false;
    }

    // ---- CONTROLS ----------------------------------------------------------------------------------------------

    /**
     * The device's buttons and the Xbox controller buttons the game gets: the SDL game controller mapping of
     * src/input/xinput_device.c (L1 = Black, R1 = White, L2 / R2 = the triggers, Select = Back, stick presses = the
     * thumbstick buttons), and the shell's own Android Back, which opens the in-game menu (GameMenu).
     */
    private void buildControls() {
        screenTitle.setText(R.string.CONTROLS);
        LinearLayout h = rowLayout();
        FrameLayout left = panel();
        Widgets.PadDiagram pad = new Widgets.PadDiagram(this);
        int[] labels = { R.string.PAD_L1, R.string.PAD_L2, R.string.PAD_R1, R.string.PAD_R2, R.string.PAD_SELECT,
                         R.string.PAD_START, R.string.PAD_DPAD, R.string.PAD_LSTICK, R.string.PAD_RSTICK,
                         R.string.PAD_FACE, R.string.PAD_SCREEN };
        for (int i = 0; i < labels.length; i++)
            pad.labels[i] = getString(labels[i]);
        left.addView(pad, new FrameLayout.LayoutParams(MATCH, MATCH));
        LinearLayout.LayoutParams ll = lp(0, MATCH, 1);
        ll.rightMargin = Theme.GAP;
        h.addView(left, ll);

        h.addView(detailPanel(), lp(640, MATCH));
        detailClear(getString(R.string.CONTROLS_CAPS), getString(R.string.CONTROLS_HEAD), null);
        int[][] rows = { { R.string.CTL_FACE, R.string.CTL_FACE_X }, { R.string.CTL_L1, R.string.CTL_L1_X },
                         { R.string.CTL_R1, R.string.CTL_R1_X }, { R.string.CTL_TRIGGERS, R.string.CTL_TRIGGERS_X },
                         { R.string.CTL_START, R.string.CTL_START_X }, { R.string.CTL_SELECT, R.string.CTL_SELECT_X },
                         { R.string.CTL_DPAD, R.string.CTL_DPAD_X }, { R.string.CTL_STICKS, R.string.CTL_STICKS_X },
                         { R.string.CTL_MENU, R.string.CTL_MENU_X } };
        for (int[] r : rows)
            detailKv(getString(r[0]), getString(r[1]));
        detailFooter(getString(R.string.CONTROLS_FOOTER), Theme.TEXT_FAINT);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("B", R.string.HINT_BACK, this::back);
    }

    // ---- STORAGE (docs/android/app-shell.md « Game library », Storage) -------------------------------------------

    /**
     * What the app stores, per game: the imported copy, the pack, the Prepare's files, the game's caches, the saves;
     * free space on top. Each deleted on its own, A twice within CONFIRM_MS, on a worker thread. Saves have their own
     * row and a stronger warning; the player's images and library folders are never listed.
     */
    private void buildStorage() {
        screenTitle.setText(R.string.LIB_STORAGE);
        Storage.Dirs dirs = storageDirs();
        LinearLayout h = rowLayout();
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setClipToPadding(false);
        scroll.setPadding(0, 0, 0, 24);
        LinearLayout list = column();
        list.setPadding(0, 4, 0, 4);
        scroll.addView(list, new FrameLayout.LayoutParams(MATCH, WRAP));

        TextView freeCaps = Theme.caps(this, getString(R.string.LIB_ST_FREE_CAPS), Theme.TEXT_FAINT);
        freeCaps.setPadding(36, 0, 0, 0);
        list.addView(freeCaps, lp(MATCH, WRAP));
        TextView free = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT,
                                   getString(R.string.LIB_ST_FREE_LINE, size(internalFree()), size(freeSpace())));
        free.setPadding(36, 10, 0, 0);
        list.addView(free, lp(MATCH, WRAP));

        List<Storage.Item> all = new ArrayList<>();
        for (String tid : Storage.titles(dirs, games)) {
            List<Storage.Item> items = Storage.items(dirs, tid, games.get(tid));
            if (items.isEmpty())
                continue;
            Library.Record r = games.get(tid);
            String name = r != null && !r.title.isEmpty() ? r.title : tid;
            TextView t = Theme.caps(this, name, Theme.TEXT_FAINT);
            t.setPadding(36, 0, 0, 0);
            LinearLayout.LayoutParams p = lp(MATCH, WRAP);
            p.topMargin = 30;
            p.bottomMargin = 8;
            list.addView(t, p);
            for (Storage.Item it : items) {
                list.addView(storageRow(it, name), lp(MATCH, Theme.ROW_H_SMALL));
                all.add(it);
            }
        }
        if (all.isEmpty()) {
            TextView none = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM,
                                       getString(R.string.LIB_ST_EMPTY));
            none.setPadding(36, 30, 0, 0);
            list.addView(none, lp(MATCH, WRAP));
        }
        storageMeasureLater(all);

        LinearLayout.LayoutParams sl = lp(920, MATCH);
        sl.rightMargin = Theme.GAP;
        h.addView(scroll, sl);
        h.addView(detailPanel(), lp(0, MATCH, 1));
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        detailClear(getString(R.string.LIB_STORAGE), getString(R.string.LIB_ST_FREE_CAPS), null);
        detailKv(getString(R.string.KV_INTERNAL_FREE), size(internalFree()));
        detailKv(getString(R.string.KV_FREE), size(freeSpace()));
        detailFooter(getString(R.string.LIB_ST_FOOTER), Theme.TEXT_FAINT);
        addHint("A", R.string.LIB_ST_HINT_DELETE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
        if (gameRunning())
            setNote(getString(R.string.LIB_RUNNING_NOTE, runningTitle()), NOTE_INFO);
    }

    private static String storageKey(Storage.Item it) {
        return it.tid + "/" + it.kind;
    }

    private int storageLabel(Storage.Kind k) {
        switch (k) {
        case DATA:
            return R.string.LIB_ST_DATA;
        case PACK:
            return R.string.LIB_ST_PACK;
        case WORK:
            return R.string.LIB_ST_WORK;
        case CACHES:
            return R.string.LIB_ST_CACHES;
        default:
            return R.string.LIB_ST_SAVES;
        }
    }

    private String storageSize(Storage.Item it) {
        Long n = storageSizes.get(storageKey(it));
        return n == null ? getString(R.string.SIZE_COUNTING) : size(n);
    }

    /** One stored thing of a game: its size, A twice deletes it; the detail panel says what it is. */
    private MenuRow storageRow(Storage.Item it, String gameName) {
        MenuRow r = row(it.kind == Storage.Kind.SAVES ? Icon.INFO : Icon.TRASH, getString(storageLabel(it.kind)),
                        storageSize(it), null);
        r.setOnClickListener(v -> onStorageDelete(r, it));
        onFocus(r, () -> {
            int detailText;
            switch (it.kind) {
            case DATA:
                detailText = R.string.LIB_ST_DATA_DETAIL;
                break;
            case PACK:
                detailText = R.string.LIB_ST_PACK_DETAIL;
                break;
            case WORK:
                detailText = R.string.LIB_ST_WORK_DETAIL;
                break;
            case CACHES:
                detailText = R.string.LIB_ST_CACHES_DETAIL;
                break;
            default:
                detailText = R.string.LIB_ST_SAVES_DETAIL;
                break;
            }
            detailClear(gameName, getString(storageLabel(it.kind)), getString(detailText));
            detailKv(getString(R.string.KV_SIZE), storageSize(it));
            for (File f : it.files) {
                if (f.exists())
                    detailKv(getString(R.string.KV_FOLDER), f.getAbsolutePath());
            }
            detailFooter(getString(it.kind == Storage.Kind.SAVES ? R.string.LIB_ST_SAVES_FOOTER
                                                                 : R.string.LIB_ST_FOOTER),
                         it.kind == Storage.Kind.SAVES ? Theme.WARN : Theme.TEXT_FAINT);
        });
        return r;
    }

    /** The sizes of items not measured yet, on a worker (an imported copy can be gigabytes), then a refresh. */
    private void storageMeasureLater(List<Storage.Item> items) {
        if (storageMeasurer != null && storageMeasurer.isAlive())
            return;
        List<Storage.Item> todo = new ArrayList<>();
        for (Storage.Item it : items) {
            if (!storageSizes.containsKey(storageKey(it)))
                todo.add(it);
        }
        if (todo.isEmpty())
            return;
        storageMeasurer = new Thread(() -> {
            java.util.Map<String, Long> m = new java.util.HashMap<>();
            for (Storage.Item it : todo)
                m.put(storageKey(it), Storage.measure(it));
            runOnUiThread(() -> {
                storageSizes.putAll(m);
                if (screen == Screen.STORAGE || screen == Screen.GAME_DATA)
                    refreshKeepingFocus();
            });
        }, "xba-storage");
        storageMeasurer.start();
    }

    /**
     * A twice within CONFIRM_MS deletes it (saves: a stronger warning first). The game that runs from these files is
     * ended first. An imported copy leaves its record's data_dir (the game stays in the library); its art and theme
     * go only when the title also left the library folder.
     */
    private void onStorageDelete(MenuRow row, Storage.Item it) {
        if (deleting)
            return;
        long now = SystemClock.uptimeMillis();
        String key = storageKey(it);
        if (!key.equals(storageArmedKey) || now - deleteArmedAt > CONFIRM_MS) {
            storageArmedKey = key;
            deleteArmedAt = now;
            row.setBadge(getString(R.string.DELETE_CONFIRM_BADGE), Theme.WARN);
            setNote(getString(it.kind == Storage.Kind.SAVES ? R.string.LIB_ST_SAVES_CONFIRM
                                                             : R.string.LIB_ST_DELETE_CONFIRM),
                    it.kind == Storage.Kind.SAVES ? NOTE_ERROR : NOTE_WARN);
            return;
        }
        storageArmedKey = null;
        deleteArmedAt = 0;
        deleting = true;
        row.setBadge(getString(R.string.DELETING), Theme.TEXT_DIM);
        setNote(getString(R.string.DELETING), NOTE_INFO);
        if (gameRunning() && it.tid.equals(launchedTid()))
            endRunningGame();
        Storage.Dirs dirs = storageDirs();
        Log.i(TAG, "launcher: storage: deleting " + key + " " + it.files);
        new Thread(() -> {
            Storage.delete(dirs, it, null);
            runOnUiThread(() -> {
                deleting = false;
                storageSizes.remove(key);
                if (it.kind == Storage.Kind.DATA) {
                    String dir = it.files.get(0).getAbsolutePath();
                    for (Library.Record r : games.games.values()) {
                        if (dir.equals(r.dataDir)) {
                            r.dataDir = "";
                            r.dataKind = "";
                        }
                    }
                    writeGames();
                    if (dir.equals(GameStore.chosenDir(this)))
                        GameStore.forget(this);
                    if (!indexGames.containsKey(it.tid))
                        GameTheme.clear(this, it.tid);
                    libArt.remove(it.tid);
                    sizeDir = null;
                    hashDir = null;
                    titleIdDir = null;
                    artTid = null;
                }
                if (screen == Screen.GAME_DATA && it.kind == Storage.Kind.DATA) {
                    history.clear();
                    show(libraryOn() ? (rec() != null ? Screen.HOME : Screen.LIBRARY)
                                     : state() == State.NONE ? Screen.WELCOME : Screen.HOME);
                } else {
                    refreshKeepingFocus();
                }
                setNote(getString(R.string.DELETED), NOTE_OK);
            });
        }, "xba-delete").start();
    }

    // ---- ABOUT -------------------------------------------------------------------------------------------------

    private void buildAbout() {
        screenTitle.setText(R.string.ABOUT);
        LinearLayout h = rowLayout();
        LinearLayout left = column();
        left.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.BrandMark mark = new Widgets.BrandMark(this);
        left.addView(mark, lp(150, 150));
        TextView name = Theme.text(this, Theme.T_HERO, Theme.MEDIUM, Theme.TEXT, getString(R.string.app_name));
        LinearLayout.LayoutParams nl = lp(MATCH, WRAP);
        nl.topMargin = 30;
        nl.bottomMargin = 16;
        left.addView(name, nl);
        left.addView(Theme.caps(this, getString(R.string.ABOUT_TAGLINE), Theme.ACCENT), lp(MATCH, WRAP));
        TextView t = Theme.body(this, getString(R.string.ABOUT_TEXT));
        LinearLayout.LayoutParams tl = lp(MATCH, WRAP);
        tl.topMargin = 30;
        left.addView(t, tl);
        LinearLayout.LayoutParams ll = lp(0, MATCH, 1);
        ll.rightMargin = 80;
        h.addView(left, ll);

        h.addView(detailPanel(), lp(820, MATCH));
        detailClear(getString(R.string.ABOUT_CAPS), getString(R.string.ABOUT_BUILD), null);
        detailKv(getString(R.string.KV_VERSION), version());
        detailKv(getString(R.string.KV_BUILD), getString(R.string.xba_version_code));
        detailKv(getString(R.string.KV_APP_ID), getPackageName());
        String sha = GameStore.expectedSha256(this);
        detailKv(getString(R.string.KV_GAME_CHECK), sha.isEmpty() ? getString(R.string.GAME_CHECK_NONE)
                                                                  : sha.substring(0, 16) + "…");
        detailKv(getString(R.string.KV_LICENCE), getString(R.string.ABOUT_LICENCE));
        detailFooter(getString(R.string.ABOUT_LEGAL), Theme.TEXT_FAINT);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private String version() {
        return getString(R.string.xba_version);
    }

    // ---- Game state --------------------------------------------------------------------------------------------

    /**
     * The selected library record when it stands for a game Home can show: its files are set (present or not: a
     * missing folder is « Game data missing »), or it has a source in the library folder. Null otherwise, and the
     * launcher falls back to the one-game rules (the remembered folder, recomp.cfg, the default game/).
     */
    private Library.Record rec() {
        Library.Record r = games.selected();
        if (r == null)
            return null;
        if (!r.dataDir.isEmpty())
            return r;
        return indexGames.containsKey(r.titleId) ? r : null;
    }

    /**
     * A game is ready when the selected game's folder holds default.xbe, or, with nothing remembered, when
     * recomp.cfg sets RECOMP_GAME_DIR (the native side then decides) or default.xbe was pushed to the default game/
     * folder.
     */
    private boolean gameReady() {
        Library.Record r = rec();
        if (r != null)
            return !r.dataDir.isEmpty()
                   && (Library.KIND_CFG.equals(r.dataKind) || GameStore.findXbe(new File(r.dataDir)) != null);
        String dir = GameStore.chosenDir(this);
        if (dir != null)
            return GameStore.findXbe(new File(dir)) != null;
        if (GameStore.cfgGameDir(this) != null)
            return true;
        return GameStore.findXbe(GameStore.defaultGameDir(this)) != null;
    }

    /** The folder the selected game is read from, or null when none is set up (or its files are not here). */
    private String gameDir() {
        Library.Record r = rec();
        if (r != null)
            return r.dataDir.isEmpty() ? null : r.dataDir;
        return legacyDir();
    }

    /** The one-game rule: the remembered folder, else RECOMP_GAME_DIR, else game/ when it holds default.xbe. */
    private String legacyDir() {
        String dir = GameStore.chosenDir(this);
        if (dir != null)
            return dir;
        String cfgDir = GameStore.cfgGameDir(this);
        if (cfgDir != null)
            return cfgDir;
        File def = GameStore.defaultGameDir(this);
        return GameStore.findXbe(def) != null ? def.getAbsolutePath() : null;
    }

    /** The folder the game process runs (or last ran): the preferences, as ShellActivity reads them. */
    private String launchedDir() {
        String dir = GameStore.chosenDir(this);
        if (dir != null)
            return dir;
        String cfgDir = GameStore.cfgGameDir(this);
        if (cfgDir != null)
            return cfgDir;
        File def = GameStore.defaultGameDir(this);
        return def == null ? null : def.getAbsolutePath();
    }

    /** The selected game is the one the process runs or last ran. */
    private boolean selectedIsLaunched() {
        String d = gameDir();
        return d != null && d.equals(launchedDir());
    }

    /** The title ID of the game the process runs or last ran, or null. */
    private String launchedTid() {
        String t = GameStore.launchedTitleId(this);
        if (t != null)
            return t;
        String d = launchedDir();
        return d == null ? null : Pack.xbeTitleId(GameStore.findXbe(new File(d)));
    }

    private State state() {
        Library.Record r = rec();
        String dir;
        if (r != null) {
            if (r.dataDir.isEmpty())
                return State.LIBRARY;
            dir = r.dataDir;
            if (!Library.KIND_CFG.equals(r.dataKind) && GameStore.findXbe(new File(dir)) == null)
                return missingState(dir);
        } else {
            dir = GameStore.chosenDir(this);
            if (dir != null) {
                if (GameStore.findXbe(new File(dir)) == null)
                    return missingState(dir);
            } else if (!gameReady()) {
                return State.NONE;
            }
        }
        String d = gameDir();
        return d != null && d.equals(hashDir) && Boolean.FALSE.equals(hashOk) ? State.WRONG : State.READY;
    }

    /** A folder without default.xbe: unreadable (no All files access, outside the app) or gone. */
    private State missingState(String dir) {
        File ext = GameStore.externalFilesDir(this);
        boolean outside = ext == null || !dir.startsWith(ext.getAbsolutePath());
        return new File(dir).exists() && !Environment.isExternalStorageManager() && outside ? State.NO_ACCESS
                                                                                            : State.MISSING;
    }

    private static int stateLabel(State s) {
        switch (s) {
        case READY:
            return R.string.STATE_READY;
        case MISSING:
            return R.string.STATE_MISSING;
        case NO_ACCESS:
            return R.string.STATE_NO_ACCESS;
        case WRONG:
            return R.string.STATE_WRONG;
        case LIBRARY:
            return R.string.LIB_CHIP_NOT_IMPORTED;
        default:
            return R.string.STATE_NONE;
        }
    }

    private static int stateColor(State s) {
        return s == State.READY ? Theme.OK : s == State.WRONG ? Theme.ERROR : Theme.WARN;
    }

    private String stateText(State s, String dir) {
        switch (s) {
        case MISSING:
            return getString(R.string.GAME_FOLDER_GONE, dir);
        case NO_ACCESS:
            return getString(R.string.GAME_FOLDER_NO_ACCESS, dir);
        case WRONG:
            return getString(R.string.WRONG_GAME_HOME);
        case NONE:
            return getString(R.string.NO_GAME_SET_UP);
        default:
            return "";
        }
    }

    private String gameTitle(String dir) {
        Library.Record r = rec();
        if (r != null && !r.title.isEmpty() && (dir == null ? r.dataDir.isEmpty() : dir.equals(r.dataDir)))
            return r.title;
        String t = GameStore.chosenTitle(this);
        if (dir != null && dir.equals(GameStore.chosenDir(this)) && !t.isEmpty())
            return t;
        if (dir == null)
            return getString(R.string.app_name);
        File xbe = GameStore.findXbe(new File(dir));
        String x = xbe == null ? "" : GameStore.xbeTitle(xbe);
        return x.isEmpty() ? new File(dir).getName() : x;
    }

    private Storage.Dirs storageDirs() {
        return new Storage.Dirs(getFilesDir(), GameStore.externalFilesDir(this));
    }

    /** A copy this app made (games/<TID>, or the older game/): the only game files it ever deletes. */
    private boolean ownedCopy(String dir) {
        return Storage.ownedCopy(storageDirs(), dir) && new File(dir).isDirectory();
    }

    /** The selected game came from recomp.cfg's RECOMP_GAME_DIR. */
    private boolean fromCfg() {
        Library.Record r = rec();
        if (r != null)
            return Library.KIND_CFG.equals(r.dataKind);
        return GameStore.chosenDir(this) == null && GameStore.cfgGameDir(this) != null;
    }

    private String sourceLabel(String dir) {
        if (ownedCopy(dir))
            return getString(R.string.SOURCE_IMPORTED);
        if (fromCfg())
            return getString(R.string.SOURCE_CFG);
        return getString(R.string.SOURCE_IN_PLACE);
    }

    private String sizeText(String dir) {
        return dir.equals(sizeDir) && sizeBytes >= 0 ? size(sizeBytes) : getString(R.string.SIZE_COUNTING);
    }

    /** The folder's size, counted once per folder on a background thread; the screen is rebuilt when it is known. */
    private void sizeLater(String dir) {
        if (dir == null || dir.equals(sizeDir))
            return;
        sizeDir = dir;
        sizeBytes = -1;
        new Thread(() -> {
            long n = GameStore.treeSize(new File(dir));
            runOnUiThread(() -> {
                if (!dir.equals(sizeDir))
                    return;
                sizeBytes = n;
                if (screen == Screen.HOME || screen == Screen.GAME_DATA)
                    refreshKeepingFocus();
            });
        }, "xba-size").start();
    }

    /**
     * WRONG_GAME on HOME: default.xbe of the current folder against the hash this APK was built for, once per folder
     * on a background thread (a picked game was checked already; this catches files changed since, or a folder set
     * up through recomp.cfg or adb). No expected hash (the shell, the stub): nothing to check.
     */
    private void checkHashLater(String dir) {
        if (dir == null || dir.equals(hashDir) || GameStore.expectedSha256(this).isEmpty())
            return;
        hashDir = dir;
        hashOk = null;
        new Thread(() -> {
            Boolean ok = null;
            File xbe = GameStore.findXbe(new File(dir));
            if (xbe != null) {
                try {
                    String sha = GameStore.sha256(xbe);
                    ok = GameStore.hashMatches(this, sha);
                    Log.i(TAG, "launcher: " + xbe + " sha256 " + sha + (ok ? "" : " (WRONG_GAME)"));
                } catch (IOException e) {
                    Log.w(TAG, "launcher: cannot hash " + xbe, e);
                }
            }
            Boolean result = ok;
            runOnUiThread(() -> {
                if (!dir.equals(hashDir))
                    return;
                hashOk = result;
                if (Boolean.FALSE.equals(result) && screen == Screen.HOME)
                    refreshKeepingFocus();
            });
        }, "xba-hash").start();
    }

    /** Rebuilds the current screen (a fact computed in the background arrived), keeping its focus and note. */
    private void refreshKeepingFocus() {
        String text = noteText;
        int kind = noteKind;
        show(screen, 0);
        noteText = text;
        noteKind = kind;
        paintNote();
    }

    // ---- Shortcuts -----------------------------------------------------------------------------------------------

    /** Long-press on the app icon: « Change game » and « Launcher ». Dynamic: the package name differs per title. */
    private void publishShortcuts() {
        try {
            ShortcutManager sm = getSystemService(ShortcutManager.class);
            if (sm == null)
                return;
            int flags = Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK;
            ShortcutInfo change = new ShortcutInfo.Builder(this, "change_game")
                    .setShortLabel(getString(R.string.SHORTCUT_CHANGE_GAME))
                    .setLongLabel(getString(R.string.SHORTCUT_CHANGE_GAME_LONG))
                    .setIntent(new Intent(this, LauncherActivity.class).setAction(ACTION_CHOOSE_GAME).addFlags(flags))
                    .setRank(0)
                    .build();
            ShortcutInfo home = new ShortcutInfo.Builder(this, "launcher")
                    .setShortLabel(getString(R.string.SHORTCUT_LAUNCHER))
                    .setLongLabel(getString(R.string.SHORTCUT_LAUNCHER_LONG))
                    .setIntent(new Intent(this, LauncherActivity.class).setAction(ACTION_SHOW_LAUNCHER)
                                       .addFlags(flags))
                    .setRank(1)
                    .build();
            List<ShortcutInfo> list = new ArrayList<>();
            Collections.addAll(list, change, home);
            sm.setDynamicShortcuts(list);
        } catch (RuntimeException e) {
            Log.w(TAG, "launcher: could not publish the shortcuts", e);
        }
    }

    // ---- Starting the game ----------------------------------------------------------------------------------

    /** The main process, where ShellActivity and the runtime live, when it is running. */
    private int mainProcessPid() {
        ActivityManager am = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> procs = am == null ? null : am.getRunningAppProcesses();
        if (procs == null)
            return 0;
        for (ActivityManager.RunningAppProcessInfo p : procs) {
            if (p.processName.equals(getPackageName()))
                return p.pid;
        }
        return 0;
    }

    /** A task of this app whose activity is ShellActivity (the game's window). */
    private boolean shellAlive() {
        ActivityManager am = getSystemService(ActivityManager.class);
        if (am == null)
            return false;
        for (ActivityManager.AppTask t : am.getAppTasks()) {
            try {
                ActivityManager.RecentTaskInfo info = t.getTaskInfo();
                if (info.numActivities > 0 && info.baseActivity != null
                    && info.baseActivity.getClassName().equals(ShellActivity.class.getName()))
                    return true;
            } catch (IllegalArgumentException ignored) {   // the task went away meanwhile
            }
        }
        return false;
    }

    private boolean gameRunning() {
        return mainProcessPid() != 0 && shellAlive();
    }

    /**
     * Ends the game's process, if any: after a new choice (the runtime maps its memory once per process, so the new
     * game needs a fresh one) and before deleting its files. Saves are in internal storage and stay.
     */
    private void endRunningGame() {
        int pid = mainProcessPid();
        if (pid == 0)
            return;
        Log.i(TAG, "launcher: ending the running game (pid " + pid + ")");
        Process.killProcess(pid);
        long until = SystemClock.uptimeMillis() + 2000;
        while (mainProcessPid() != 0 && SystemClock.uptimeMillis() < until)
            SystemClock.sleep(50);
    }

    /**
     * Starts ShellActivity for the selected game. A live game with no new choice is only brought to the front; a
     * main process without a live ShellActivity (SDL_main returned, e.g. after NO_GAME_DATA) cannot start the
     * runtime again, so it is ended first (restart = true does the same unconditionally). Another game than the one
     * the process runs: that process is ended, the selected game becomes the launched one (GameStore.rememberLaunch,
     * the only place it is written) and the shared caches switch owner (SaveCaches).
     */
    private void launchShell(boolean restart) {
        boolean other = !selectedIsLaunched() && gameDir() != null;
        int pid = mainProcessPid();
        if (pid != 0 && !restart && (other || !shellAlive())) {
            Log.i(TAG, "launcher: main process " + pid + (other ? " runs another game" : " has no game window")
                       + ", restarting it");
            restart = true;
        }
        if (restart)
            endRunningGame();
        String tid = titleId();
        Library.Record r = rec();
        if (other || (r != null && tid != null && !tid.equals(GameStore.launchedTitleId(this)))) {
            String dir = gameDir();
            boolean cfg = r != null && Library.KIND_CFG.equals(r.dataKind);
            GameStore.rememberLaunch(this, cfg ? null : dir, gameTitle(dir), tid);
            Log.i(TAG, "launcher: launched game set: " + dir + " (" + tid + ")");
        }
        String owner = tid != null ? tid : r != null ? r.titleId : null;
        boolean resume = pid != 0 && !restart;   // a live game brought back: its play session goes on
        if (owner != null && !resume) {   // the caches move only while no game process uses them
            try {
                if (SaveCaches.switchTo(GameTheme.saveDir(this), Library.cacheOwnerFile(getFilesDir()), owner))
                    Log.i(TAG, "launcher: game caches switched to " + owner);
            } catch (IOException e) {
                Log.w(TAG, "launcher: cannot switch the game caches to " + owner, e);
            }
        }
        if (!resume) {
            long now = System.currentTimeMillis();
            GameStore.sessionStart(this, now, owner);
            if (owner != null) {
                Library.sessionStart(games, owner, now);
                writeGames();
            }
        }
        Log.i(TAG, "launcher: play");
        Intent i = new Intent(this, ShellActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (restart)
            i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
    }

    /** The app icon while the game runs: back to it as it is (whatever game the library shows). */
    private void resumeShell() {
        startActivity(new Intent(this, ShellActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        finish();
    }

    /**
     * Play while another game runs: the first press says it will close (badge and note), a second within CONFIRM_MS
     * goes on. True when the launch may go on.
     */
    private boolean confirmSwitch() {
        if (selectedIsLaunched() || !gameRunning())
            return true;
        long now = SystemClock.uptimeMillis();
        if (switchArmedAt != 0 && now - switchArmedAt <= CONFIRM_MS) {
            switchArmedAt = 0;
            Log.i(TAG, "launcher: switching games, ending " + runningTitle());
            return true;
        }
        switchArmedAt = now;
        if (homePrimary != null)
            homePrimary.setBadge(getString(R.string.PREPARE_CANCEL_BADGE), Theme.WARN);
        setNote(getString(R.string.LIB_SWITCH_CONFIRM, runningTitle()), NOTE_WARN);
        return false;
    }

    /** The running (or last launched) game's title, for the switch warning. */
    private String runningTitle() {
        String t = GameStore.chosenTitle(this);
        String tid = launchedTid();
        Library.Record r = tid == null ? null : games.get(tid);
        if (r != null && !r.title.isEmpty())
            return r.title;
        return t.isEmpty() ? getString(R.string.LIB_ANOTHER_GAME) : t;
    }

    /**
     * ACTION_RESTART_GAME: the runtime cannot start a title twice in one process (it maps its memory once), so the
     * game's process is ended here and ShellActivity starts in a new one, which takes the launch data back. Nothing
     * is shown and the play session is not closed: to the player it is the game going back to its own menu.
     */
    private void restartGame() {
        Log.i(TAG, "launcher: the game asked for a restart (quick reboot)");
        endRunningGame();
        startActivity(new Intent(this, ShellActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
        finish();
        overridePendingTransition(0, 0);
    }

    /**
     * A new game was checked (picked, imported, copied): it becomes a library record and the selected game, then
     * HOME with Play focused. The launched game (the preferences) changes only when it is played (launchShell).
     * After an import from the library, a Prepare that is needed starts at once.
     */
    private void onGameChosen(String dir, String title) {
        File d = new File(dir);
        String tid = Pack.xbeTitleId(GameStore.findXbe(d));
        if (tid == null)
            tid = Library.localKey(d.getAbsolutePath());
        long now = System.currentTimeMillis();
        Library.Record r = games.get(tid);
        if (r == null) {
            r = new Library.Record();
            r.titleId = tid;
            r.addedAt = now;
            games.put(r);
        }
        if (!title.isEmpty())
            r.title = title;
        else if (r.title.isEmpty())
            r.title = d.getName();
        r.dataDir = d.getAbsolutePath();
        r.dataKind = Storage.ownedCopy(storageDirs(), dir) ? Library.KIND_IMPORTED : Library.KIND_IN_PLACE;
        if (Library.KIND_IN_PLACE.equals(r.dataKind) && r.source.isEmpty())
            r.source = LibraryIndex.fileLoc(d);
        if (chosenSha != null)
            r.xbeSha256 = chosenSha;
        games.selected = tid;
        writeGames();
        boolean fromLibrary = importFromLibrary;
        importFromLibrary = false;
        importTid = null;
        titleIdDir = null;   // a new game may sit in the same folder: read its title ID again
        artTid = null;
        libArt.remove(tid);
        Log.i(TAG, "launcher: game set: " + dir + " (" + title + ", " + tid + ")");
        hashDir = r.dataDir;
        hashOk = Boolean.TRUE;
        sizeDir = null;
        history.clear();
        if (libraryOn())
            history.push(Screen.LIBRARY);
        focusMemory.remove(Screen.HOME);
        show(Screen.HOME);
        feedback.play(UiFeedback.Cue.DONE, host);
        setNote(getString(R.string.READY_TO_PLAY, title.isEmpty() ? d.getName() : title), NOTE_OK);
        if (fromLibrary && prep() == Prep.NEEDED)
            startPrepare();
    }

    /** « Change game » (Home's bottom-screen shortcut): the library when it is on, else the choice of source. */
    private void onChangeGame() {
        if (libraryOn())
            openLibrary();
        else
            go(Screen.SOURCE);
    }

    // ---- Folder --------------------------------------------------------------------------------------------

    private void onChooseFolder() {
        pickLibrary = false;
        if (Environment.isExternalStorageManager())
            openTreePicker();
        else
            go(Screen.FOLDER_ACCESS);
    }

    /**
     * « Game library folder »: the tree picker; without All files access the FOLDER_ACCESS screen first (granted:
     * the folder is read as files; otherwise through the Storage Access Framework, ISOs imported, folders copied).
     */
    private void onChooseLibraryFolder() {
        pickLibrary = true;
        if (Environment.isExternalStorageManager())
            openTreePicker();
        else
            go(Screen.FOLDER_ACCESS);
    }

    private void askAllFilesAccess() {
        Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                              Uri.parse("package:" + getPackageName()));
        try {
            startActivityForResult(i, REQ_ALL_FILES);
        } catch (ActivityNotFoundException e) {
            try {
                startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), REQ_ALL_FILES);
            } catch (ActivityNotFoundException e2) {
                openTreePicker();
            }
        }
    }

    private void openTreePicker() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), pickLibrary ? REQ_LIBRARY_TREE
                                                                                             : REQ_TREE);
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.PICKER_MISSING), NOTE_ERROR);
        }
    }

    private void onTreePicked(Uri tree) {
        String path = Environment.isExternalStorageManager() ? GameStore.treeUriToPath(tree) : null;
        Log.i(TAG, "launcher: folder " + tree + (path != null ? " = " + path : " (copy)"));
        File dir = path == null ? null : new File(path);
        if (dir != null && dir.list() != null) {
            if (GameStore.findXbe(dir) == null)
                setNote(getString(R.string.NOT_A_GAME_FOLDER), NOTE_ERROR);
            else
                runWorker(R.string.CHECKING, () -> checkInPlace(dir));
            return;
        }
        // No All files access, a provider that is not a storage volume, or a path we cannot list: copy.
        try {
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
        runWorker(R.string.COPYING, () -> copyTree(tree));
    }

    /** Worker: the folder is read in place; check default.xbe's hash, then remember it. */
    private void checkInPlace(File dir) throws IOException {
        File xbe = GameStore.findXbe(dir);
        String sha = GameStore.sha256(xbe);
        Log.i(TAG, "launcher: " + xbe + " sha256 " + sha);
        if (!GameStore.hashMatches(this, sha)) {
            fail(getString(R.string.WRONG_GAME));
            return;
        }
        chosen(dir.getAbsolutePath(), GameStore.xbeTitle(xbe));
    }

    private static final class Doc {
        final String id, path;
        final boolean dir;
        final long size;

        Doc(String id, String path, boolean dir, long size) {
            this.id = id;
            this.path = path;
            this.dir = dir;
            this.size = size;
        }
    }

    private void listTree(Uri tree, String parentId, String prefix, List<Doc> out) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                          DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE };
        List<Doc> dirs = new ArrayList<>();
        try (Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            if (c == null)
                throw new IOException("cannot list " + prefix);
            while (c.moveToNext()) {
                if (cancelRequested)
                    return;
                String name = c.getString(1);
                boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                Doc d = new Doc(c.getString(0), prefix + name, isDir, c.isNull(3) ? 0 : c.getLong(3));
                out.add(d);
                if (isDir)
                    dirs.add(d);
            }
        }
        for (Doc d : dirs)
            listTree(tree, d.id, d.path + "/", out);
    }

    /** Worker: copy a picked tree into the app (fallback without All files access, XBA-40). */
    private void copyTree(Uri tree) throws IOException {
        copyTree(tree, DocumentsContract.getTreeDocumentId(tree), true);
    }

    /**
     * Worker: copy the folder rootId of tree into <external files>/games/<TID>. release: give the read permission
     * back at the end (a picked game folder; the library folder keeps it).
     */
    private void copyTree(Uri tree, String rootId, boolean release) throws IOException {
        ContentResolver cr = getContentResolver();
        try {
            List<Doc> docs = new ArrayList<>();
            listTree(tree, rootId, "", docs);
            if (cancelled())
                return;

            Doc xbe = null;
            long total = 0;
            for (Doc d : docs) {
                if (!d.dir) {
                    total += d.size;
                    if (d.path.equalsIgnoreCase("default.xbe"))
                        xbe = d;
                }
            }
            if (xbe == null) {
                fail(getString(R.string.NOT_A_GAME_FOLDER));
                return;
            }
            String sha;
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, xbe.id))) {
                if (in == null)
                    throw new IOException("cannot open default.xbe");
                sha = GameStore.sha256(in);
            }
            Log.i(TAG, "launcher: copy: default.xbe sha256 " + sha + ", " + docs.size() + " entries, " + total
                       + " bytes");
            if (!GameStore.hashMatches(this, sha)) {
                fail(getString(R.string.WRONG_GAME));
                return;
            }
            if (!enoughSpace(total))
                return;

            File staged = stagingDir();
            byte[] buf = new byte[1 << 20];
            long done = 0;
            for (Doc d : docs) {
                File dst = new File(staged, d.path);
                if (d.dir) {
                    if (!dst.mkdirs() && !dst.isDirectory())
                        throw new IOException("cannot create " + dst);
                    continue;
                }
                progress(done, total, d.path);
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id));
                     OutputStream out = new FileOutputStream(dst)) {
                    if (in == null)
                        throw new IOException("cannot open " + d.path);
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        if (cancelRequested)
                            break;
                        out.write(buf, 0, n);
                        done += n;
                        progress(done, total, d.path);
                    }
                }
                if (cancelRequested) {
                    GameStore.deleteTree(staged);
                    cancelled();
                    return;
                }
            }
            finishImport(staged);
        } finally {
            if (release) {
                try {
                    cr.releasePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (SecurityException ignored) {
                }
            }
        }
    }

    // ---- ISO -----------------------------------------------------------------------------------------------

    private void openIsoPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try {
            startActivityForResult(i, REQ_ISO);
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.PICKER_MISSING), NOTE_ERROR);
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null,
                                                   null)) {
            if (c != null && c.moveToFirst())
                return c.getString(0);
        } catch (RuntimeException ignored) {
        }
        String last = uri.getLastPathSegment();
        return last == null ? "" : last;
    }

    private void onIsoPicked(Uri uri) {
        String name = displayName(uri);
        String lower = name.toLowerCase(Locale.ROOT);
        Log.i(TAG, "launcher: image " + uri + " (" + name + ")");
        if (!lower.endsWith(".iso") && !lower.endsWith(".xiso")) {
            setNote(getString(R.string.NOT_AN_ISO, name), NOTE_ERROR);
            return;
        }
        runWorker(R.string.EXTRACTING, () -> extractIso(uri, name));
    }

    /** Worker: probe, check the space, extract into the staging dir (libxba_import.so), check the hash, swap. */
    private void extractIso(Uri uri, String name) throws IOException {
        extractIso(getContentResolver().openFileDescriptor(uri, "r"), name);
    }

    /** extractIso() of an image already open (the library's: a file, or a document of the library folder). */
    private void extractIso(ParcelFileDescriptor opened, String name) throws IOException {
        try (ParcelFileDescriptor pfd = opened) {
            if (pfd == null)
                throw new IOException("cannot open " + name);
            String[] title = new String[1];
            long[] p = XisoNative.probe(pfd.getFd(), title);
            if (p[0] != 0) {
                fail(getString(R.string.NOT_A_GAME_IMAGE, name + ": " + XisoNative.strerror((int)p[0])));
                return;
            }
            if (p[2] == 0) {
                fail(getString(R.string.NOT_A_GAME_IMAGE, name));
                return;
            }
            if (!enoughSpace(p[1]))
                return;
            File staged = stagingDir();
            int rc = XisoNative.extract(pfd.getFd(), staged.getAbsolutePath(), (done, total, path) -> {
                progress(done, total, path);
                return cancelRequested;
            });
            if (rc == XisoNative.canceledCode()) {
                GameStore.deleteTree(staged);
                cancelled();
                return;
            }
            if (rc != 0) {
                GameStore.deleteTree(staged);
                if (rc == XisoNative.noSpaceCode())
                    fail(getString(R.string.NOT_ENOUGH_SPACE, size(p[1]), size(freeSpace())));
                else
                    fail(getString(R.string.IMPORT_FAILED, XisoNative.strerror(rc)));
                return;
            }
            finishImport(staged);
        }
    }

    // ---- Shared import steps -------------------------------------------------------------------------------

    /**
     * <external files dir>/games/.import, emptied: the import goes there, and becomes games/<TID> only once checked
     * (the older game/ is never an import target again: docs « Game library »).
     */
    private File stagingDir() throws IOException {
        File ext = GameStore.externalFilesDir(this);
        if (ext == null)
            throw new IOException("no external files dir");
        File games = new File(ext, Library.GAMES_SUBDIR);
        File staged = new File(games, ".import");
        GameStore.deleteTree(staged);
        GameStore.deleteTree(new File(games, ".import.partial"));
        if (!staged.mkdirs() && !staged.isDirectory())
            throw new IOException("cannot create " + staged);
        return staged;
    }

    /** The imported tree is complete: default.xbe and its hash, then it becomes game/ and the remembered choice. */
    private void finishImport(File staged) throws IOException {
        progressIndeterminate(R.string.CHECKING);
        File xbe = GameStore.findXbe(staged);
        if (xbe == null) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.NOT_A_GAME_FOLDER));
            return;
        }
        String sha = GameStore.sha256(xbe);
        Log.i(TAG, "launcher: imported default.xbe sha256 " + sha);
        if (!GameStore.hashMatches(this, sha)) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.WRONG_GAME));
            return;
        }
        String title = GameStore.xbeTitle(xbe);
        String tid = Pack.xbeTitleId(xbe);
        if (tid == null)
            tid = Library.localKey(sha);
        File game = GameStore.swapInto(staged, Library.importDir(GameStore.externalFilesDir(this), tid));
        if (game == null) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.IMPORT_FAILED, "rename"));
            return;
        }
        Log.i(TAG, "launcher: library: imported " + tid + " into " + game);
        dropLegacyCopy(tid, game);
        chosen(game.getAbsolutePath(), title);
    }

    /**
     * Worker, after a checked import into games/<TID>: the older game/ copy of the same title goes (docs « Game
     * library », migration), unless a game process may be using it. The launched game moves to the new copy.
     */
    private void dropLegacyCopy(String tid, File game) {
        File legacy = GameStore.defaultGameDir(this);
        if (legacy == null || legacy.equals(game) || !tid.equals(Pack.xbeTitleId(GameStore.findXbe(legacy))))
            return;
        if (mainProcessPid() != 0) {
            Log.i(TAG, "launcher: library: the older copy " + legacy + " stays (a game process is running)");
            return;
        }
        String launched = GameStore.chosenDir(this);
        if (launched == null || launched.equals(legacy.getAbsolutePath()))
            GameStore.rememberLaunch(this, game.getAbsolutePath(), GameStore.xbeTitle(GameStore.findXbe(game)), tid);
        GameStore.deleteTree(legacy);
        Log.i(TAG, "launcher: library: removed the older copy " + legacy);
    }

    private long freeSpace() {
        File ext = GameStore.externalFilesDir(this);
        return ext == null ? 0 : ext.getUsableSpace();
    }

    private boolean enoughSpace(long need) {
        long free = freeSpace();
        if (free >= need + SPACE_MARGIN)
            return true;
        fail(getString(R.string.NOT_ENOUGH_SPACE, size(need), size(free)));
        return false;
    }

    private String size(long bytes) {
        return Formatter.formatShortFileSize(this, bytes);
    }

    // ---- Worker and progress -------------------------------------------------------------------------------

    private interface Work {
        void run() throws IOException;
    }

    /** Runs work on a background thread behind the IMPORT screen; its outcome is handled in onWorkerDone. */
    private void runWorker(int label, Work work) {
        if (worker != null && worker.isAlive())
            return;
        cancelRequested = false;
        workerNote = null;
        chosenDir = null;
        chosenTitle = null;
        chosenSha = null;
        importLabel = label;
        if (screen == Screen.FOLDER_ACCESS && !history.isEmpty())
            importReturn = history.pop();   // SOURCE: a failed import comes back to the choice of source
        else
            importReturn = screen == Screen.IMPORT || screen == null ? Screen.SOURCE : screen;
        int i = focusables.indexOf(focused());
        if (i >= 0 && screen != null)
            focusMemory.put(screen, i);
        show(Screen.IMPORT);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        worker = new Thread(() -> {
            try {
                work.run();
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "launcher: import failed", e);
                fail(getString(R.string.IMPORT_FAILED, String.valueOf(e.getMessage())));
            } finally {
                runOnUiThread(this::onWorkerDone);
            }
        }, "xba-import");
        worker.start();
    }

    private void onWorkerDone() {
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (isFinishing())
            return;
        if (chosenDir != null) {
            onGameChosen(chosenDir, chosenTitle == null ? "" : chosenTitle);
            return;
        }
        importTid = null;
        importFromLibrary = false;
        String text = workerNote;
        int kind = workerNoteKind;
        show(importReturn);
        if (text != null)
            setNote(text, kind);
    }

    /** Worker: the game is checked; its theme is baked here, off the UI thread, before HOME shows it. */
    private void chosen(String dir, String title) {
        File d = new File(dir);
        GameTheme.ensure(this, d, Pack.xbeTitleId(GameStore.findXbe(d)), title);
        try {
            chosenSha = GameStore.sha256(GameStore.findXbe(d));
        } catch (IOException | RuntimeException e) {
            chosenSha = null;
        }
        chosenTitle = title;
        chosenDir = dir;
    }

    private void progress(long done, long total, String path) {
        long now = SystemClock.uptimeMillis();
        if (now - lastUiUpdate < 100 && done < total)
            return;
        lastUiUpdate = now;
        runOnUiThread(() -> {
            if (screen != Screen.IMPORT || ring == null)
                return;
            float frac = total > 0 ? Math.min(1f, done / (float)total) : -1;
            String pct = frac >= 0 ? String.format(Locale.getDefault(), "%d%%", Math.round(frac * 100)) : "";
            ring.set(frac, pct, getString(importLabel), getString(R.string.PROGRESS_BYTES, size(done), size(total)));
            importBytes.setText(getString(R.string.PROGRESS_BYTES, size(done), size(total)));
            importFile.setText(path == null ? "" : path);
        });
    }

    private void progressIndeterminate(int label) {
        runOnUiThread(() -> {
            if (screen != Screen.IMPORT || ring == null)
                return;
            importLabel = label;
            importHead.setText(label);
            ring.set(-1, "", getString(label), "");
            importFile.setText("");
            if (cancelRow != null)
                cancelRow.setDim(true);
        });
    }

    private void fail(String text) {
        workerNoteKind = NOTE_ERROR;
        workerNote = text;
    }

    /** True (and says so) when the player pressed Cancel. */
    private boolean cancelled() {
        if (!cancelRequested)
            return false;
        Log.i(TAG, "launcher: import canceled");
        workerNoteKind = NOTE_INFO;
        workerNote = getString(R.string.CANCELED);
        return true;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ALL_FILES) {
            Log.i(TAG, "launcher: All files access "
                       + (Environment.isExternalStorageManager() ? "granted" : "refused"));
            openTreePicker();   // in place when granted, copied otherwise
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        if (requestCode == REQ_TREE)
            onTreePicked(data.getData());
        else if (requestCode == REQ_LIBRARY_TREE)
            onLibraryTreePicked(data.getData());
        else if (requestCode == REQ_ISO)
            onIsoPicked(data.getData());
        else if (requestCode == REQ_LSFG_DLL)
            onLsfgDllPicked(data.getData());
    }
}
