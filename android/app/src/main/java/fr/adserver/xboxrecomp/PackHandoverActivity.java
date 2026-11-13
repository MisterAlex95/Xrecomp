package fr.adserver.xboxrecomp;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/**
 * XRecomp's side of the pack handover (XBA-176, PackHandover): a translucent activity without a view that a game's
 * own app starts for a result. It checks the caller (getCallingPackage: set only by startActivityForResult), answers
 * with its pack's URIs granted or a refusal, and finishes at once: the player sees nothing of XRecomp.
 */
public class PackHandoverActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String caller = getCallingPackage();
        PackHandover.Answer a = PackHandover.serve(this, caller, getIntent());
        String tid = getIntent() == null ? null : getIntent().getStringExtra(PackHandover.EXTRA_TITLE_ID);
        if (a.refusal == null)
            Log.i(LauncherActivity.TAG, "launcher: pack " + tid + " handed over to " + caller);
        else
            Log.w(LauncherActivity.TAG, "launcher: pack handover refused to " + caller + " (" + tid + "): "
                                        + a.refusal);
        setResult(a.code, a.data);
        finish();
        overridePendingTransition(0, 0);
    }
}
