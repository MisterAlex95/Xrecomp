package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.content.Intent;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The GameCube shader warm-up's triggers (GcShaderWarmup, XBA-264): its paths (the work folder's program binaries,
 * the work folder's and the pack's shader lists), the decisions (no list yet, a game runs, run), a request through
 * ShaderWarmup starting the ":warmup" service, a pending request, the app update. The native warm-up is not run here
 * (ctest gc_gx_programs runs it on Linux).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class GcShaderWarmupTest {
    private static final String GAME = "gc-GTST01-1A2B3C4D";
    private Application c;
    private File files;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        files = PrepareService.filesDir(c);
    }

    private File list(File dir) throws IOException {
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File f = new File(dir, GcShaderWarmup.LIST);
        Files.write(f.toPath(), "# xrecomp gx shader list v1\n".getBytes(StandardCharsets.UTF_8));
        return f;
    }

    @Test
    public void onlyGameCubeTitles() {
        assertTrue(GcShaderWarmup.isGc(GAME));
        assertFalse(GcShaderWarmup.isGc("n64-NZLP-B2055FBD"));
        assertFalse(GcShaderWarmup.isGc(null));
    }

    @Test
    public void theCacheIsInTheWorkFolder() {
        assertEquals(new File(files, "work/" + GAME + "/gx_programs.bin"), GcShaderWarmup.cacheFile(files, GAME));
    }

    @Test
    public void theListsThatExistWorkFolderFirst() throws IOException {
        assertEquals(0, GcShaderWarmup.lists(files, GAME).length);
        File pack = list(Pack.packDir(files, GAME));
        assertArrayEquals(new String[] { pack.getPath() }, GcShaderWarmup.lists(files, GAME));
        File work = list(GcShaderWarmup.workDir(files, GAME));
        assertArrayEquals(new String[] { work.getPath(), pack.getPath() }, GcShaderWarmup.lists(files, GAME));
        Files.write(pack.toPath(), new byte[0]);                         // an empty one is no list
        assertArrayEquals(new String[] { work.getPath() }, GcShaderWarmup.lists(files, GAME));
    }

    @Test
    public void decisions() {
        assertEquals(ShaderWarmup.Decision.SKIP_NO_LIST, GcShaderWarmup.decide(false, false));
        assertEquals(ShaderWarmup.Decision.SKIP_NO_LIST, GcShaderWarmup.decide(false, true));
        assertEquals(ShaderWarmup.Decision.PENDING, GcShaderWarmup.decide(true, true));
        assertEquals(ShaderWarmup.Decision.RUN, GcShaderWarmup.decide(true, false));
    }

    @Test
    public void aPrepareWithoutListDoesNothing() {
        assertEquals(ShaderWarmup.Decision.SKIP_NO_LIST, ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.PREPARE));
        assertNull(shadowOf(c).getNextStartedService());
    }

    @Test
    public void aPrepareWithAListStartsTheWarmup() throws IOException {
        list(GcShaderWarmup.workDir(files, GAME));
        assertEquals(ShaderWarmup.Decision.RUN, ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.PREPARE));
        Intent i = shadowOf(c).getNextStartedService();
        assertEquals(ShaderWarmupService.class.getName(), i.getComponent().getClassName());
        assertEquals(GAME, i.getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
    }

    @Test
    public void aPendingRequestIsMadeAtTheNextResume() throws IOException {
        list(Pack.packDir(files, GAME));
        ShaderWarmup.keepPending(c, GAME);
        ShaderWarmup.runPending(c);
        assertEquals(GAME, shadowOf(c).getNextStartedService().getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
    }

    @Test
    public void afterAnUpdateTheLastGameCubeGameIsWarmed() throws IOException {
        list(GcShaderWarmup.workDir(files, GAME));
        GameStore.rememberLaunch(c, null, "Test game", GAME);
        ShaderWarmup.afterUpdate(c);
        assertEquals(GAME, shadowOf(c).getNextStartedService().getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
        ShaderWarmup.afterUpdate(c);                                     // the same version: nothing
        assertNull(shadowOf(c).getNextStartedService());
    }
}
