package fr.adserver.xrecomp;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.util.Log;

/**
 * The installer's answers for « Create an app » (TitleApp.install): STATUS_PENDING_USER_ACTION starts Android's
 * confirmation; a final status (installed, cancelled, failed with its message) is kept (TitleApp.saveOutcome) and
 * passed to the launcher when it is in front, which shows it as a note (otherwise at its next resume). Not exported:
 * only this app's own PendingIntent reaches it. In the launcher's process, with the launcher.
 */
public class TitleAppReceiver extends BroadcastReceiver {
    static final String ACTION = "fr.adserver.xrecomp.TITLE_APP_STATUS";
    static final String EXTRA_LABEL = "fr.adserver.xrecomp.TITLE_APP_LABEL";

    /** The launcher in front, told of a final status on the main thread; null when it is not. */
    interface Listener {
        void onTitleAppOutcome();
    }

    static volatile Listener listener;

    @Override
    public void onReceive(Context c, Intent in) {
        int status = in.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        String pkg = in.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME);
        String msg = in.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
        String label = in.getStringExtra(EXTRA_LABEL);
        Log.i(LauncherActivity.TAG, "launcher: game app " + pkg + ": installer status " + status
                                    + (msg == null ? "" : " (" + msg + ")"));
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            @SuppressWarnings("deprecation")     // the typed overload is API 33; minSdk 30
            Intent confirm = in.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                try {
                    c.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                    return;
                } catch (RuntimeException e) {
                    Log.w(LauncherActivity.TAG, "launcher: cannot show the install confirmation", e);
                    msg = String.valueOf(e.getMessage());
                }
            }
            status = PackageInstaller.STATUS_FAILURE;
        }
        TitleApp.saveOutcome(c, status, msg, label, pkg);
        TitleApp.clean(c);      // the signed APK was copied into the session: nothing to keep
        Listener l = listener;
        if (l != null)
            l.onTitleAppOutcome();
    }
}
