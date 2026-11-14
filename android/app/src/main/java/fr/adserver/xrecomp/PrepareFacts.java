package fr.adserver.xrecomp;

import android.content.Context;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * What a title's last Prepare cost, from its pack.json's "prepare" object (XBA-128, tools/prepare/metrics.py): for
 * Manage's facts and for the library check's report line. A pack from before it has none: nothing is said.
 */
final class PrepareFacts {
    private PrepareFacts() {
    }

    /** pack.json's "prepare" object of tid's pack, or null. */
    static JSONObject of(File filesDir, String tid) {
        File json = new File(Pack.packDir(filesDir, tid), Pack.JSON);
        if (!json.isFile())
            return null;
        try {
            return new JSONObject(PrepareProtocol.readText(json, 1 << 20)).optJSONObject("prepare");
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    /** « 6 min 12 s », « 45 s ». */
    static String duration(double s) {
        long t = Math.round(s);
        return t >= 60 ? (t / 60) + " min " + String.format(Locale.ROOT, "%02d", t % 60) + " s" : t + " s";
    }

    /** The thermal status's name in the app's words (the companion panel's), « » for none. */
    static String thermal(Context c, int status) {
        return c.getString(PanelView.thermalLabel(status));
    }

    /**
     * Manage's lines: « Prepare took: 6 min 12 s · 4 jobs · -O2 (163 files, 160 compiled) », « Memory per compile job:
     * 590 MB at most, 163 MB typical », « Hottest during it: Very hot (2 min) ». Empty without metrics.
     */
    static List<String> lines(Context c, File filesDir, String tid) {
        List<String> out = new ArrayList<>();
        JSONObject p = of(filesDir, tid);
        if (p == null)
            return out;
        out.add(c.getString(R.string.KV_LINE, c.getString(R.string.KV_PREPARE_TOOK),
                            c.getString(R.string.PREPARE_TOOK, duration(p.optDouble("total_s", 0)),
                                        p.optInt("jobs", 0), p.optString("opt", "?"), p.optInt("files", 0),
                                        p.optInt("compiled", 0))));
        JSONObject rss = p.optJSONObject("peak_rss_mb");
        if (rss != null)
            out.add(c.getString(R.string.KV_LINE, c.getString(R.string.KV_PREPARE_MEMORY),
                                c.getString(R.string.PREPARE_MEMORY, rss.optInt("max", 0), rss.optInt("median", 0))));
        JSONObject th = p.optJSONObject("thermal");
        if (th != null && th.has("max") && !th.isNull("max")) {
            String hot = thermal(c, th.optInt("max", 0));
            int severe = th.optInt("severe_s", 0);
            if (severe > 0)
                hot = c.getString(R.string.PREPARE_HOT_FOR, hot, duration(severe));
            out.add(c.getString(R.string.KV_LINE, c.getString(R.string.KV_PREPARE_HEAT), hot));
        }
        return out;
    }

    /**
     * The library check's fields for tools.compat grade (« key=value », JSON values): what the pack's Prepare cost.
     * prepare_s is the check's own timing when it ran the Prepare, else this pack's.
     */
    static List<String> reportFields(File filesDir, String tid, boolean checkTimedIt) {
        List<String> f = new ArrayList<>();
        JSONObject p = of(filesDir, tid);
        if (p == null)
            return f;
        if (!checkTimedIt && p.has("total_s"))
            f.add("prepare_s=" + Math.round(p.optDouble("total_s", 0)));
        JSONObject rss = p.optJSONObject("peak_rss_mb");
        if (rss != null)
            f.add("prepare_peak_mb=" + rss.optInt("max", 0));
        if (p.has("pack_mb"))
            f.add("pack_mb=" + p.optDouble("pack_mb", 0));
        if (p.has("jobs"))
            f.add("prepare_jobs=" + p.optInt("jobs", 0));
        if (p.has("opt"))
            f.add("prepare_opt=" + JSONObject.quote(p.optString("opt", "")));
        JSONObject th = p.optJSONObject("thermal");
        if (th != null && th.has("max") && !th.isNull("max"))
            f.add("prepare_thermal_max=" + th.optInt("max", 0));
        if (th != null && th.has("temp_c_max"))
            f.add("prepare_temp_c_max=" + th.optDouble("temp_c_max", 0));
        JSONObject steps = p.optJSONObject("steps_s");
        if (steps != null)
            f.add("prepare_steps_s=" + steps);
        return f;
    }
}
