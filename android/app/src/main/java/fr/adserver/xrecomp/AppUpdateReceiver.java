package fr.adserver.xrecomp;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.util.Log;

/**
 * The installer's answers for XRecomp's own update (AppUpdate.install): STATUS_PENDING_USER_ACTION starts Android's
 * confirmation; a final status (cancelled, failed with its message) is kept (AppUpdate.saveOutcome) and passed to the
 * launcher when it is in front, otherwise at its next resume. A success is rarely heard: Android ends this app to
 * replace it, and the launcher says « Updated to … » at its next start (AppUpdate.justUpdated). Not exported: only
 * this app's own PendingIntent reaches it. In the launcher's process.
 */
public class AppUpdateReceiver extends BroadcastReceiver {
    static final String ACTION = "fr.adserver.xrecomp.APP_UPDATE_STATUS";
    static final String EXTRA_VERSION = "fr.adserver.xrecomp.APP_UPDATE_VERSION";

    /** The launcher in front, told of a final status; null when it is not. */
    interface Listener {
        void onAppUpdateOutcome();
    }

    static volatile Listener listener;

    @Override
    public void onReceive(Context c, Intent in) {
        int status = in.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        String msg = in.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
        String version = in.getStringExtra(EXTRA_VERSION);
        Log.i(LauncherActivity.TAG, "launcher: update " + version + ": installer status " + status
                                    + (msg == null ? "" : " (" + msg + ")"));
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            @SuppressWarnings("deprecation")     // the typed overload is API 33; minSdk 30
            Intent confirm = in.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                try {
                    c.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                    return;
                } catch (RuntimeException e) {
                    Log.w(LauncherActivity.TAG, "launcher: cannot show the update confirmation", e);
                    msg = String.valueOf(e.getMessage());
                }
            }
            status = PackageInstaller.STATUS_FAILURE;
        }
        AppUpdate.saveOutcome(c, status, msg, version);
        AppUpdate.clean(c);
        Listener l = listener;
        if (l != null)
            l.onAppUpdateOutcome();
    }
}
