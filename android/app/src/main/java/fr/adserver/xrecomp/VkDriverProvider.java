package fr.adserver.xrecomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.OutputStream;
import java.util.List;

/**
 * XRecomp's Vulkan driver choice for its games' own apps (VkDriverShare): exported, but every call checks the caller
 * as the pack handover does (PackHandover.checkCaller: package fr.adserver.xrecomp.t&lt;tid&gt;, signed by the game
 * apps' key). call(« active », tid) gives the id of the driver that title starts with in XRecomp (its own choice, else
 * the global one: VkDrivers.resolve); openFile(content://&lt;authority&gt;/&lt;id&gt;) streams that driver as a
 * package zip through a pipe. Only the calling title's driver is served. In a game's own app
 * (TitleApk moves the authority) it refuses everything (NOT_A_PARENT).
 */
public class VkDriverProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    /** Null when the calling app may read the driver choice, else why not. */
    private PackHandover.Refusal check(String tid) {
        Context c = getContext();
        if (c == null)
            return PackHandover.Refusal.NO_CALLER;
        String caller;
        try {
            caller = getCallingPackage();
        } catch (SecurityException e) {
            caller = null;
        }
        return PackHandover.checkCaller(c, caller, tid != null ? tid : VkDriverShare.callerTitleId(caller));
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        if (!VkDriverShare.METHOD_ACTIVE.equals(method))
            return null;
        PackHandover.Refusal r = check(arg);
        if (r != null) {
            Log.w(LauncherActivity.TAG, "launcher: vulkan driver choice refused (" + arg + "): " + r);
            return VkDriverShare.refusal(r);
        }
        return VkDriverShare.answer(VkDrivers.of(getContext()), TitleApk.normKey(arg));
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode))
            throw new SecurityException("read-only");
        PackHandover.Refusal r = check(null);
        if (r != null)
            throw new SecurityException("refused: " + r);
        List<String> seg = uri.getPathSegments();
        VkDrivers drivers = VkDrivers.of(getContext());
        String id = seg.size() == 1 ? seg.get(0) : null;
        String caller;
        try {
            caller = getCallingPackage();
        } catch (SecurityException e) {
            caller = null;
        }
        VkDrivers.Driver served = drivers.resolve(VkDriverShare.callerTitleId(caller));
        if (id == null || served == null || !id.equals(served.id))
            throw new FileNotFoundException(uri.toString());
        ParcelFileDescriptor[] pipe;
        try {
            pipe = ParcelFileDescriptor.createReliablePipe();
        } catch (IOException e) {
            throw new FileNotFoundException(e.toString());
        }
        ParcelFileDescriptor w = pipe[1];
        new Thread(() -> {
            try (OutputStream o = new ParcelFileDescriptor.AutoCloseOutputStream(w)) {
                drivers.exportZip(id, o);
                Log.i(LauncherActivity.TAG, "launcher: vulkan driver " + id + " handed to a game's own app");
            } catch (IOException e) {
                Log.w(LauncherActivity.TAG, "launcher: vulkan driver " + id + " export failed: " + e);
                try {
                    w.closeWithError(e.toString());
                } catch (IOException ignored) {
                }
            }
        }, "xba-vkd-export").start();
        return pipe[0];
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return "application/zip";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
