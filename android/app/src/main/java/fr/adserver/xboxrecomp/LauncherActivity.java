package fr.adserver.xboxrecomp;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.format.Formatter;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * The game picker (XBA-54, XBA-40): the app's launcher entry, in its own process (":launcher"), before SDL.
 *
 * With a valid game it starts ShellActivity at once and shows nothing. Otherwise (no game, the remembered folder
 * gone, NO_GAME_DATA from the native side, or « Change game » from the app icon's long-press menu) it shows one
 * screen: « Choose game folder » (read in place with All files access, else copied into the app's game/ folder),
 * « Choose disc image (ISO) » (extracted into game/ by libxba_import.so) and « Start the game ».
 * The choice is remembered in SharedPreferences (GameStore); ShellActivity hands it to native code as
 * RECOMP_GAME_DIR. See docs/android/app-shell.md, « Choosing the game ».
 */
public class LauncherActivity extends Activity {
    static final String TAG = "xboxrecomp";
    /** Open the picker screen even when a game is set up (the « Change game » shortcut). */
    static final String ACTION_CHOOSE_GAME = "fr.adserver.xboxrecomp.CHOOSE_GAME";
    /** Set by ShellActivity when the native side found no default.xbe: the folder it looked in. */
    static final String EXTRA_NO_GAME_DATA = "fr.adserver.xboxrecomp.NO_GAME_DATA";

    private static final int REQ_TREE = 1;
    private static final int REQ_ISO = 2;
    private static final int REQ_ALL_FILES = 3;
    private static final long SPACE_MARGIN = 64L << 20;

    private TextView status, message, progressLabel, progressDetail;
    private Button chooseFolder, chooseIso, start, cancel;
    private View progressBox;
    private ProgressBar progressBar;

    private Thread worker;
    private volatile boolean cancelRequested;
    private long lastUiUpdate;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        publishShortcut();

        Intent in = getIntent();
        boolean forced = ACTION_CHOOSE_GAME.equals(in.getAction()) || in.hasExtra(EXTRA_NO_GAME_DATA);
        if (!forced && savedInstanceState == null && gameReady()) {
            launchShell(false);
            return;
        }

        setContentView(R.layout.activity_launcher);
        status = findViewById(R.id.status);
        message = findViewById(R.id.message);
        chooseFolder = findViewById(R.id.choose_folder);
        chooseIso = findViewById(R.id.choose_iso);
        start = findViewById(R.id.start);
        progressBox = findViewById(R.id.progress_box);
        progressLabel = findViewById(R.id.progress_label);
        progressDetail = findViewById(R.id.progress_detail);
        progressBar = findViewById(R.id.progress_bar);
        cancel = findViewById(R.id.cancel);

        chooseFolder.setOnClickListener(v -> onChooseFolder());
        chooseIso.setOnClickListener(v -> openIsoPicker());
        start.setOnClickListener(v -> launchShell(true));
        cancel.setOnClickListener(v -> {
            cancelRequested = true;
            cancel.setEnabled(false);
        });

        String noData = in.getStringExtra(EXTRA_NO_GAME_DATA);
        String dir = GameStore.chosenDir(this);
        Log.i(TAG, "picker: shown (" + (noData != null ? "NO_GAME_DATA in " + noData
                                        : forced ? "change game"
                                        : dir != null ? "remembered folder unusable: " + dir
                                        : "no game set up") + ")");
        refreshStatus(noData);
        if (gameRunning())
            showMessage(getString(R.string.GAME_RUNNING), false);
        chooseFolder.requestFocus();
    }

    /** Long-press on the app icon: « Change game ». Dynamic, as the package name differs per title. */
    private void publishShortcut() {
        try {
            ShortcutManager sm = getSystemService(ShortcutManager.class);
            if (sm == null)
                return;
            Intent i = new Intent(this, LauncherActivity.class).setAction(ACTION_CHOOSE_GAME)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            ShortcutInfo s = new ShortcutInfo.Builder(this, "change_game")
                    .setShortLabel(getString(R.string.SHORTCUT_CHANGE_GAME))
                    .setLongLabel(getString(R.string.SHORTCUT_CHANGE_GAME_LONG))
                    .setIntent(i)
                    .build();
            sm.setDynamicShortcuts(Collections.singletonList(s));
        } catch (RuntimeException e) {
            Log.w(TAG, "picker: could not publish the Change game shortcut", e);
        }
    }

    /**
     * A game is ready when the remembered folder holds default.xbe, or, with nothing remembered, when recomp.cfg sets
     * RECOMP_GAME_DIR (the native side then decides) or default.xbe was pushed to the default game/ folder.
     */
    private boolean gameReady() {
        String dir = GameStore.chosenDir(this);
        if (dir != null)
            return GameStore.findXbe(new File(dir)) != null;
        if (GameStore.cfgGameDir(this) != null)
            return true;
        return GameStore.findXbe(GameStore.defaultGameDir(this)) != null;
    }

    private void refreshStatus(String noGameData) {
        String dir = GameStore.chosenDir(this);
        boolean ready = false;
        String text;
        if (noGameData != null && !noGameData.isEmpty()) {
            text = getString(R.string.NO_GAME_DATA, noGameData);
        } else if (dir != null) {
            File xbe = GameStore.findXbe(new File(dir));
            if (xbe != null) {
                String title = GameStore.chosenTitle(this);
                text = getString(R.string.CURRENT_GAME, title.isEmpty() ? new File(dir).getName() : title, dir);
                ready = true;
            } else if (new File(dir).exists() && !Environment.isExternalStorageManager()
                       && !dir.startsWith(GameStore.externalFilesDir(this).getAbsolutePath())) {
                text = getString(R.string.GAME_FOLDER_NO_ACCESS, dir);
            } else {
                text = getString(R.string.GAME_FOLDER_GONE, dir);
            }
            if (!ready)
                Log.i(TAG, "picker: remembered game folder unusable: " + dir);
        } else if (gameReady()) {
            File def = GameStore.defaultGameDir(this);
            String cfg = GameStore.cfgGameDir(this);
            text = getString(R.string.CURRENT_GAME, getString(R.string.GAME_UNNAMED),
                             cfg != null ? cfg : def.getAbsolutePath());
            ready = true;
        } else {
            text = getString(R.string.NO_GAME_SET_UP);
        }
        status.setText(text);
        start.setEnabled(ready);
    }

    private void showMessage(String text, boolean error) {
        message.setText(text);
        message.setTextColor(error ? 0xFFFF8A80 : 0xFFB0BEC5);
        message.setVisibility(text == null || text.isEmpty() ? View.GONE : View.VISIBLE);
        if (error)
            Log.i(TAG, "picker: " + text);
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
     * Starts ShellActivity. After a new choice (restart = true) a running game is ended first: the runtime maps its
     * memory once per process, so the new game needs a fresh one. Saves are in internal storage and stay.
     */
    private void launchShell(boolean restart) {
        int pid = mainProcessPid();
        /* A main process without a live ShellActivity (SDL_main returned, e.g. after NO_GAME_DATA) cannot start the
         * runtime again either: end it too. A live game with no new choice is only brought to the front. */
        if (pid != 0 && !restart && !shellAlive()) {
            Log.i(TAG, "picker: main process " + pid + " has no game window, restarting it");
            restart = true;
        }
        if (restart) {
            if (pid != 0) {
                Log.i(TAG, "picker: ending the running game (pid " + pid + ") to start the new choice");
                Process.killProcess(pid);
                long until = SystemClock.uptimeMillis() + 2000;
                while (mainProcessPid() != 0 && SystemClock.uptimeMillis() < until)
                    SystemClock.sleep(50);
            }
        }
        Intent i = new Intent(this, ShellActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (restart)
            i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
    }

    private void startChosenGame(String dir, String title) {
        GameStore.remember(this, dir, title);
        Log.i(TAG, "picker: game set: " + dir + " (" + title + ")");
        Toast.makeText(this, getString(R.string.STARTING, title.isEmpty() ? new File(dir).getName() : title),
                       Toast.LENGTH_LONG).show();
        launchShell(true);
    }

    // ---- Folder --------------------------------------------------------------------------------------------

    private void onChooseFolder() {
        showMessage("", false);
        if (Environment.isExternalStorageManager()) {
            openTreePicker();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.ALL_FILES_TITLE)
                .setMessage(R.string.ALL_FILES_EXPLAIN)
                .setPositiveButton(R.string.ALL_FILES_GRANT, (d, w) -> askAllFilesAccess())
                .setNegativeButton(R.string.ALL_FILES_COPY, (d, w) -> openTreePicker())
                .show();
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
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_TREE);
        } catch (ActivityNotFoundException e) {
            showMessage(getString(R.string.PICKER_MISSING), true);
        }
    }

    private void onTreePicked(Uri tree) {
        String path = Environment.isExternalStorageManager() ? GameStore.treeUriToPath(tree) : null;
        Log.i(TAG, "picker: folder " + tree + (path != null ? " = " + path : " (copy)"));
        File dir = path == null ? null : new File(path);
        if (dir != null && dir.list() != null) {
            if (GameStore.findXbe(dir) == null)
                showMessage(getString(R.string.NOT_A_GAME_FOLDER), true);
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
        Log.i(TAG, "picker: " + xbe + " sha256 " + sha);
        if (!GameStore.hashMatches(this, sha)) {
            fail(getString(R.string.WRONG_GAME));
            return;
        }
        String title = GameStore.xbeTitle(xbe);
        runOnUiThread(() -> startChosenGame(dir.getAbsolutePath(), title));
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

    /** Worker: copy a picked tree into <external files dir>/game (fallback without All files access, XBA-40). */
    private void copyTree(Uri tree) throws IOException {
        ContentResolver cr = getContentResolver();
        try {
            String rootId = DocumentsContract.getTreeDocumentId(tree);
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
            Log.i(TAG, "picker: copy: default.xbe sha256 " + sha + ", " + docs.size() + " entries, " + total + " bytes");
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
            try {
                cr.releasePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
            }
        }
    }

    // ---- ISO -----------------------------------------------------------------------------------------------

    private void openIsoPicker() {
        showMessage("", false);
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try {
            startActivityForResult(i, REQ_ISO);
        } catch (ActivityNotFoundException e) {
            showMessage(getString(R.string.PICKER_MISSING), true);
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
        Log.i(TAG, "picker: image " + uri + " (" + name + ")");
        if (!lower.endsWith(".iso") && !lower.endsWith(".xiso")) {
            showMessage(getString(R.string.NOT_AN_ISO, name), true);
            return;
        }
        runWorker(R.string.EXTRACTING, () -> extractIso(uri, name));
    }

    /** Worker: probe, check the space, extract into the staging dir (libxba_import.so), check the hash, swap. */
    private void extractIso(Uri uri, String name) throws IOException {
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
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

    /** <external files dir>/game.import, emptied: the import goes there, and replaces game/ only once checked. */
    private File stagingDir() throws IOException {
        File ext = GameStore.externalFilesDir(this);
        if (ext == null)
            throw new IOException("no external files dir");
        File staged = new File(ext, GameStore.GAME_SUBDIR + ".import");
        GameStore.deleteTree(staged);
        GameStore.deleteTree(new File(ext, GameStore.GAME_SUBDIR + ".import.partial"));
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
        Log.i(TAG, "picker: imported default.xbe sha256 " + sha);
        if (!GameStore.hashMatches(this, sha)) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.WRONG_GAME));
            return;
        }
        String title = GameStore.xbeTitle(xbe);
        File game = GameStore.swapIntoGameDir(this, staged);
        if (game == null) {
            GameStore.deleteTree(staged);
            fail(getString(R.string.IMPORT_FAILED, "rename"));
            return;
        }
        runOnUiThread(() -> startChosenGame(game.getAbsolutePath(), title));
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

    private void runWorker(int label, Work work) {
        if (worker != null && worker.isAlive())
            return;
        cancelRequested = false;
        setBusy(true, label);
        worker = new Thread(() -> {
            try {
                work.run();
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "picker: import failed", e);
                fail(getString(R.string.IMPORT_FAILED, String.valueOf(e.getMessage())));
            } finally {
                runOnUiThread(() -> setBusy(false, 0));
            }
        }, "xba-import");
        worker.start();
    }

    /** Buttons off, progress on, the screen kept on while a copy or an extraction runs. */
    private void setBusy(boolean busy, int label) {
        if (isFinishing())
            return;
        chooseFolder.setEnabled(!busy);
        chooseIso.setEnabled(!busy);
        if (busy)
            start.setEnabled(false);
        else
            refreshStatus(null);
        progressBox.setVisibility(busy ? View.VISIBLE : View.GONE);
        cancel.setEnabled(busy);
        if (busy) {
            progressLabel.setText(label);
            progressDetail.setText("");
            progressBar.setIndeterminate(true);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            cancel.requestFocus();
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            chooseFolder.requestFocus();
        }
    }

    private void progress(long done, long total, String path) {
        long now = SystemClock.uptimeMillis();
        if (now - lastUiUpdate < 100 && done < total)
            return;
        lastUiUpdate = now;
        runOnUiThread(() -> {
            if (total > 0) {
                progressBar.setIndeterminate(false);
                progressBar.setMax(1000);
                progressBar.setProgress((int)(done * 1000 / total));
            }
            progressDetail.setText(getString(R.string.PROGRESS_BYTES, size(done), size(total))
                                   + (path == null || path.isEmpty() ? "" : "\n" + path));
        });
    }

    private void progressIndeterminate(int label) {
        runOnUiThread(() -> {
            progressLabel.setText(label);
            progressDetail.setText("");
            progressBar.setIndeterminate(true);
            cancel.setEnabled(false);
        });
    }

    private void fail(String text) {
        runOnUiThread(() -> showMessage(text, true));
    }

    /** True (and says so) when the player pressed Cancel. */
    private boolean cancelled() {
        if (!cancelRequested)
            return false;
        Log.i(TAG, "picker: import canceled");
        runOnUiThread(() -> showMessage(getString(R.string.CANCELED), false));
        return true;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ALL_FILES) {
            Log.i(TAG, "picker: All files access " + (Environment.isExternalStorageManager() ? "granted" : "refused"));
            openTreePicker();   // in place when granted, copied otherwise
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        if (requestCode == REQ_TREE)
            onTreePicked(data.getData());
        else if (requestCode == REQ_ISO)
            onIsoPicked(data.getData());
    }

    @Override
    public void onBackPressed() {
        if (worker != null && worker.isAlive()) {
            cancelRequested = true;
            return;
        }
        super.onBackPressed();
    }
}
