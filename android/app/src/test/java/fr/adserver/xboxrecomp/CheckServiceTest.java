package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.content.Intent;

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

import java.time.Duration;
import java.util.Arrays;

/**
 * CheckService: foreground with its notification, a driver over check.json (recovering a check left by a dead
 * process, not a fresh one), the screen's Stop, and nothing to do without an active check.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class CheckServiceTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private CheckDriverTest.Fake fake;

    @Before
    public void setUp() throws Exception {
        app = RuntimeEnvironment.getApplication();
        CheckDriverTest outer = new CheckDriverTest();
        fake = outer.new Fake();
        fake.files = app.getFilesDir();
        CheckService.platformOverride = (c, changed) -> fake;
    }

    @After
    public void tearDown() {
        CheckService.platformOverride = null;
    }

    private ServiceController<CheckService> start(boolean fresh) {
        Intent i = CheckService.intent(app, CheckService.ACTION_START).putExtra(CheckService.EXTRA_FRESH, fresh);
        ServiceController<CheckService> sc = Robolectric.buildService(CheckService.class, i).create();
        sc.startCommand(0, 1);
        return sc;
    }

    @Test
    public void withoutAnActiveCheckItStops() {
        ServiceController<CheckService> sc = start(true);
        assertTrue(shadowOf(sc.get()).isStoppedBySelf());
        assertNull(CheckService.driver());
        sc.destroy();
    }

    @Test
    public void carriesOnALeftCheckAndStops() throws Exception {
        LibraryCheck.Title a = CheckDriverTest.title("58420001", "Sample Racer", "/g/1");
        a.phase = LibraryCheck.BOOTING;
        LibraryCheck.State s = LibraryCheck.start(Arrays.asList(a, CheckDriverTest.title("58420002", "Tiny Tanks",
                                                                                         "/g/2")), 1);
        s.run = new LibraryCheck.Run();
        s.run.launchedAt = fake.now - 30_000;
        LibraryCheck.write(app.getFilesDir(), s);
        ServiceController<CheckService> sc = start(false);
        assertNotNull(shadowOf(sc.get()).getLastForegroundNotification());
        assertNotNull(CheckService.driver());
        shadowOf(sc.get().looperForTest()).idleFor(Duration.ofMillis(100));
        // The boot that was cut is graded (« lost »), the check goes on with the next title.
        assertEquals("lost", fake.grades.get(0).exit);
        assertEquals(1, LibraryCheck.read(app.getFilesDir()).index);
        sc.withIntent(CheckService.intent(app, CheckService.ACTION_STOP)).startCommand(0, 2);
        shadowOf(sc.get().looperForTest()).idleFor(Duration.ofMillis(1000));
        assertEquals(LibraryCheck.STOPPED, LibraryCheck.read(app.getFilesDir()).state);
        sc.destroy();
    }
}
