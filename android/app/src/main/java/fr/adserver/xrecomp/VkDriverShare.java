package fr.adserver.xrecomp;

import android.content.Context;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;

/**
 * A game's own app uses the Vulkan driver chosen in XRecomp (docs/android/app-shell.md « Vulkan drivers »): it has
 * no network (no list, no download) and no choice of its own.
 *
 * XRecomp's side: {@link VkDriverProvider} (exported, authority &lt;XRecomp's package&gt;.vkdrivers) answers only a
 * caller PackHandover.checkCaller accepts (package fr.adserver.xrecomp.t&lt;tid&gt; signed by the game apps' key):
 * {@link #METHOD_ACTIVE} returns the active driver's id (empty: the system driver) and its origin; reading
 * content://&lt;authority&gt;/&lt;id&gt; streams that driver as a package zip (VkDrivers.exportZip).
 *
 * The game app's side ({@link #sync}): at each start (the launcher, on a thread; ShellActivity before an N64 game),
 * it asks; a new id is fetched and installed with Origin.inherited (XRecomp's id kept, so the trial marker and the
 * choice line up) and made active; « system » clears the choice; inherited drivers no longer chosen are deleted.
 * Unchanged: nothing is written (the native trial marker stays). XRecomp missing, too old (no provider) or refusing:
 * the app keeps what it had.
 */
final class VkDriverShare {
    private VkDriverShare() {}

    static final String AUTHORITY_SUFFIX = ".vkdrivers";
    static final String METHOD_ACTIVE = "active";
    static final String KEY_ID = "id", KEY_REPO = "repo", KEY_TAG = "tag", KEY_ASSET = "asset",
                        KEY_REFUSAL = "refusal";

    static Uri base(String parentPackage) {
        return new Uri.Builder().scheme("content").authority(parentPackage + AUTHORITY_SUFFIX).build();
    }

    static Uri driverUri(String parentPackage, String id) {
        return base(parentPackage).buildUpon().appendPath(id).build();
    }

    /** A game app's title ID from its package (fr.adserver.xrecomp.t&lt;tid&gt;), or null. */
    static String callerTitleId(String caller) {
        if (caller == null || !caller.startsWith(TitleApk.PACKAGE_PREFIX))
            return null;
        return TitleApk.keyOfPackage(caller);
    }

    // ---- XRecomp's answer -----------------------------------------------------------------------------------------

    /**
     * The answer to METHOD_ACTIVE for the game app of title tid: the driver that game starts with in XRecomp (its own
     * choice, else the global one: VkDrivers.resolve) as an id ("" = system) and its origin's description.
     */
    static Bundle answer(VkDrivers drivers, String tid) {
        Bundle b = new Bundle();
        VkDrivers.Driver d = drivers.resolve(tid);
        b.putString(KEY_ID, d == null ? "" : d.id);
        if (d != null) {
            b.putString(KEY_REPO, d.repo);
            b.putString(KEY_TAG, d.tag);
            b.putString(KEY_ASSET, d.asset);
        }
        return b;
    }

    static Bundle refusal(PackHandover.Refusal r) {
        Bundle b = new Bundle();
        b.putString(KEY_REFUSAL, r.name());
        return b;
    }

    // ---- The game app's side --------------------------------------------------------------------------------------

    /** Opens XRecomp's export of a driver. */
    interface Opener {
        InputStream open(String id) throws IOException;
    }

    /** What a sync did. */
    enum Outcome { UNCHANGED, INSTALLED, SELECTED, SYSTEM, NO_ANSWER, FAILED }

    /**
     * Applies XRecomp's answer to this app's store: id "" = system, else installed (through opener when missing)
     * and made active. Writes nothing when it is already so. Inherited drivers that are not the active one are
     * deleted.
     */
    static Outcome apply(VkDrivers local, Bundle answer, Opener opener) {
        if (answer == null || answer.containsKey(KEY_REFUSAL) || !answer.containsKey(KEY_ID))
            return Outcome.NO_ANSWER;
        String id = answer.getString(KEY_ID, "");
        Outcome o;
        if (id.isEmpty()) {
            o = local.activeId() == null ? Outcome.UNCHANGED : Outcome.SYSTEM;
            if (o == Outcome.SYSTEM)
                local.clearActive();
        } else {
            if (!VkDrivers.validId(id))
                return Outcome.NO_ANSWER;
            o = Outcome.UNCHANGED;
            if (local.read(id) == null) {
                try (InputStream in = opener.open(id)) {
                    if (in == null)
                        throw new IOException("no stream for " + id);
                    local.install(in, VkDrivers.Origin.inherited(id, answer.getString(KEY_REPO),
                                                                 answer.getString(KEY_TAG),
                                                                 answer.getString(KEY_ASSET)));
                } catch (IOException | RuntimeException e) {
                    Log.w(LauncherActivity.TAG, "vulkan driver: cannot take " + id + " from XRecomp: " + e);
                    return Outcome.FAILED;
                }
                o = Outcome.INSTALLED;
            }
            if (!id.equals(local.activeId())) {
                local.setActive(id);
                if (o == Outcome.UNCHANGED)
                    o = Outcome.SELECTED;
            }
        }
        String active = local.activeId();
        for (VkDrivers.Driver d : local.list()) {
            if (VkDrivers.Origin.INHERITED.equals(d.originKind) && !d.id.equals(active))
                local.delete(d.id);
        }
        return o;
    }

    /**
     * A game's own app: asks XRecomp (its PARENT) for its driver choice and applies it. Blocks (IPC, maybe a copy of a
     * few MB): off the UI thread where possible. Nothing in XRecomp itself.
     */
    static Outcome sync(Context c) {
        String parent = TitleApp.ownParent(c), tid = TitleApk.normKey(TitleApp.ownTitleId(c));
        if (parent == null || tid == null)
            return Outcome.NO_ANSWER;
        Bundle b;
        try {
            b = c.getContentResolver().call(base(parent), METHOD_ACTIVE, tid, null);
        } catch (RuntimeException e) {   // XRecomp missing, or older than this provider
            Log.i(LauncherActivity.TAG, "vulkan driver: XRecomp not asked (" + e.getClass().getSimpleName() + ")");
            return Outcome.NO_ANSWER;
        }
        if (b != null && b.containsKey(KEY_REFUSAL))
            Log.w(LauncherActivity.TAG, "vulkan driver: XRecomp refused: " + b.getString(KEY_REFUSAL));
        Outcome o = apply(VkDrivers.of(c), b,
                          id -> c.getContentResolver().openInputStream(driverUri(parent, id)));
        if (o != Outcome.UNCHANGED && o != Outcome.NO_ANSWER)
            Log.i(LauncherActivity.TAG, "vulkan driver: from XRecomp: " + o + " "
                                        + (b == null ? "" : b.getString(KEY_ID, "")));
        return o;
    }
}
