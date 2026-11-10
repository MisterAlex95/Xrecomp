package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.app.Notification;
import android.app.NotificationManager;
import android.content.Intent;
import android.os.Looper;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.android.controller.ServiceController;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/** PrepareService with a scripted command (PrepareService.environmentOverride): foreground, status, cancel, one at a time. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PrepareServiceTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private static final String TID = "12345678";

    @Before
    public void setUp() {
        app = RuntimeEnvironment.getApplication();
    }

    @After
    public void tearDown() throws Exception {
        PrepareService.environmentOverride = null;
        Thread w = PrepareService.workerThread();
        if (w != null)
            w.join(20000);
    }

    private void useScript(String body) throws Exception {
        File script = tmp.newFile("fake_prepare.sh");
        TestSupport.write(script, body);
        File files = app.getFilesDir();
        PrepareService.environmentOverride = (c, gameDir, titleId, runner) -> {
            runner.installProgress(50, "toolkit");
            return new PrepareRunner.CommandFactory() {
                @Override
                public List<String> command(int jobs) {
                    return TestSupport.sh(script, String.valueOf(jobs), Pack.packDir(files, titleId).getAbsolutePath());
                }

                @Override
                public Map<String, String> environment() {
                    return Collections.emptyMap();
                }

                @Override
                public File workingDir() {
                    return tmp.getRoot();
                }
            };
        };
    }

    private ServiceController<PrepareService> start() {
        Intent i = PrepareService.startIntent(app, tmp.getRoot().getAbsolutePath(), TID, "Test title");
        ServiceController<PrepareService> sc = Robolectric.buildService(PrepareService.class, i).create();
        sc.startCommand(0, 1);
        return sc;
    }

    private void waitWorker() throws Exception {
        Thread w = PrepareService.workerThread();
        assertNotNull(w);
        w.join(30000);
        assertFalse(w.isAlive());
        shadowOf(Looper.getMainLooper()).idle();
    }

    @Test
    public void progressThenDone() throws Exception {
        // The script writes a valid pack where the service will check it (like tools.prepare's pack step).
        String packJson = "{\"format\":1,\"title_id\":\"" + TID + "\",\"shell_abi\":1,\"lib\":\"libtitle.so\","
                          + "\"lib_size\":4,\"abi\":\"arm64-v8a\",\"entry\":\"recomp_title_main\"}";
        useScript("echo 'XBA_PROGRESS step=1/5 id=identify pct=100 msg=ok'\n"
                  + "echo 'XBA_PROGRESS step=4/5 id=compile pct=50 eta=120 msg=jobs '$1\n"
                  + "mkdir -p \"$2\" && printf 'ELF!' > \"$2/libtitle.so\"\n"
                  + "echo '" + packJson + "' > \"$2/pack.json\"\n"
                  + "echo \"XBA_RESULT ok pack=$2\"\n");
        ServiceController<PrepareService> sc = start();
        assertTrue(PrepareService.busy() || PrepareService.workerThread() != null);
        Notification fg = shadowOf(sc.get()).getLastForegroundNotification();
        assertNotNull("the service went foreground", fg);
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertNotNull(s);
        assertEquals(PrepareProtocol.DONE, s.state);
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID));
        assertTrue(PrepareService.logFile(app, TID).isFile());
        assertTrue(shadowOf(sc.get()).isStoppedBySelf());
        NotificationManager nm = app.getSystemService(NotificationManager.class);
        assertNotNull(shadowOf(nm).getNotification(PrepareService.NOTIF_DONE_ID));
    }

    @Test
    public void okWithoutAPackFails() throws Exception {
        useScript("echo 'XBA_RESULT ok pack=/nowhere'\n");
        start();
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.FAILED, s.state);
        assertEquals(PrepareRunner.PACK_INVALID, s.code);
    }

    @Test
    public void errorThenStatusFailed() throws Exception {
        useScript("echo 'XBA_RESULT error code=NOT_AN_XBE msg=default.xbe is not an XBE'\nexit 1\n");
        start();
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.FAILED, s.state);
        assertEquals("NOT_AN_XBE", s.code);
        assertEquals("default.xbe is not an XBE", s.msg);
    }

    @Test
    public void cancelAHang() throws Exception {
        useScript("echo 'XBA_PROGRESS step=3/5 id=lift pct=1 msg=forever'\nwhile :; do sleep 1; done\n");
        ServiceController<PrepareService> sc = start();
        File status = PrepareService.statusFile(app, TID);
        long until = System.currentTimeMillis() + 15000;
        while (System.currentTimeMillis() < until) {
            PrepareProtocol.Status s = PrepareProtocol.readStatus(status);
            if (s != null && "lift".equals(s.id))
                break;
            Thread.sleep(50);
        }
        assertEquals("lift", PrepareProtocol.readStatus(status).id);
        // A second start while it runs is ignored (one Prepare at a time).
        Thread w = PrepareService.workerThread();
        sc.withIntent(PrepareService.startIntent(app, "/elsewhere", "99999999", "x")).startCommand(0, 2);
        assertEquals(w, PrepareService.workerThread());
        sc.withIntent(PrepareService.cancelIntent(app, TID)).startCommand(0, 3);
        waitWorker();
        assertEquals(PrepareProtocol.CANCELLED, PrepareProtocol.readStatus(status).state);
    }

    @Test
    public void installFailureIsReported() throws Exception {
        PrepareService.environmentOverride = (c, gameDir, titleId, runner) -> {
            throw new Toolchain.MissingAssetException("no toolkit.zip in this APK");
        };
        start();
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.FAILED, s.state);
        assertEquals(PrepareRunner.NO_TOOLS, s.code);
    }

    @Test
    public void cancelWithNothingRunningFixesAStaleStatus() throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = TID;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
        ServiceController<PrepareService> sc =
                Robolectric.buildService(PrepareService.class, PrepareService.cancelIntent(app, TID)).create();
        sc.startCommand(0, 1);
        assertEquals(PrepareProtocol.CANCELLED,
                     PrepareProtocol.readStatus(PrepareService.statusFile(app, TID)).state);
    }
}
