package fr.adserver.xrecomp;

import android.app.job.JobParameters;
import android.app.job.JobService;

/**
 * « Send reports automatically » (XBA-336): posts the sessions and reports AutoReports keeps, once the network the
 * option asks for is there (JobScheduler, no WorkManager: the app has no AndroidX). In the launcher's process; tried
 * again later (the job's back-off) only after a network failure with the gate open.
 */
public final class ReportJobService extends JobService {
    @Override
    public boolean onStartJob(JobParameters params) {
        new Thread(() -> jobFinished(params, AutoReports.flush(this)), "xba-reports").start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return true;   // the network went: what is left waits for the next run
    }
}
