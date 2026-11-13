package fr.adserver.xboxrecomp;

/**
 * What the device's heat and battery allow (XBA-128, shared with the library check of XBA-130): the thresholds and
 * the decisions, as plain functions the JVM tests call with numbers. The library check pauses between titles
 * (CheckDriver); a running Prepare drops its compile jobs instead (PrepareMonitor, through the device log tools.prepare
 * reads before each compile).
 *
 * <p>Thermal statuses are PowerManager.THERMAL_STATUS_*: 0 none, 1 light, 2 moderate, 3 severe, 4 critical,
 * 5 emergency, 6 shutdown. Severe drops to one compile at a time, critical and above hold (the running compiles end,
 * none starts); moderate or less gives the jobs back. So a hot device keeps making progress slowly instead of being
 * stopped, and the platform's own throttling at critical is not fought.
 */
final class DeviceGuard {
    static final int THERMAL_MODERATE = 2, THERMAL_SEVERE = 3, THERMAL_CRITICAL = 4;
    /** Not charging and below this: the check pauses; it resumes when charging or at RESUME_BATTERY. */
    static final int LOW_BATTERY = 20, RESUME_BATTERY = 30;

    private DeviceGuard() {
    }

    /** Compiles a Prepare may run at once at this thermal status, from the jobs it was started with. */
    static int jobsFor(int thermal, int jobs) {
        if (thermal >= THERMAL_CRITICAL)
            return 0;
        if (thermal >= THERMAL_SEVERE)
            return Math.min(1, jobs);
        return jobs;
    }

    /** Too hot to start anything new (the check's pause). */
    static boolean hot(int thermal) {
        return thermal >= THERMAL_SEVERE;
    }

    /** Cool enough again to go on after a pause for heat. */
    static boolean cooled(int thermal) {
        return thermal <= THERMAL_MODERATE;
    }

    /** The battery is too low to start anything new (not charging; an unknown level, -1, is not low). */
    static boolean batteryLow(int percent, boolean charging) {
        return !charging && percent >= 0 && percent < LOW_BATTERY;
    }

    /** Enough battery again after a pause for it. */
    static boolean batteryBack(int percent, boolean charging) {
        return charging || percent >= RESUME_BATTERY;
    }
}
