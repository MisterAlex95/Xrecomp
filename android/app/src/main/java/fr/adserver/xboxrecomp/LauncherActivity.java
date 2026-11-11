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
 * - WELCOME (first run, no game set up) -> SOURCE (game folder, disc image or library folder) -> FOLDER_ACCESS (All
 *   files access explained, or copy) -> IMPORT (progress ring; B cancels) -> HOME;
 * - LIBRARY: the game library's grid, a context line, the Options sheet on Y (Rescan, Storage, Library folder,
 *   Settings, About); STORAGE: what the app stores, per game;
 * - HOME: the game's art and title, one line (when it was played, or its state), one row of buttons: a solid Play
 *   (or Prepare, Choose…) and three quiet pills, Settings, Controls, Manage;
 * - SETTINGS: four tabs (General, Graphics, Audio, Advanced): the launcher's preferences and a few RECOMP_* options
 *   saved to recomp.cfg (RecompConfig), applied at the next start; About ends Advanced;
 * - CONTROLS: the device's buttons and the Xbox controller buttons the game gets;
 * - GAME_DATA (« Manage »): change game, delete the imported copy and the game's stored things; ABOUT;
 * - PREPARE: Home's layout with the on-device Prepare's progress (dimmed after two idle minutes, for OLED screens).
 *
 * The « Calm » look (docs/android/app-shell.md « Look, motion and sound »): a title-only top bar, one footer line
 * (the note left, at most three hints right), list rows of label and value with the focused row's description in a
 * quiet column. D-pad / stick move the focus (remembered per screen and per Settings tab; one highlight glides from
 * item to item), A selects, B goes back, left / right change a setting, L1 / R1 switch Settings' tabs, Start plays;
 * touch works too. Screens fade and slide in; moves, selects, backs, errors and a finished Prepare have a short sound
 * and a haptic (UiFeedback).
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
    private TextView screenTitle, note, topRight;
    /** The top bar (hidden on Home and Preparing, which show the game's title big), its tabs' slot. */
    private LinearLayout topBar, topTabs;
    /** The stage's column (top bar, body, footer): its focus is blocked while a sheet is open. */
    private LinearLayout stageRoot;
    /** The open sheet (the library's Options), and the view focused before it opened. */
    private Widgets.Sheet sheet;
    private View sheetReturn;
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
    private Widgets.PillButton homePrimary;
    /** Home's status line while a Prepare runs (the poll updates it). */
    private Widgets.Chip homeStatus;

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
    private volatile String workerNote, chosenDir, chosenTitle;
    private volatile int workerNoteKind;

    // ---- Prepare ----
    private String titleIdDir, titleIdValue;
    private final android.os.Handler poller = new android.os.Handler(android.os.Looper.getMainLooper());
    private boolean resumed;
    private Prep homePrep;
    private PrepareProtocol.Status prepShown;
    private Widgets.Chip prepStatus;
    private Widgets.Meter prepMeter;
    private TextView prepPct, prepMsg, prepEta, prepCaption;
    private View prepProgress;
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
        // The second screen (AYN Thor, docs/android/app-shell.md « Dual screen »): Home's, the library's or the
        // Prepare's panel; its one action is the main screen's.
        companion = new LauncherCompanion(this, this::onPlay);
        companion.libraryActions(() -> {
            LibraryScreen.Entry e = focusedEntry();
            if (e != null)
                libraryPrimary(e);
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

    Widgets.PillButton homePrimaryForTest() {
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
     * The parts every screen shares: the top bar (the screen's title, Settings' tabs, a quiet fact on the right), the
     * body, and one footer line (the note left, at most three hints right). Built again when the game theme changes
     * (syncTheme): it holds no state of its own but the views.
     */
    private void buildStage() {
        if (screenAnim != null)
            screenAnim.cancel();
        host = new FrameLayout(this);
        backdrop = new Widgets.Backdrop();
        backdrop.setHero(heroFor(screen));
        host.setBackground(backdrop);
        LinearLayout root = column();
        host.addView(root, new FrameLayout.LayoutParams(MATCH, MATCH));
        stageRoot = root;
        sheet = null;
        glide = new Widgets.Glide(this);
        host.addView(glide, new FrameLayout.LayoutParams(MATCH, MATCH));
        buildDim();
        host.getViewTreeObserver().addOnGlobalFocusChangeListener((was, now) -> {
            if (!building && was != null && (now instanceof Widgets.Selectable || now instanceof GameCard))
                feedback.play(UiFeedback.Cue.MOVE, now);
        });
        root.setPadding(Theme.MARGIN_X, Theme.MARGIN_TOP, Theme.MARGIN_X, Theme.MARGIN_BOTTOM);
        root.setClipChildren(false);

        topBar = rowLayout();
        topBar.setGravity(Gravity.CENTER_VERTICAL);
        screenTitle = Theme.oneLine(Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT));
        topBar.addView(screenTitle, lp(WRAP, WRAP));
        topTabs = rowLayout();
        topTabs.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams tl = lp(0, MATCH, 1);
        tl.leftMargin = Theme.S6 + Theme.S5;
        topBar.addView(topTabs, tl);
        topRight = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM));
        topBar.addView(topRight, lp(WRAP, WRAP));
        LinearLayout.LayoutParams bl = lp(MATCH, Theme.TOP_BAR_H);
        bl.bottomMargin = Theme.S4;
        root.addView(topBar, bl);

        body = new FrameLayout(this);
        body.setClipChildren(false);
        root.addView(body, lp(MATCH, 0, 1));

        LinearLayout bottom = rowLayout();
        bottom.setGravity(Gravity.CENTER_VERTICAL);
        bottomLeft = rowLayout();
        bottomLeft.setGravity(Gravity.CENTER_VERTICAL);
        note = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM));
        note.setGravity(Gravity.CENTER_VERTICAL);
        noteDot = new ShapeDrawable(new OvalShape());
        noteDot.setBounds(0, 0, 14, 14);
        note.setCompoundDrawables(noteDot, null, null, null);
        note.setCompoundDrawablePadding(Theme.S2);
        bottomLeft.addView(note, lp(WRAP, MATCH));
        bottom.addView(bottomLeft, lp(0, MATCH, 1));
        hints = rowLayout();
        hints.setGravity(Gravity.CENTER_VERTICAL | Gravity.END);
        LinearLayout.LayoutParams hl = lp(WRAP, MATCH);
        hl.leftMargin = Theme.S5;
        bottom.addView(hints, hl);
        LinearLayout.LayoutParams fl = lp(MATCH, Theme.FOOTER_H);
        fl.topMargin = Theme.S2;
        root.addView(bottom, fl);

        setContentView(new Widgets.Stage(this, host));
        setNote(null, NOTE_INFO);
    }

    /** The hero backdrop shows on Home and Preparing only; every other screen has the plain gradient and one glow. */
    private Bitmap heroFor(Screen s) {
        return s == Screen.HOME || s == Screen.PREPARE ? hero : null;
    }

    /** The top bar: shown with the screen's title, or hidden (Home, Preparing: the game's title is the heading). */
    private void setTopBar(boolean shown) {
        topBar.setVisibility(shown ? View.VISIBLE : View.GONE);
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

    /** The footer's left side when there is no note: the library's context line (its dot colour, 0: no dot). */
    private String contextText = "";
    private int contextDot;

    private void setContext(String text, int dot) {
        contextText = text == null ? "" : text;
        contextDot = dot;
        paintNote();
    }

    private void paintNote() {
        if (note == null)
            return;
        if (noteText.isEmpty()) {
            note.setText(contextText);
            note.setTextColor(Theme.TEXT_DIM);
            noteDot.getPaint().setColor(contextText.isEmpty() ? 0 : contextDot);
            noteDot.invalidateSelf();
            return;
        }
        int color = noteKind == NOTE_ERROR ? Theme.ERROR : noteKind == NOTE_WARN ? Theme.WARN
                    : noteKind == NOTE_OK ? Theme.OK : Theme.TEXT_DIM;
        note.setText(noteText);
        note.setTextColor(noteKind == NOTE_INFO ? Theme.TEXT_DIM : color);
        noteDot.getPaint().setColor(color);
        noteDot.invalidateSelf();
    }

    /** At most this many hints on a screen: the footer stays one discreet line. */
    static final int MAX_HINTS = 3;

    private void addHint(String glyph, int label, Runnable action) {
        if (hints.getChildCount() >= MAX_HINTS)
            throw new IllegalStateException("more than " + MAX_HINTS + " hints on " + screen);
        Widgets.Hint h = new Widgets.Hint(this, glyph, getString(label), v -> action.run());
        LinearLayout.LayoutParams p = lp(WRAP, WRAP);
        p.leftMargin = hints.getChildCount() == 0 ? 0 : Theme.S4;
        hints.addView(h, p);
    }

    /** The hints shown (tests). */
    List<Widgets.Hint> hintsForTest() {
        List<Widgets.Hint> out = new ArrayList<>();
        for (int i = 0; i < hints.getChildCount(); i++)
            out.add((Widgets.Hint)hints.getChildAt(i));
        return out;
    }

    /** A focusable the screen registers, in order: focus memory is an index in this list. */
    private <T extends View> T focusable(T v) {
        focusables.add(v);
        return v;
    }

    private MenuRow row(String title, String description, Runnable click) {
        MenuRow r = new MenuRow(this);
        r.set(title, description);
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        if (click != null)
            r.setOnClickListener(v -> click.run());
        return focusable(r);
    }

    // ---- Navigation --------------------------------------------------------------------------------------------

    private void go(Screen s) {
        if (s == Screen.SETTINGS && screen != Screen.SETTINGS) {   // Settings opens on General
            settingsTab = TAB_GENERAL;
            java.util.Arrays.fill(tabFocus, -1);
            focusMemory.remove(Screen.SETTINGS);
        }
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
        if (screen != null && !switchingTab) {
            int i = focusables.indexOf(host.findFocus());
            if (i >= 0)
                focusMemory.put(screen, i);
        }
        closeSheet(false);
        syncTheme(s);
        if (s != Screen.LIBRARY && backdrop != null)
            backdrop.setGlows(Theme.GLOW, Theme.GLOW2);   // the library's per-card glow stays there
        backdrop.setHero(heroFor(s));
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
        desc = null;
        xAction = yAction = startAction = l1Action = r1Action = null;
        topRight.setText("");
        topTabs.removeAllViews();
        setTopBar(s != Screen.HOME && s != Screen.PREPARE);
        contextText = "";
        deleteArmedAt = 0;
        storageArmedKey = null;
        cancelArmedAt = 0;
        homePrep = null;
        prepShown = null;
        prepStatus = null;
        prepMeter = null;
        prepPct = prepMsg = prepEta = prepCaption = null;
        prepProgress = null;
        homePrimary = null;
        homeStatus = null;
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
        if (companion != null && s != Screen.HOME && s != Screen.LIBRARY && s != Screen.PREPARE)
            companion.artOnly();   // the bottom screen: the game's art alone
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
        if (sheet != null)
            return sheet.rows.getChildCount() == 0 ? null : sheet.rows.getChildAt(0);
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
        if (sheet != null) {   // the sheet: B (or Y again) closes it, A and the D-pad act in it, the rest waits
            switch (k) {
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_ESCAPE:
            case KeyEvent.KEYCODE_BUTTON_Y:
                if (once) {
                    feedback.play(UiFeedback.Cue.BACK, host);
                    closeSheet(true);
                }
                return true;
            case KeyEvent.KEYCODE_BUTTON_X:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1:
                return true;
            default:
                break;
            }
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
            if (down && focused() == null && (!focusables.isEmpty() || sheet != null)) {
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

    // ---- Sheet -------------------------------------------------------------------------------------------------

    /** Opens a sheet over the screen: the screen's focus is kept for later and blocked while it is open. */
    private Widgets.Sheet openSheet(int title) {
        closeSheet(false);
        sheetReturn = focused();
        sheet = new Widgets.Sheet(this, getString(title), () -> {
            feedback.play(UiFeedback.Cue.BACK, host);
            closeSheet(true);
        });
        host.addView(sheet, host.indexOfChild(glide), new FrameLayout.LayoutParams(MATCH, MATCH));
        stageRoot.setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
        return sheet;
    }

    /** A row of the open sheet: its action runs once the sheet is closed. */
    private MenuRow sheetRow(String title, Runnable action) {
        MenuRow r = new MenuRow(this);
        r.set(title, null);
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        r.setOnClickListener(v -> {
            closeSheet(false);
            action.run();
        });
        sheet.addRow(r);
        return r;
    }

    /** After the rows are added: the first one takes the focus (as the D-pad would). */
    private void sheetReady() {
        sheet.addHint(new Widgets.Hint(this, "A", getString(R.string.HINT_SELECT), v -> clickFocused()));
        sheet.addHint(new Widgets.Hint(this, "B", getString(R.string.HINT_CLOSE), v -> {
            feedback.play(UiFeedback.Cue.BACK, host);
            closeSheet(true);
        }));
        View first = focusTarget();
        if (first != null && !first.requestFocus())
            first.requestFocusFromTouch();
    }

    /** Closes the open sheet; restore: the focus goes back where it was. */
    private void closeSheet(boolean restore) {
        if (sheet == null)
            return;
        host.removeView(sheet);
        sheet = null;
        stageRoot.setDescendantFocusability(ViewGroup.FOCUS_BEFORE_DESCENDANTS);
        View back = sheetReturn;
        sheetReturn = null;
        if (restore) {
            if (back != null && back.isAttachedToWindow()) {
                if (!back.requestFocus())
                    back.requestFocusFromTouch();
            } else {
                restoreFocus(false);
            }
        }
    }

    Widgets.Sheet sheetForTest() {
        return sheet;
    }

    // ---- The Preparing screen's idle dim (OLED burn-in) --------------------------------------------------------

    /** A near-black layer over the stage: the overall percentage and the step, small and grey, moved each minute. */
    private void buildDim() {
        dimView = new FrameLayout(this);
        dimView.setBackgroundColor(0xFF000000);
        dimView.setVisibility(View.GONE);
        LinearLayout c = column();
        c.setGravity(Gravity.CENTER_HORIZONTAL);
        dimBig = Theme.text(this, 120, Theme.REGULAR, Theme.alpha(Theme.TEXT_DIM, 0.55f), "");
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

    // ---- The list template (Settings, Manage, Storage) -----------------------------------------------------------

    /** The list's width, its focus plates included (its rows' text starts on the margin). */
    static final int LIST_W = 1072;
    /** Between the list's focus plates and the description column. */
    static final int DESC_GAP = 80;

    /** The description column of a list screen: the focused row's explanation, as plain text. */
    private LinearLayout desc;

    /**
     * The list template: a scrolling list on the left (its rows' text on the margin, their focus plates in the
     * margin), and a quiet column on the right with the focused row's description only. Returns the list.
     */
    private LinearLayout listTemplate() {
        LinearLayout h = rowLayout();
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setClipToPadding(false);
        scroll.setClipChildren(false);
        scroll.setPadding(0, 0, 0, Theme.S3);
        LinearLayout list = column();
        list.setClipChildren(false);
        scroll.addView(list, new FrameLayout.LayoutParams(MATCH, WRAP));
        LinearLayout.LayoutParams sl = lp(LIST_W, MATCH);
        sl.leftMargin = -Theme.ROW_INSET;
        h.addView(scroll, sl);
        desc = column();
        desc.setPadding(0, Theme.S3, 0, 0);
        LinearLayout.LayoutParams dl = lp(0, MATCH, 1);
        dl.leftMargin = DESC_GAP;
        h.addView(desc, dl);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        return list;
    }

    /** A list row: label (and value); its description shows in the column when it is focused. */
    private MenuRow listRow(LinearLayout list, String title, String description, Runnable click) {
        MenuRow r = row(title, description, click);
        list.addView(r, lp(MATCH, Theme.ROW_H));
        onFocus(r, () -> describe(r.description));
        return r;
    }

    /** A group label in a list (a game's name in Storage): Small, sentence case. */
    private void listLabel(LinearLayout list, String text) {
        TextView t = Theme.oneLine(Theme.label(this, text));
        t.setPadding(Theme.ROW_INSET, 0, Theme.ROW_INSET, 0);
        LinearLayout.LayoutParams p = lp(MATCH, WRAP);
        p.topMargin = list.getChildCount() == 0 ? Theme.S1 : Theme.S4;
        p.bottomMargin = Theme.S1;
        list.addView(t, p);
    }

    private void describe(String text) {
        describe(text, null, 0);
    }

    /** The description column: text, and an extra paragraph in its colour (a warning, a path), either may be null. */
    private void describe(String text, String extra, int extraColor) {
        if (desc == null)
            return;
        desc.removeAllViews();
        if (text != null && !text.isEmpty())
            desc.addView(Theme.body(this, text), lp(MATCH, WRAP));
        if (extra != null && !extra.isEmpty()) {
            TextView t = Theme.body(this, extra);
            t.setTextColor(extraColor);
            LinearLayout.LayoutParams p = lp(MATCH, WRAP);
            p.topMargin = desc.getChildCount() == 0 ? 0 : Theme.S3;
            desc.addView(t, p);
        }
    }

    /** What the description column shows (tests). */
    String descriptionForTest() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; desc != null && i < desc.getChildCount(); i++)
            b.append(i == 0 ? "" : "\n\n").append(((TextView)desc.getChildAt(i)).getText());
        return b.toString();
    }

    /** « Folder: /sdcard/xbox »: a fact line of a description. */
    private String kv(int label, String value) {
        return getString(R.string.KV_LINE, getString(label), value);
    }

    private void onFocus(View v, Runnable fill) {
        v.setOnFocusChangeListener((view, has) -> {
            if (has)
                fill.run();
        });
    }

    // ---- WELCOME -----------------------------------------------------------------------------------------------

    /** First run: a welcome, what it takes in three plain steps, and one button. */
    private void buildWelcome() {
        screenTitle.setText(R.string.WELCOME_TITLE);
        LinearLayout h = rowLayout();
        LinearLayout left = column();
        left.setGravity(Gravity.CENTER_VERTICAL);
        left.addView(Theme.text(this, Theme.T_DISPLAY, Theme.MEDIUM, Theme.TEXT, getString(R.string.WELCOME_HERO)),
                     lp(MATCH, WRAP));
        TextView t = Theme.body(this, getString(R.string.WELCOME_TEXT));
        LinearLayout.LayoutParams tl = lp(MATCH, WRAP);
        tl.topMargin = Theme.S3;
        left.addView(t, tl);
        LinearLayout buttons = rowLayout();
        LinearLayout.LayoutParams bl = lp(MATCH, Theme.BUTTON_H);
        bl.topMargin = Theme.S5;
        left.addView(buttons, bl);
        defaultFocus = pill(buttons, Icon.ARROW, getString(R.string.WELCOME_START), true, () -> go(Screen.SOURCE));
        h.addView(left, lp(0, MATCH, 1));

        LinearLayout steps = column();
        steps.setGravity(Gravity.CENTER_VERTICAL);
        int[][] items = { { R.string.STEP1, R.string.STEP1_SUB }, { R.string.STEP2, R.string.STEP2_SUB },
                          { R.string.STEP3, R.string.STEP3_SUB } };
        for (int i = 0; i < items.length; i++) {
            TextView n = Theme.text(this, Theme.T_ROW, Theme.MEDIUM, Theme.TEXT,
                                    (i + 1) + "   " + getString(items[i][0]));
            LinearLayout.LayoutParams nl = lp(MATCH, WRAP);
            nl.topMargin = i == 0 ? 0 : Theme.S5;
            steps.addView(n, nl);
            TextView d = Theme.body(this, getString(items[i][1]));
            LinearLayout.LayoutParams dl = lp(MATCH, WRAP);
            dl.topMargin = Theme.S1;
            steps.addView(d, dl);
        }
        LinearLayout.LayoutParams sl = lp(Theme.DESC_W + Theme.S5, MATCH);
        sl.leftMargin = Theme.S6;
        h.addView(steps, sl);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("A", R.string.HINT_START, () -> go(Screen.SOURCE));
        addHint("B", R.string.HINT_QUIT, this::back);
    }

    // ---- SOURCE and FOLDER_ACCESS --------------------------------------------------------------------------------

    /** A choice of the list template: its label and a short value, its explanation in the description column. */
    private MenuRow choice(LinearLayout list, int title, String description, String value, Runnable click) {
        MenuRow r = listRow(list, getString(title), description, click);
        if (value != null)
            r.setValue(value);
        return r;
    }

    private void buildSource() {
        screenTitle.setText(R.string.SOURCE_TITLE);
        topRight.setText(getString(R.string.LIB_ST_FREE, size(freeSpace())));
        LinearLayout list = listTemplate();
        boolean all = Environment.isExternalStorageManager();
        MenuRow folder = choice(list, R.string.CARD_FOLDER, getString(R.string.CARD_FOLDER_SUB),
                                getString(all ? R.string.BADGE_IN_PLACE : R.string.BADGE_FOLDER_ASK),
                                this::onChooseFolder);
        choice(list, R.string.CARD_ISO, getString(R.string.CARD_ISO_SUB), null, this::openIsoPicker);
        String libPath = Library.folderPath(this);
        choice(list, R.string.LIB_CARD, getString(R.string.LIB_CARD_SUB),
               Library.folderTree(this) == null ? null : libPath != null ? new File(libPath).getName()
                                                                         : getString(R.string.LIB_BADGE_SET),
               this::onChooseLibraryFolder);
        // The intro under the focused choice's own text: what any choice keeps.
        for (int i = 0; i < list.getChildCount(); i++) {
            MenuRow r = (MenuRow)list.getChildAt(i);
            onFocus(r, () -> describe(r.description, getString(R.string.SOURCE_TEXT), Theme.TEXT_DIM));
        }
        defaultFocus = folder;
        describe(folder.description, getString(R.string.SOURCE_TEXT), Theme.TEXT_DIM);
        addHint("A", R.string.HINT_CHOOSE, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    private void buildFolderAccess() {
        screenTitle.setText(R.string.ALL_FILES_TITLE);
        LinearLayout list = listTemplate();
        String why = getString(pickLibrary ? R.string.LIB_ALL_FILES_EXPLAIN : R.string.ALL_FILES_EXPLAIN);
        MenuRow grant = choice(list, R.string.ALL_FILES_GRANT, getString(R.string.ALL_FILES_GRANT_SUB),
                               getString(R.string.BADGE_RECOMMENDED), this::askAllFilesAccess);
        if (pickLibrary)
            choice(list, R.string.LIB_SAF, getString(R.string.LIB_SAF_SUB), null, this::openTreePicker);
        else
            choice(list, R.string.ALL_FILES_COPY, getString(R.string.ALL_FILES_COPY_SUB), null, this::openTreePicker);
        for (int i = 0; i < list.getChildCount(); i++) {
            MenuRow r = (MenuRow)list.getChildAt(i);
            onFocus(r, () -> describe(r.description, why, Theme.TEXT_DIM));
        }
        defaultFocus = grant;
        describe(grant.description, why, Theme.TEXT_DIM);
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

    /** The import: a plain ring on the left, what is copied and how much on the right; B cancels (nothing to focus). */
    private void buildImport() {
        screenTitle.setText(R.string.IMPORT_TITLE);
        LinearLayout h = rowLayout();
        h.setGravity(Gravity.CENTER_VERTICAL);
        ring = new Widgets.Ring(this);
        h.addView(ring, lp(560, 560));

        LinearLayout c = column();
        importHead = Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, getString(importLabel));
        c.addView(importHead, lp(MATCH, WRAP));
        importBytes = Theme.text(this, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT, "");
        LinearLayout.LayoutParams bl = lp(MATCH, WRAP);
        bl.topMargin = Theme.S3;
        c.addView(importBytes, bl);
        importFile = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM, "");
        importFile.setSingleLine(true);
        importFile.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        LinearLayout.LayoutParams fl = lp(MATCH, WRAP);
        fl.topMargin = Theme.S2;
        c.addView(importFile, fl);
        TextView free = Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM,
                                   getString(R.string.FREE_SPACE_APP, size(freeSpace())));
        LinearLayout.LayoutParams frl = lp(MATCH, WRAP);
        frl.topMargin = Theme.S2;
        c.addView(free, frl);
        TextView keeps = Theme.body(this, getString(R.string.IMPORT_KEEPS));
        LinearLayout.LayoutParams kl = lp(MATCH, WRAP);
        kl.topMargin = Theme.S5;
        c.addView(keeps, kl);
        LinearLayout.LayoutParams cl = lp(0, WRAP, 1);
        cl.leftMargin = Theme.S6;
        h.addView(c, cl);
        body.addView(h, new FrameLayout.LayoutParams(MATCH, MATCH));
        addHint("B", R.string.CANCEL, this::requestCancel);
        ring.set(-1, "", getString(R.string.IMPORT_STARTING), "");
    }

    private void requestCancel() {
        if (worker == null || !worker.isAlive() || cancelRequested)
            return;
        cancelRequested = true;
        setNote(getString(R.string.CANCELING), NOTE_INFO);
    }

    // ---- HOME --------------------------------------------------------------------------------------------------

    /**
     * The Home layout, shared by Home, a library game's Home and Preparing: the art tile on the left, and on the right
     * the game's title (Display) over a column the screen fills (returned). No top bar: the title is the heading.
     */
    private LinearLayout hero(String title, boolean empty) {
        LinearLayout hero = rowLayout();
        hero.setGravity(Gravity.CENTER_VERTICAL);
        Widgets.ArtTile tile = new Widgets.ArtTile(this);
        if (empty)
            tile.setEmpty();
        else if (art != null)
            tile.setArt(art, artColor);
        else
            tile.setMonogram(TitleArt.initials(title), artColor);
        hero.addView(tile, lp(Theme.ART, Theme.ART));
        LinearLayout info = column();
        TextView big = Theme.text(this, Theme.T_DISPLAY, Theme.MEDIUM, Theme.TEXT, title);
        big.setMaxLines(2);
        big.setEllipsize(TextUtils.TruncateAt.END);
        big.setLineSpacing(0, 1.05f);
        info.addView(big, lp(MATCH, WRAP));
        LinearLayout.LayoutParams il = lp(0, WRAP, 1);
        il.leftMargin = Theme.S5;
        hero.addView(info, il);
        body.addView(hero, new FrameLayout.LayoutParams(MATCH, MATCH));
        return info;
    }

    /** The line under the title: a status (a dot in its colour) when dot is not 0, else quiet text. */
    private View heroLine(LinearLayout info, String text, int dot) {
        View v;
        if (dot != 0) {
            Widgets.Chip c = new Widgets.Chip(this);
            c.set(text, dot);
            v = c;
        } else {
            TextView t = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, text);
            t.setMaxLines(2);
            t.setEllipsize(TextUtils.TruncateAt.END);
            t.setLineSpacing(0, 1.2f);
            v = t;
        }
        LinearLayout.LayoutParams p = lp(dot != 0 ? WRAP : MATCH, WRAP);
        p.topMargin = Theme.S2;
        info.addView(v, p);
        return v;
    }

    /** The row of buttons under the title: one solid primary and quiet pills. */
    private LinearLayout heroButtons(LinearLayout info) {
        LinearLayout row = rowLayout();
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setClipChildren(false);
        LinearLayout.LayoutParams p = lp(MATCH, Theme.BUTTON_H);
        p.topMargin = Theme.S5;
        info.addView(row, p);
        return row;
    }

    /** The primary button is at least this wide (Play, Prepare). */
    static final int PRIMARY_MIN_W = 300;

    private Widgets.PillButton pill(LinearLayout row, Icon icon, String label, boolean primary, Runnable click) {
        Widgets.PillButton b = new Widgets.PillButton(this);
        b.set(icon, label, primary);
        b.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, b);
        if (click != null)
            b.setOnClickListener(v -> click.run());
        LinearLayout.LayoutParams p = lp(primary ? Math.max(PRIMARY_MIN_W, b.naturalWidth()) : WRAP, Theme.BUTTON_H);
        p.leftMargin = row.getChildCount() == 0 ? 0 : Theme.S2;
        row.addView(b, p);
        return focusable(b);
    }

    /** Home's quiet pills: Settings, Controls, Manage. */
    private void homePills(LinearLayout row) {
        pill(row, Icon.SLIDERS, getString(R.string.SETTINGS), false, () -> go(Screen.SETTINGS));
        pill(row, Icon.PAD, getString(R.string.CONTROLS), false, () -> go(Screen.CONTROLS));
        pill(row, Icon.DISC, getString(R.string.HOME_MANAGE), false, () -> go(Screen.GAME_DATA));
    }

    /** « Yesterday · 12 h 40 min played », or « Not played yet ». */
    private String homeMeta(long last, long playMs) {
        if (last <= 0 && playMs <= 0)
            return getString(R.string.HOME_META_NEW);
        return getString(R.string.HOME_META, lastPlayedText(last, System.currentTimeMillis()), playTimeText(playMs));
    }

    /**
     * Home: the game's art and title, one line (when it was played, or its state when it is not ready), and one row of
     * buttons: a solid Play (or Prepare, Choose…) and three quiet pills (Settings, Controls, Manage). Start plays too.
     */
    private void buildHome() {
        State st = state();
        if (st == State.LIBRARY) {
            buildHomeNotHere();
            return;
        }
        String dir = gameDir();
        boolean running = gameRunning();
        Prep prep = prep();
        PrepareProtocol.Status ps = prep == Prep.OFF || prep == Prep.PLAYABLE ? null : readPrepStatus();
        boolean preparing = prep != Prep.OFF && prep != Prep.PLAYABLE;
        checkHashLater(dir);
        sizeLater(dir);

        String title = st == State.NONE ? getString(R.string.NO_GAME_HEAD) : gameTitle(dir);
        LinearLayout info = hero(title, st == State.NONE);
        String tid = st == State.NONE ? null : titleId();

        // The one line: the state when it is not « Ready », else when it was last played.
        String statusText = null;
        int statusColor = Theme.OK;
        if (st == State.READY && preparing) {
            switch (prep) {
            case RUNNING:
                statusText = ps == null ? getString(R.string.PREPARE_STARTING) : progressLine(ps);
                break;
            case QUEUED:
                statusText = queuedLabel(tid);
                statusColor = Theme.ACCENT;
                break;
            case FAILED:
                statusText = getString(R.string.CHIP_PREPARE_FAILED)
                             + (ps == null || ps.code.isEmpty() ? "" : " · " + ps.code);
                statusColor = Theme.ERROR;
                break;
            default:
                statusText = getString(R.string.HOME_NEEDS_PREPARE, getString(R.string.CHIP_NEEDS_PREPARE),
                                       estimate());
                statusColor = Theme.WARN;
                break;
            }
            View line = heroLine(info, statusText, statusColor);
            if (prep == Prep.RUNNING)
                homeStatus = (Widgets.Chip)line;
        } else if (st == State.READY) {
            Library.Record rec = rec();
            heroLine(info, homeMeta(rec != null ? rec.lastPlayed : GameStore.lastPlayed(this),
                                    rec != null ? rec.playMs : GameStore.playTime(this)), 0);
        } else {
            statusText = getString(stateLabel(st));
            statusColor = st == State.NONE ? Theme.TEXT_DIM : stateColor(st);
            if (st != State.NONE)
                heroLine(info, statusText, statusColor);
            heroLine(info, stateText(st, dir), 0);
        }

        LinearLayout buttons = heroButtons(info);
        Widgets.PillButton play;
        if (st == State.NONE || st == State.MISSING || st == State.NO_ACCESS) {
            play = pill(buttons, null, getString(st == State.NONE ? R.string.CHOOSE_GAME : R.string.CHOOSE_AGAIN),
                        true, () -> go(Screen.SOURCE));
        } else if (preparing && prep == Prep.QUEUED) {
            play = pill(buttons, null, queuedLabel(tid), true, this::onPlay);
            homePrep = prep;
        } else if (preparing) {
            int t = prep == Prep.RUNNING ? R.string.PREPARING : prep == Prep.FAILED ? R.string.PREPARE_SEE_WHY
                    : R.string.PREPARE;
            play = pill(buttons, null, getString(t), true, this::onPlay);
            homePrep = prep;
        } else {
            play = pill(buttons, Icon.PLAY, getString(R.string.PLAY), true, this::onPlay);
            play.setDim(st != State.READY);
        }
        homePrimary = play;
        homePills(buttons);
        if (companion != null) {
            companion.art(st == State.NONE ? null : art);
            companion.home(st == State.NONE ? "" : title,
                           statusText != null ? statusText : getString(R.string.STATE_READY),
                           statusText != null ? statusColor : Theme.OK, !play.dim, play.title);
        }

        defaultFocus = play;
        startAction = this::onPlay;
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", libraryOn() && history.isEmpty() ? R.string.LIB_TITLE : history.isEmpty() ? R.string.HINT_QUIT
                                                                                                : R.string.HINT_BACK,
                this::back);

        if (preparing && prep == Prep.INTERRUPTED)
            setNote(getString(R.string.PREPARE_INTERRUPTED), NOTE_WARN);
        else if (running && selectedIsLaunched())
            setNote(getString(R.string.GAME_RUNNING_HOME), NOTE_INFO);
        else if (running && st == State.READY)
            setNote(getString(R.string.LIB_OTHER_RUNNING, runningTitle()), NOTE_INFO);
        else if (st == State.READY && !preparing && (rec() != null ? rec().lastPlayed : GameStore.lastPlayed(this)) <= 0)
            setNote(getString(R.string.HOME_LEAD_READY), NOTE_INFO);   // before the first play: where the menu is
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

    // ---- SETTINGS ----------------------------------------------------------------------------------------------

    /** Settings' tabs: General (the launcher, the bottom screen, the game theme, the language), Graphics, Audio, Advanced. */
    static final int TAB_GENERAL = 0, TAB_GRAPHICS = 1, TAB_AUDIO = 2, TAB_ADVANCED = 3, TABS = 4;
    private int settingsTab;
    /** The focus of each tab when it was left (an index in its focusables; -1: its first row). */
    private final int[] tabFocus = { -1, -1, -1, -1 };
    /** True while a tab switch rebuilds Settings: show() does not overwrite the new tab's focus memory. */
    private boolean switchingTab;
    /** The option rows of the tab shown. */
    private final java.util.Map<RecompConfig.Option, MenuRow> optionRows = new java.util.LinkedHashMap<>();

    int settingsTabForTest() {
        return settingsTab;
    }

    private static RecompConfig.Section tabSection(int tab) {
        switch (tab) {
        case TAB_GRAPHICS:
            return RecompConfig.Section.GRAPHICS;
        case TAB_AUDIO:
            return RecompConfig.Section.AUDIO;
        case TAB_ADVANCED:
            return RecompConfig.Section.ADVANCED;
        default:
            return RecompConfig.Section.GAME;
        }
    }

    private static final int[] TAB_LABELS = { R.string.SECTION_GENERAL, R.string.SECTION_GRAPHICS,
                                               R.string.SECTION_AUDIO, R.string.SECTION_ADVANCED };

    /**
     * Settings: four tabs (L1 / R1), each a short list of « label  value » rows (left / right change a value, A steps
     * it), the focused row's description on the right. No tab needs scrolling on a 1080p stage. Y puts the options
     * back to their defaults; « Reset all » and About end the Advanced tab.
     */
    private void buildSettings() {
        screenTitle.setText(R.string.SETTINGS);
        cfg = RecompConfig.load(GameStore.externalFilesDir(this));
        optionRows.clear();
        String[] names = new String[TABS];
        for (int i = 0; i < TABS; i++)
            names[i] = getString(TAB_LABELS[i]);
        topTabs.addView(new Widgets.Tabs(this, names, settingsTab, i -> switchTab(i - settingsTab)),
                        lp(WRAP, MATCH));
        LinearLayout list = listTemplate();

        List<RecompConfig.Option> shown = RecompConfig.shownOptions(lsfgBuilt());
        lsfgOff = lsfgForcedOff();
        if (settingsTab == TAB_GENERAL) {
            launcherToggle(list, R.string.OPEN_DIRECTLY, R.string.OPEN_DIRECTLY_DETAIL, GameStore::autoStart,
                           GameStore::setAutoStart, R.string.OPEN_DIRECTLY_ON, R.string.OPEN_DIRECTLY_OFF);
            launcherToggle(list, R.string.UI_SOUND, R.string.UI_SOUND_DETAIL, GameStore::uiSound,
                           GameStore::setUiSound, R.string.UI_SOUND_ON, R.string.UI_SOUND_OFF);
            launcherToggle(list, R.string.UI_HAPTICS, R.string.UI_HAPTICS_DETAIL, GameStore::uiHaptics,
                           GameStore::setUiHaptics, R.string.UI_HAPTICS_ON, R.string.UI_HAPTICS_OFF);
            if (DualScreen.available(this, CompanionScreen.ownDisplayId(this)))
                bottomScreenRow(list);
            String dir = gameDir();
            if (dir != null && state() != State.NONE && GameStore.findXbe(new File(dir)) != null && titleId() != null)
                gameThemeRow(list, titleId(), gameTitle(dir));
        }
        RecompConfig.Section section = tabSection(settingsTab);
        for (RecompConfig.Option o : shown) {
            if (o.section != section)
                continue;
            MenuRow r = listRow(list, getString(o.title), getString(o.detail), null);
            r.onPress = null;   // a press steps the value: the « move » tick, not « select »
            String[] labels = new String[o.choices.length + 1];
            for (int i = 0; i < o.choices.length; i++)
                labels[i] = getString(o.choices[i].label);
            labels[o.choices.length] = getString(R.string.VAL_CUSTOM);
            r.setChoices(labels);
            optionRows.put(o, r);
            refreshOptionRow(r, o);
            r.cycler = d -> stepOption(r, o, d);
            r.setOnClickListener(v -> stepOption(r, o, 1));
            onFocus(r, () -> optionDetail(o));
            if (o.needsLsfg && lsfgOff) {
                // The game's pack forces it off (pack.json env_force RECOMP_LSFG=0): the setting stays, for the
                // other games, but is dimmed and its description says it does nothing for this one.
                r.setDim(true);
                r.setDescription(getString(R.string.LSFG_TITLE_UNAVAILABLE));
            }
            if (o.needsLsfg)
                lsfgDllRow(list);
        }
        refreshRendererDims();
        if (settingsTab == TAB_ADVANCED) {
            MenuRow reset = listRow(list, getString(R.string.RESET), getString(R.string.RESET_DETAIL),
                                    this::resetSettings);
            onFocus(reset, () -> describe(getString(R.string.RESET_DETAIL),
                                          getString(R.string.SETTINGS_FOOTER, cfg.file().getAbsolutePath()),
                                          Theme.TEXT_DIM));
            MenuRow about = listRow(list, getString(R.string.ABOUT), getString(R.string.ABOUT_TEXT),
                                    () -> go(Screen.ABOUT));
            about.setValue(version());
        }

        l1Action = () -> switchTab(-1);
        r1Action = () -> switchTab(1);
        yAction = this::resetSettings;
        addHint("A", R.string.HINT_CHANGE, this::clickFocused);
        addHint("Y", R.string.HINT_DEFAULTS, this::resetSettings);
        addHint("B", R.string.HINT_BACK, this::back);
        if (gameRunning())
            setNote(getString(R.string.SETTINGS_GAME_RUNNING), NOTE_INFO);
    }

    /** L1 / R1 (and a tap on a tab): the previous or next tab, wrapping; each tab keeps its own focus. */
    private void switchTab(int dir) {
        if (screen != Screen.SETTINGS || dir == 0)
            return;
        tabFocus[settingsTab] = focusables.indexOf(focused());
        settingsTab = ((settingsTab + dir) % TABS + TABS) % TABS;
        int f = tabFocus[settingsTab];
        if (f >= 0)
            focusMemory.put(Screen.SETTINGS, f);
        else
            focusMemory.remove(Screen.SETTINGS);
        feedback.play(UiFeedback.Cue.MOVE, host);
        switchingTab = true;
        show(Screen.SETTINGS, dir > 0 ? 1 : -1);
        switchingTab = false;
    }

    /** Frame cap and Resolution only apply to the GPU renderer: dimmed (still changeable) otherwise. */
    private void refreshRendererDims() {
        boolean gpu = false;
        for (RecompConfig.Option o : RecompConfig.OPTIONS) {
            if (o.title == R.string.OPT_GPU) {
                int i = cfg.choiceIndex(o);
                gpu = i >= 0 && o.choices[i].label == R.string.VAL_GPU_GL;
            }
        }
        for (java.util.Map.Entry<RecompConfig.Option, MenuRow> e : optionRows.entrySet()) {
            int t = e.getKey().title;
            if (t == R.string.OPT_GPU_FPS_CAP || t == R.string.OPT_GPU_SCALE)
                e.getValue().setDim(!gpu);
        }
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
        MenuRow r = settingRow(list, getString(R.string.BOTTOM_SCREEN), getString(R.string.BOTTOM_SCREEN_DETAIL));
        r.setChoices(names);
        Runnable refresh = () -> r.setValue(names[GameStore.bottomScreen(this).ordinal()]);
        refresh.run();
        r.cycler = d -> {
            int n = modes.length, i = ((GameStore.bottomScreen(this).ordinal() + d) % n + n) % n;
            GameStore.setBottomScreen(this, modes[i]);
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            if (companion != null)
                companion.screen().refresh();
            setNote(getString(R.string.BOTTOM_SCREEN_SAVED, names[i]), NOTE_OK);
            Log.i(TAG, "launcher: bottom screen " + modes[i]);
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    /**
     * Settings › General › Game theme, per title: « Art and colours », « Colours only », « Off ». Applies at once
     * here (the stage is built again in the new colours), in the game's menu at its next start.
     */
    private void gameThemeRow(LinearLayout list, String tid, String title) {
        GameTheme.Mode[] modes = GameTheme.Mode.values();
        int[] labels = { R.string.VAL_THEME_FULL, R.string.VAL_THEME_COLORS, R.string.VAL_THEME_OFF };
        String[] names = new String[modes.length];
        for (int i = 0; i < modes.length; i++)
            names[i] = getString(labels[i]);
        MenuRow r = settingRow(list, getString(R.string.GAME_THEME), getString(R.string.GAME_THEME_DETAIL, title));
        r.setChoices(names);
        r.setValue(names[GameTheme.mode(this, tid).ordinal()]);
        r.cycler = d -> {
            int n = modes.length, i = ((GameTheme.mode(this, tid).ordinal() + d) % n + n) % n;
            GameTheme.setMode(this, tid, modes[i]);
            feedback.play(UiFeedback.Cue.MOVE, r);
            Log.i(TAG, "launcher: game theme " + tid + " " + modes[i].key);
            show(Screen.SETTINGS, 0);   // syncTheme: the new colours, the focus stays on this row
            setNote(getString(R.string.GAME_THEME_SAVED, names[i]), NOTE_OK);
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    /** A setting row: its press steps the value, which plays the « move » tick instead of « select ». */
    private MenuRow settingRow(LinearLayout list, String title, String description) {
        MenuRow r = listRow(list, title, description, null);
        r.onPress = null;
        return r;
    }

    /** An on / off launcher preference as a setting row. */
    private void launcherToggle(LinearLayout list, int title, int detailText,
                                Function<android.content.Context, Boolean> get,
                                BiFunction<android.content.Context, Boolean, Boolean> set, int onNote, int offNote) {
        MenuRow r = settingRow(list, getString(title), getString(detailText));
        r.setChoices(new String[] { getString(R.string.VAL_ON), getString(R.string.VAL_OFF) });
        Runnable refresh = () -> r.setValue(getString(get.apply(this) ? R.string.VAL_ON : R.string.VAL_OFF));
        refresh.run();
        r.cycler = d -> {
            set.apply(this, !get.apply(this));
            refresh.run();
            feedback.play(UiFeedback.Cue.MOVE, r);
            setNote(getString(get.apply(this) ? onNote : offNote), NOTE_OK);
            Log.i(TAG, "launcher: " + getResources().getResourceEntryName(title) + " "
                       + (get.apply(this) ? "on" : "off"));
        };
        r.setOnClickListener(v -> r.cycler.step(1));
    }

    private String choiceLabel(RecompConfig c, RecompConfig.Option o) {
        int i = c.choiceIndex(o);
        return i < 0 ? getString(R.string.VAL_CUSTOM) : getString(o.choices[i].label);
    }

    private void refreshOptionRow(MenuRow r, RecompConfig.Option o) {
        r.setValue(choiceLabel(cfg, o));
    }

    private void stepOption(MenuRow r, RecompConfig.Option o, int dir) {
        int n = o.choices.length, i = cfg.choiceIndex(o);
        int next = i < 0 ? o.defaultIndex() : ((i + dir) % n + n) % n;
        cfg.set(o, o.choices[next]);
        saveSettings();
        refreshOptionRow(r, o);
        if (o.title == R.string.OPT_GPU)
            refreshRendererDims();
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

    /** Y, or « Reset all »: every option back to its default (the launcher's own preferences stay). */
    private void resetSettings() {
        cfg.resetAll();
        saveSettings();
        for (java.util.Map.Entry<RecompConfig.Option, MenuRow> e : optionRows.entrySet())
            refreshOptionRow(e.getValue(), e.getKey());
        refreshRendererDims();
        setNote(getString(R.string.RESET_DONE), NOTE_OK);
    }

    /** The focused option's description; frame generation forced off for this game says so, in WARN. */
    private void optionDetail(RecompConfig.Option o) {
        boolean off = o.needsLsfg && lsfgOff;
        describe(getString(o.detail), off ? getString(R.string.LSFG_TITLE_UNAVAILABLE) + ". "
                                            + getString(R.string.LSFG_TITLE_UNAVAILABLE_DETAIL) : null, Theme.WARN);
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
     * the row's value. kind gets a NOTE_* colour.
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

    /** « Choose Lossless.dll », under the frame generation option: its state as the value. */
    private void lsfgDllRow(LinearLayout list) {
        int[] kind = new int[1];
        String status = lsfgStatus(kind);
        MenuRow r = listRow(list, getString(R.string.LSFG_CHOOSE_DLL), getString(R.string.LSFG_CHOOSE_DLL_DETAIL),
                            this::openLsfgDllPicker);
        r.setValue(status);
        onFocus(r, () -> describe(getString(R.string.LSFG_CHOOSE_DLL_DETAIL), getString(R.string.LSFG_LEGAL_FOOTER),
                                  Theme.TEXT_DIM));
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

    /**
     * Manage (Home's third pill): the library (or Change game), deleting the imported copy, and the game's other
     * stored things, each with its size as the value. The technical facts (title ID, pack, folder) are the first
     * row's description.
     */
    private void buildGameData() {
        screenTitle.setText(R.string.HOME_MANAGE);
        String dir = gameDir();
        sizeLater(dir);
        LinearLayout list = listTemplate();
        boolean lib = libraryOn();
        String facts = manageFacts(dir);
        MenuRow change = lib ? listRow(list, getString(R.string.LIB_TITLE), null, this::openLibrary)
                             : listRow(list, getString(dir == null ? R.string.CHOOSE_GAME : R.string.CHANGE_GAME),
                                       getString(R.string.CHANGE_GAME_DETAIL), () -> go(Screen.SOURCE));
        onFocus(change, () -> describe(change.description, facts, Theme.TEXT_DIM));
        defaultFocus = change;

        if (dir != null && ownedCopy(dir)) {
            MenuRow del = listRow(list, getString(R.string.DELETE_COPY), getString(R.string.DELETE_COPY_DETAIL, dir),
                                  null);
            del.setValue(sizeText(dir));
            del.setOnClickListener(v -> onDelete(del, dir));
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
                storageRow(list, it);
            storageMeasureLater(items);
        }
        describe(change.description, facts, Theme.TEXT_DIM);
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", R.string.HINT_BACK, this::back);
    }

    /** The game's technical facts, one per line: title ID, when it was prepared and its pack's size, its files. */
    private String manageFacts(String dir) {
        List<String> l = new ArrayList<>();
        String tid = titleId();
        if (tid != null)
            l.add(kv(R.string.KV_TITLE_ID, tid));
        if (tid != null && Pack.check(getFilesDir(), tid) == Pack.Check.VALID) {
            String[] pack = packFacts(tid);
            l.add(kv(R.string.KV_PREPARED, pack[0]));
            l.add(kv(R.string.KV_PACK_SIZE, pack[1]));
        }
        if (dir == null) {
            l.add(kv(R.string.KV_STATUS, getString(stateLabel(state()))));
        } else {
            l.add(kv(R.string.KV_FOLDER, dir));
            l.add(kv(R.string.KV_SOURCE, sourceLabel(dir)));
            l.add(kv(R.string.KV_SIZE, sizeText(dir)));
        }
        l.add(kv(R.string.KV_FREE, size(freeSpace())));
        return TextUtils.join("\n", l);
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

    /** The progress bar's width on the Preparing screen. */
    static final int PREPARE_BAR_W = 960;

    /**
     * Preparing: Home's layout with the buttons replaced by the progress (so nothing jumps): the title, « Preparing ·
     * step 5 of 6 · Compile », one bar, the percentage and the time left, and that the app can be left. Nothing to
     * focus while it runs: Y hides it, B cancels (twice). Failed or stopped: the status line says so, the reason
     * under it, and Retry (or Resume) and Share log as buttons.
     */
    private void buildPrepare() {
        String dir = gameDir();
        String title = gameTitle(dir);
        LinearLayout info = hero(title, false);
        if (companion != null)
            companion.preparing(titleId(), title);
        prepStatus = (Widgets.Chip)heroLine(info, getString(R.string.PREPARE_STARTING), Theme.OK);

        LinearLayout progress = column();
        prepMeter = new Widgets.Meter(this);
        progress.addView(prepMeter, lp(MATCH, 12));
        LinearLayout nums = rowLayout();
        nums.setGravity(Gravity.CENTER_VERTICAL);
        prepPct = Theme.oneLine(Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, ""));
        nums.addView(prepPct, lp(0, WRAP, 1));
        prepEta = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, ""));
        nums.addView(prepEta, lp(WRAP, WRAP));
        LinearLayout.LayoutParams nl = lp(MATCH, WRAP);
        nl.topMargin = Theme.S3;
        progress.addView(nums, nl);
        LinearLayout.LayoutParams pl = lp(PREPARE_BAR_W, WRAP);
        pl.topMargin = Theme.S5;
        info.addView(progress, pl);
        prepProgress = progress;

        prepCaption = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, getString(R.string.PREPARE_KEEPS));
        LinearLayout.LayoutParams cl = lp(PREPARE_BAR_W, WRAP);
        cl.topMargin = Theme.S5;
        info.addView(prepCaption, cl);
        prepMsg = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT, "");
        prepMsg.setMaxLines(3);
        prepMsg.setEllipsize(TextUtils.TruncateAt.END);
        prepMsg.setLineSpacing(0, 1.2f);
        LinearLayout.LayoutParams ml = lp(PREPARE_BAR_W, WRAP);
        ml.topMargin = Theme.S3;
        info.addView(prepMsg, ml);

        PrepareProtocol.Status s = readPrepStatus();
        boolean running = s == null || statusAlive(s);
        if (running) {
            prepMsg.setVisibility(View.GONE);
            yAction = this::leavePrepare;
            addHint("Y", R.string.HINT_HIDE, this::leavePrepare);
            addHint("B", R.string.CANCEL, this::armCancelPrepare);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            progress.setVisibility(View.GONE);
            prepCaption.setVisibility(View.GONE);
            boolean cancelled = PrepareProtocol.CANCELLED.equals(s.state) || PrepareProtocol.RUNNING.equals(s.state);
            LinearLayout buttons = heroButtons(info);
            Widgets.PillButton retry = pill(buttons, Icon.RESET,
                                            getString(cancelled ? R.string.PREPARE_RESUME : R.string.PREPARE_RETRY),
                                            true, this::startPrepare);
            pill(buttons, Icon.COPY, getString(R.string.SHARE_LOG), false, this::shareLog);
            defaultFocus = retry;
            yAction = this::shareLog;
            addHint("A", R.string.HINT_SELECT, this::clickFocused);
            addHint("Y", R.string.SHARE_LOG, this::shareLog);
            addHint("B", R.string.HINT_BACK, this::back);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
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
        if (prepStatus == null)
            return;
        boolean running = s == null || statusAlive(s);
        boolean failed = s != null && !running && PrepareProtocol.FAILED.equals(s.state);
        boolean stopped = s != null && !running && !failed;
        if (failed) {
            prepStatus.set(getString(R.string.PREPARE_FAILED_HEAD) + (s.code.isEmpty() ? "" : " · " + s.code),
                           Theme.ERROR);
            String why = s.msg;
            if (PrepareRunner.KILLED.equals(s.code))
                why = getString(R.string.PREPARE_HINT_KILLED);
            else if ("NO_SPACE".equals(s.code))
                why = getString(R.string.PREPARE_HINT_NO_SPACE, size(internalFree()))
                      + (why.isEmpty() ? "" : " " + why);
            prepMsg.setText(why);
        } else if (stopped) {
            prepStatus.set(getString(R.string.PREPARE_CANCELLED_HEAD), Theme.WARN);
            prepMsg.setText(getString(PrepareProtocol.RUNNING.equals(s.state) ? R.string.PREPARE_INTERRUPTED
                                                                              : R.string.PREPARE_CANCELLED_TEXT));
        } else {
            String step = s == null ? getString(R.string.PREPARE_STARTING) : PrepareService.stepName(this, s.id);
            prepStatus.set(s != null && s.step > 0 ? getString(R.string.PREPARE_STATUS, s.step, s.n, step)
                                                   : getString(R.string.PREPARE_STATUS_SHORT, step), Theme.OK);
            float frac = s == null ? -1 : s.step > 0 ? PrepareProtocol.overall(s.step, s.n, s.pct) : -1;
            prepMeter.set(Math.max(0, frac), Theme.ACCENT);
            prepPct.setText(frac >= 0 ? String.format(Locale.getDefault(), "%d %%", Math.round(frac * 100)) : "");
            prepEta.setText(s != null && s.eta >= 0 ? getString(R.string.PREPARE_ETA_BIG,
                                                                PrepareProtocol.formatEta(s.eta))
                                                    : getString(R.string.PREPARE_ETA_UNKNOWN));
        }
        if (dimmed)
            paintDim();
    }

    /** Twice within CONFIRM_MS: the first press arms, the second sends ACTION_CANCEL to the service. */
    private void armCancelPrepare() {
        if (cancelSent)
            return;
        long now = SystemClock.uptimeMillis();
        if (cancelArmedAt == 0 || now - cancelArmedAt > CONFIRM_MS) {
            cancelArmedAt = now;
            setNote(getString(R.string.PREPARE_CANCEL_CONFIRM), NOTE_WARN);
            return;
        }
        cancelArmedAt = 0;
        cancelSent = true;
        String tid = titleId();
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
                if (s != null && homeStatus != null)
                    homeStatus.set(progressLine(s), Theme.OK);
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

    /** Under the grid: the scan's progress, an empty library's hint, a scan cut short, the « Other files » line. */
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
        if (index.truncated)
            parts.add(getString(R.string.LIB_TRUNCATED));
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
     * The library: a full-width grid of games (five columns), the focused game's state and play time as one context
     * line at the bottom left, and the rest (Rescan, Storage, the library folder, Settings, About) in the Options
     * sheet on Y. An empty library shows why and a « Library folder » button. Stock colours; the backdrop's glow
     * follows the focused card (its cached theme).
     */
    private void buildLibrary() {
        screenTitle.setText(R.string.LIB_TITLE);
        libEntries = LibraryScreen.entries(getFilesDir(), games, index, libEnv());
        Library.Record sel = games.selected();
        String want = focusTid != null ? focusTid : sel != null ? sel.titleId : null;
        GameCard focusCard = null;
        for (LibraryScreen.Entry e : libEntries) {
            GameCard c = new GameCard(this);
            c.setGame(LibraryScreen.art(getFilesDir(), libArt, e), e.tid, e.title(), LibraryScreen.chipText(this, e),
                      LibraryScreen.chipColor(e.chip));
            c.setChipShown(e.chip != GameState.Chip.READY);
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
        if (focusCard == null && !libCards.isEmpty())
            focusCard = libCards.get(0);

        libCaption = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, "");
        libCaption.setLineSpacing(0, 1.3f);
        paintScanCaption();
        if (libEntries.isEmpty()) {
            // Nothing to show yet: why, and the one thing to do.
            LinearLayout empty = column();
            empty.addView(libCaption, lp(MATCH, WRAP));
            LinearLayout buttons = rowLayout();
            LinearLayout.LayoutParams bl = lp(MATCH, Theme.BUTTON_H);
            bl.topMargin = Theme.S5;
            empty.addView(buttons, bl);
            Widgets.PillButton folder = pill(buttons, Icon.FOLDER, getString(R.string.LIB_FOLDER_TILE), true,
                                             this::onChooseLibraryFolder);
            defaultFocus = folder;
            body.addView(empty, new FrameLayout.LayoutParams(MATCH, WRAP));
            focusTid = "";
            if (backdrop != null)
                backdrop.setGlows(Theme.GLOW, Theme.GLOW2);
            if (companion != null)
                companion.library("", getString(Library.folderTree(this) == null ? R.string.LIB_FOLDER_NONE
                                                                                 : R.string.LIB_FOLDER_TILE),
                                  Theme.TEXT_DIM, null, null);
        } else {
            body.addView(LibraryScreen.grid(this, libCards, libCaption), new FrameLayout.LayoutParams(MATCH, MATCH));
            defaultFocus = focusCard;
            for (LibraryScreen.Entry e : libEntries) {
                if (libCards.get(libEntries.indexOf(e)) == focusCard) {
                    libFocus(e);
                    break;
                }
            }
        }
        focusMemory.remove(Screen.LIBRARY);   // the focus follows the title, not a place in the grid

        startAction = () -> {
            LibraryScreen.Entry e = focusedEntry();
            if (e != null)
                libraryPrimary(e);
            else
                onChooseLibraryFolder();
        };
        yAction = this::openOptions;
        addHint("A", R.string.LIB_HINT_OPEN, this::clickFocused);
        addHint("START", R.string.LIB_HINT_PRIMARY, startAction);
        addHint("Y", R.string.HINT_OPTIONS, this::openOptions);
        if (gameRunning())
            setNote(getString(R.string.LIB_RUNNING_NOTE, runningTitle()), NOTE_INFO);
    }

    /** Y on the library: the Options sheet (Rescan, Storage, Library folder, Settings, About). */
    private void openOptions() {
        if (screen != Screen.LIBRARY)
            return;
        openSheet(R.string.HINT_OPTIONS);
        sheetRow(getString(R.string.LIB_RESCAN), this::rescanLibrary);
        sheetRow(getString(R.string.LIB_STORAGE), () -> go(Screen.STORAGE))
                .setValue(getString(R.string.LIB_ST_FREE, size(freeSpace())));
        String path = Library.folderPath(this), tree = Library.folderTree(this);
        sheetRow(getString(R.string.LIB_FOLDER_TILE), this::onChooseLibraryFolder)
                .setValue(tree == null ? getString(R.string.LIB_FOLDER_NONE)
                          : path != null ? new File(path).getName() : getString(R.string.LIB_FOLDER_SAF));
        sheetRow(getString(R.string.SETTINGS), () -> go(Screen.SETTINGS));
        sheetRow(getString(R.string.ABOUT), () -> go(Screen.ABOUT));
        sheetReady();
    }

    /** A card got the focus: the context line, the backdrop's glow, the bottom screen. */
    private void libFocus(LibraryScreen.Entry e) {
        focusTid = e.tid;
        int[] g = LibraryScreen.glows(this, e);
        if (backdrop != null)
            backdrop.setGlows(g[0], g[1]);
        setContext(libContext(e), LibraryScreen.chipColor(e.chip));
        if (companion != null)
            companion.library(e.title(), LibraryScreen.chipText(this, e), LibraryScreen.chipColor(e.chip),
                              LibraryScreen.art(getFilesDir(), libArt, e), primaryLabel(e));
    }

    /** « Ready · Yesterday · 12 h 40 min played »: the focused game's state, and when it was played. */
    private String libContext(LibraryScreen.Entry e) {
        String chip = LibraryScreen.chipText(this, e);
        if (e.record == null)
            return chip;
        return chip + " · " + homeMeta(e.record.lastPlayed, e.record.playMs);
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

    /**
     * Home of a library game whose files are not here: its art (the scan's cached image), title and state, the
     * primary action (Import, Copy, Preparing, Queued (n), Choose the folder again) and, before an import, « Prepare
     * now » from the cached default.xbe, then the quiet pills.
     */
    private void buildHomeNotHere() {
        LibraryScreen.Entry e = currentEntry();
        homeChip = e.chip;
        String title = e.title();
        String chipText = LibraryScreen.chipText(this, e);
        int chipColor = LibraryScreen.chipColor(e.chip);

        LinearLayout info = hero(title, false);
        heroLine(info, chipText, chipColor);
        heroLine(info, libLead(e), 0);

        GameState.Facts f = e.facts;
        Prep p = GameState.prep(f.toolchain, f.pack == Pack.Check.VALID && f.packMatches, f.prepare, f.prepareFresh,
                                f.prepareProcess);
        LinearLayout buttons = heroButtons(info);
        Widgets.PillButton play;
        switch (e.chip) {
        case NOT_IMPORTED:
            play = pill(buttons, null, getString(R.string.LIB_IMPORT), true, this::onPlay);
            break;
        case NEEDS_COPYING:
            play = pill(buttons, null, getString(R.string.LIB_COPY), true, this::onPlay);
            break;
        case NO_FILE_ACCESS:
            play = pill(buttons, null, getString(R.string.LIB_CHOOSE_FOLDER), true, this::onPlay);
            break;
        case PREPARING:
            play = pill(buttons, null, getString(R.string.PREPARING), true, this::onPlay);
            break;
        case QUEUED:
            play = pill(buttons, null, queuedLabel(e.tid), true, this::onPlay);
            break;
        default:
            play = pill(buttons, null, getString(R.string.LIB_FOLDER_TILE), true, this::onChooseLibraryFolder);
            break;
        }
        homePrimary = play;
        // « Prepare now »: tools.prepare needs only default.xbe, so a game can be prepared (or queued) before its
        // gigabytes are imported.
        boolean canPrepare = f.toolchain && e.realTid() && cachedXbeDir() != null
                             && (e.chip == GameState.Chip.NOT_IMPORTED || e.chip == GameState.Chip.NEEDS_COPYING);
        if (canPrepare && (p == Prep.NEEDED || p == Prep.CANCELLED || p == Prep.INTERRUPTED || p == Prep.FAILED)) {
            boolean failed = p == Prep.FAILED;
            pill(buttons, null, getString(failed ? R.string.PREPARE_SEE_WHY : R.string.LIB_PREPARE_NOW), false,
                 failed ? () -> go(Screen.PREPARE) : this::startPrepare);
        }
        homePills(buttons);

        if (companion != null) {
            companion.art(art);
            companion.home(title, chipText, chipColor, primaryLabel(e) != null, play.title);
        }
        defaultFocus = play;
        startAction = this::onPlay;
        addHint("A", R.string.HINT_SELECT, this::clickFocused);
        addHint("B", history.isEmpty() ? R.string.LIB_TITLE : R.string.HINT_BACK, this::back);
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
        Widgets.PadDiagram pad = new Widgets.PadDiagram(this);
        int[] labels = { R.string.PAD_L1, R.string.PAD_L2, R.string.PAD_R1, R.string.PAD_R2, R.string.PAD_SELECT,
                         R.string.PAD_START, R.string.PAD_DPAD, R.string.PAD_LSTICK, R.string.PAD_RSTICK,
                         R.string.PAD_FACE, R.string.PAD_SCREEN };
        for (int i = 0; i < labels.length; i++)
            pad.labels[i] = getString(labels[i]);
        h.addView(pad, lp(0, MATCH, 1));

        // Your button, and the Xbox button the game gets: two quiet columns.
        LinearLayout col = column();
        int[][] rows = { { R.string.CTL_FACE, R.string.CTL_FACE_X }, { R.string.CTL_L1, R.string.CTL_L1_X },
                         { R.string.CTL_R1, R.string.CTL_R1_X }, { R.string.CTL_TRIGGERS, R.string.CTL_TRIGGERS_X },
                         { R.string.CTL_START, R.string.CTL_START_X }, { R.string.CTL_SELECT, R.string.CTL_SELECT_X },
                         { R.string.CTL_DPAD, R.string.CTL_DPAD_X }, { R.string.CTL_STICKS, R.string.CTL_STICKS_X },
                         { R.string.CTL_MENU, R.string.CTL_MENU_X } };
        for (int[] r : rows) {
            LinearLayout line = rowLayout();
            line.addView(Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT, getString(r[0]))),
                         lp(220, WRAP));
            TextView x = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, getString(r[1]));
            x.setMaxLines(2);
            line.addView(x, lp(0, WRAP, 1));
            LinearLayout.LayoutParams p = lp(MATCH, WRAP);
            p.bottomMargin = Theme.S2;
            col.addView(line, p);
        }
        TextView foot = Theme.body(this, getString(R.string.CONTROLS_FOOTER));
        LinearLayout.LayoutParams fl = lp(MATCH, WRAP);
        fl.topMargin = Theme.S3;
        col.addView(foot, fl);
        LinearLayout.LayoutParams cl = lp(Theme.DESC_W, MATCH);
        cl.leftMargin = Theme.S5;
        h.addView(col, cl);
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
        topRight.setText(getString(R.string.LIB_ST_FREE, size(freeSpace())));
        Storage.Dirs dirs = storageDirs();
        LinearLayout list = listTemplate();
        List<Storage.Item> all = new ArrayList<>();
        for (String tid : Storage.titles(dirs, games)) {
            List<Storage.Item> items = Storage.items(dirs, tid, games.get(tid));
            if (items.isEmpty())
                continue;
            Library.Record r = games.get(tid);
            listLabel(list, r != null && !r.title.isEmpty() ? r.title : tid);
            for (Storage.Item it : items) {
                storageRow(list, it);
                all.add(it);
            }
        }
        if (all.isEmpty()) {
            TextView none = Theme.body(this, getString(R.string.LIB_ST_EMPTY));
            none.setPadding(Theme.ROW_INSET, Theme.S3, 0, 0);
            list.addView(none, lp(MATCH, WRAP));
            describe(getString(R.string.LIB_ST_FOOTER));
        }
        storageMeasureLater(all);
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

    /** One stored thing of a game: its size as the value, A twice deletes it; the description says what it is. */
    private MenuRow storageRow(LinearLayout list, Storage.Item it) {
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
        MenuRow r = listRow(list, getString(storageLabel(it.kind)), getString(detailText), null);
        r.setValue(storageSize(it));
        r.setOnClickListener(v -> onStorageDelete(r, it));
        onFocus(r, () -> {
            List<String> paths = new ArrayList<>();
            for (File f : it.files) {
                if (f.exists())
                    paths.add(kv(R.string.KV_FOLDER, f.getAbsolutePath()));
            }
            boolean saves = it.kind == Storage.Kind.SAVES;
            String extra = saves ? getString(R.string.LIB_ST_SAVES_FOOTER) : TextUtils.join("\n", paths);
            describe(r.description + (saves && !paths.isEmpty() ? "\n\n" + TextUtils.join("\n", paths) : ""),
                     extra, saves ? Theme.WARN : Theme.TEXT_DIM);
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
        left.addView(mark, lp(120, 120));
        TextView name = Theme.text(this, Theme.T_DISPLAY, Theme.MEDIUM, Theme.TEXT, getString(R.string.app_name));
        LinearLayout.LayoutParams nl = lp(MATCH, WRAP);
        nl.topMargin = Theme.S4;
        left.addView(name, nl);
        TextView tag = Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, getString(R.string.ABOUT_TAGLINE));
        LinearLayout.LayoutParams gl = lp(MATCH, WRAP);
        gl.topMargin = Theme.S2;
        left.addView(tag, gl);
        TextView t = Theme.body(this, getString(R.string.ABOUT_TEXT));
        LinearLayout.LayoutParams tl = lp(MATCH, WRAP);
        tl.topMargin = Theme.S5;
        left.addView(t, tl);
        h.addView(left, lp(0, MATCH, 1));

        LinearLayout right = column();
        right.setGravity(Gravity.CENTER_VERTICAL);
        String sha = GameStore.expectedSha256(this);
        String facts = TextUtils.join("\n", new String[] {
            kv(R.string.KV_VERSION, version()), kv(R.string.KV_BUILD, getString(R.string.xba_version_code)),
            kv(R.string.KV_APP_ID, getPackageName()),
            kv(R.string.KV_GAME_CHECK, sha.isEmpty() ? getString(R.string.GAME_CHECK_NONE) : sha.substring(0, 16) + "…"),
            kv(R.string.KV_LICENCE, getString(R.string.ABOUT_LICENCE)) });
        right.addView(Theme.body(this, facts), lp(MATCH, WRAP));
        TextView legal = Theme.body(this, getString(R.string.ABOUT_LEGAL));
        LinearLayout.LayoutParams ll = lp(MATCH, WRAP);
        ll.topMargin = Theme.S5;
        right.addView(legal, ll);
        LinearLayout.LayoutParams rl = lp(Theme.DESC_W + Theme.S6, MATCH);
        rl.leftMargin = Theme.S6;
        h.addView(right, rl);
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
     * Play while another game runs: the first press says it will close (the button and the note), a second within CONFIRM_MS
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
            homePrimary.setTitle(getString(R.string.PREPARE_CANCEL_BADGE));
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
