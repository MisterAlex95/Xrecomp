package fr.adserver.xrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Typeface;
import android.text.InputType;
import android.util.Log;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Toast;

import java.util.List;

/**
 * The dialogs of a game's cheat codes (XBA-197, {@link Cheats}): « Add a code » (a name and the code's lines, typed on
 * the keyboard: the one place of the app that needs text) for the in-game menu's Cheats page and Game settings, and
 * Game settings' list (turn each on or off, add, delete). Every change is written at once; a running game reads the
 * file again by itself.
 */
final class CheatsUi {
    private CheatsUi() {}

    static AlertDialog.Builder builder(Context c) {
        return new AlertDialog.Builder(c, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    static String problemText(Context c, Console con, Cheats.Problem p) {
        switch (p) {
        case EMPTY:
            return c.getString(R.string.CHEATS_P_EMPTY);
        case NOT_HEX:
            return c.getString(R.string.CHEATS_P_NOT_HEX);
        case TOO_LONG:
            return c.getString(R.string.CHEATS_P_TOO_LONG);
        case ENCRYPTED:
            return c.getString(R.string.CHEATS_P_ENCRYPTED);
        default:
            return c.getString(con == Console.GBA    ? R.string.CHEATS_P_SHAPE_GBA
                               : con == Console.XBOX ? R.string.CHEATS_P_SHAPE_XBOX
                               : con == Console.GC   ? R.string.CHEATS_P_SHAPE_GC
                               : con == Console.X360 ? R.string.CHEATS_P_SHAPE_X360
                                                     : R.string.CHEATS_P_SHAPE);
        }
    }

    /**
     * Adds a cheat after checking its shape (the dialog stays open on a problem, which shows under the code).
     * after(name) runs once it is saved; nothing runs on Cancel.
     */
    static AlertDialog add(Activity a, Console con, String key, java.util.function.Consumer<String> after) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int)(20 * a.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        EditText name = new EditText(a);
        name.setSingleLine(true);
        name.setHint(R.string.CHEATS_NAME_HINT);
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        box.addView(name);
        EditText code = new EditText(a);
        code.setHint(con == Console.XBOX ? R.string.CHEATS_CODE_HINT_XBOX : R.string.CHEATS_CODE_HINT);
        code.setTypeface(Typeface.MONOSPACE);
        code.setMinLines(3);
        code.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                          | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        box.addView(code);
        AlertDialog d = builder(a).setTitle(R.string.CHEATS_ADD_TITLE).setView(box)
                                  .setPositiveButton(R.string.CHEATS_OK, null)
                                  .setNegativeButton(R.string.CHEATS_CANCEL, null).create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            List<String> lines = Cheats.normalize(code.getText().toString());
            Cheats.Problem p = Cheats.check(con, lines);
            if (p != null) {
                code.setError(problemText(a, con, p));
                return;
            }
            Cheats cheats = Cheats.load(a.getFilesDir(), key);
            Cheats.Cheat c = cheats.add(name.getText().toString(), lines,
                                        a.getString(R.string.CHEATS_DEFAULT_NAME, cheats.list.size() + 1));
            if (!cheats.save()) {
                Toast.makeText(a, R.string.CHEATS_SAVE_FAILED, Toast.LENGTH_LONG).show();
                return;
            }
            Log.i(LauncherActivity.TAG, "cheats: added \"" + c.name + "\" (" + lines.size() + " line(s)) for " + key);
            d.dismiss();
            if (after != null)
                after.accept(c.name);
        }));
        d.show();
        return d;
    }

    /**
     * Game settings' Cheats row: the list with a check box each (on / off, saved at once), « Add a code », « Delete a
     * code » (a list, then the code goes). changed runs after any change (the row's count).
     */
    static AlertDialog manage(Activity a, Console con, String key, Runnable changed) {
        Cheats cheats = Cheats.load(a.getFilesDir(), key);
        int n = cheats.list.size();
        String[] names = new String[n];
        boolean[] on = new boolean[n];
        for (int i = 0; i < n; i++) {
            names[i] = cheats.list.get(i).name;
            on[i] = cheats.list.get(i).enabled;
        }
        AlertDialog.Builder b = builder(a).setTitle(R.string.CHEATS);
        if (n == 0)
            b.setMessage(R.string.CHEATS_NONE);
        else
            b.setMultiChoiceItems(names, on, (dlg, which, checked) -> {
                Cheats now = Cheats.load(a.getFilesDir(), key);
                if (which < now.list.size()) {
                    now.list.get(which).enabled = checked;
                    if (!now.save())
                        Toast.makeText(a, R.string.CHEATS_SAVE_FAILED, Toast.LENGTH_LONG).show();
                    Log.i(LauncherActivity.TAG, "cheats: \"" + now.list.get(which).name + "\" " + (checked ? "on" : "off")
                                                + " for " + key);
                }
                if (changed != null)
                    changed.run();
            });
        b.setPositiveButton(R.string.CHEATS_DONE, null);
        b.setNeutralButton(R.string.CHEATS_ADD, (dlg, w) -> add(a, con, key, name -> {
            if (changed != null)
                changed.run();
            manage(a, con, key, changed);
        }));
        if (n > 0)
            b.setNegativeButton(R.string.CHEATS_DELETE, (dlg, w) -> builder(a).setTitle(R.string.CHEATS_DELETE_TITLE)
                    .setItems(names, (d2, which) -> {
                        Cheats now = Cheats.load(a.getFilesDir(), key);
                        if (which < now.list.size()) {
                            String gone = now.list.get(which).name;
                            now.remove(which);
                            if (!now.save())
                                Toast.makeText(a, R.string.CHEATS_SAVE_FAILED, Toast.LENGTH_LONG).show();
                            Log.i(LauncherActivity.TAG, "cheats: deleted \"" + gone + "\" for " + key);
                        }
                        if (changed != null)
                            changed.run();
                        manage(a, con, key, changed);
                    })
                    .setNegativeButton(R.string.CHEATS_CANCEL, (d2, w2) -> manage(a, con, key, changed)).show());
        return b.show();
    }

    /** « 2 on of 5 », or « No cheat code yet ». */
    static String countLabel(Context c, String key) {
        Cheats cheats = Cheats.load(c.getFilesDir(), key);
        return cheats.list.isEmpty() ? c.getString(R.string.CHEATS_NONE)
                                     : c.getString(R.string.CHEATS_COUNT, cheats.enabledCount(), cheats.list.size());
    }
}
