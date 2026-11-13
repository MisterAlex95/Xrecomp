package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.content.Context;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;

/** T7: the tool zips extracted once per version, with the hash check; absence hides the feature. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ToolchainTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Context ctx;
    private File files;

    @Before
    public void setUp() throws Exception {
        ctx = RuntimeEnvironment.getApplication();
        files = tmp.newFolder("files");
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
    }

    @Test
    public void absentInThisBuild() {
        // The real APK assets of this build: no tool zips, no executables -> feature hidden.
        assertFalse(Toolchain.assetsPresent(ctx));
        assertFalse(Toolchain.available(ctx));
    }

    @Test
    public void detection() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1.0");
        assertTrue(Toolchain.assetsPresent(ctx));
        assertFalse(Toolchain.available(ctx));   // no executables yet
        File nat = TestSupport.fakeNativeDir(tmp.getRoot());
        Toolchain.nativeDirOverride = nat.getAbsolutePath();
        assertTrue(Toolchain.available(ctx));
        assertTrue(new File(nat, Toolchain.CC).delete());
        assertFalse(Toolchain.available(ctx));
    }

    @Test
    public void installOnceThenNothing() throws Exception {
        File assetsDir = tmp.newFolder("assets");
        TestSupport.DirAssets a = TestSupport.toolAssets(assetsDir, "2026.11.10");
        Toolchain.assetsOverride = a;
        long[] last = { -1, -1 };
        Toolchain.Installed in = Toolchain.install(ctx, files, new Toolchain.Progress() {
            @Override
            public void onProgress(long done, long total, String what) {
                assertTrue(done <= total);
                last[0] = done;
                last[1] = total;
            }

            @Override
            public boolean cancelled() {
                return false;
            }
        });
        assertEquals(3, in.extracted);
        assertEquals(last[1], last[0]);
        assertTrue(new File(in.python, "lib/python3.13/os.py").isFile());
        assertTrue(new File(in.sysroot, "usr/include/stdio.h").isFile());
        assertTrue(new File(in.toolkit, "tools/prepare.py").isFile());
        assertEquals("2026.11.10", in.toolkitVersion);
        assertTrue(in.python.getName().matches("python-[0-9a-f]{12}"));
        assertTrue(in.toolkit.getName().matches("toolkit-[0-9a-f]{12}"));

        // Second start: nothing extracted, no zip even opened (the hashes are cached per install).
        a.opens = 0;
        Toolchain.Installed again = Toolchain.install(ctx, files, null);
        assertEquals(0, again.extracted);
        assertEquals(0, a.opens);
        assertEquals(in.toolkit, again.toolkit);
    }

    @Test
    public void newVersionReplacesTheOld() throws Exception {
        File assetsDir = tmp.newFolder("assets");
        Toolchain.assetsOverride = TestSupport.toolAssets(assetsDir, "1");
        Toolchain.Installed one = Toolchain.install(ctx, files, null);
        // A new APK: another toolkit zip (and a new install stamp)
        TestSupport.write(new File(assetsDir, "prepare/toolkit.zip"),
                          TestSupport.zip("VERSION", "2\n", "tools/prepare.py", "# v2\n"));
        ctx.getSharedPreferences("prepare_assets", Context.MODE_PRIVATE).edit().clear().commit();
        Toolchain.Installed two = Toolchain.install(ctx, files, null);
        assertEquals(1, two.extracted);
        assertNotEquals(one.toolkit, two.toolkit);
        assertFalse(one.toolkit.exists());
        assertEquals(one.python, two.python);
        assertEquals("2", two.toolkitVersion);
    }

    @Test
    public void sidecarHashChecked() throws Exception {
        File assetsDir = tmp.newFolder("assets");
        Toolchain.assetsOverride = TestSupport.toolAssets(assetsDir, "1");
        TestSupport.write(new File(assetsDir, "prepare/sysroot.zip.sha256"),
                          "0000000000000000000000000000000000000000000000000000000000000000  sysroot.zip\n");
        try {
            Toolchain.install(ctx, files, null);
            fail("expected a hash mismatch");
        } catch (Toolchain.HashMismatchException expected) {
        }
        String good = GameStore.sha256(new File(assetsDir, "prepare/sysroot.zip"));
        TestSupport.write(new File(assetsDir, "prepare/sysroot.zip.sha256"), good + "  sysroot.zip\n");
        assertEquals(3, Toolchain.install(ctx, files, null).extracted);
    }

    @Test
    public void incompleteExtractionIsRedone() throws Exception {
        File assetsDir = tmp.newFolder("assets");
        Toolchain.assetsOverride = TestSupport.toolAssets(assetsDir, "1");
        Toolchain.Installed in = Toolchain.install(ctx, files, null);
        assertTrue(new File(in.sysroot, Toolchain.COMPLETE).delete());
        assertEquals(1, Toolchain.install(ctx, files, null).extracted);
    }

    @Test
    public void missingAsset() throws Exception {
        File assetsDir = tmp.newFolder("assets");
        Toolchain.assetsOverride = TestSupport.toolAssets(assetsDir, "1");
        assertTrue(new File(assetsDir, "prepare/sysroot.zip").delete());
        assertFalse(Toolchain.assetsPresent(ctx));
        try {
            Toolchain.install(ctx, files, null);
            fail("expected a missing asset");
        } catch (Toolchain.MissingAssetException expected) {
        }
    }

    @Test
    public void jobsFormula() {
        // docs/android/prepare.md: (MemAvailable - 3 GB) / 1 GB, between 1 and min(5, cores)
        assertEquals(4, Toolchain.jobsFor(7L << 30, 8));
        assertEquals(5, Toolchain.jobsFor(12L << 30, 8));
        assertEquals(1, Toolchain.jobsFor(2L << 30, 8));
        assertEquals(1, Toolchain.jobsFor(0, 8));
        assertEquals(2, Toolchain.jobsFor(12L << 30, 2));
    }

    /** The command and environment of docs/android/prepare.md, built after the install. */
    @Test
    public void realCommandMatchesTheContract() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "abc123");
        File nat = TestSupport.fakeNativeDir(tmp.getRoot());
        Toolchain.nativeDirOverride = nat.getAbsolutePath();
        File appFiles = ctx.getFilesDir();
        PrepareRunner runner = new PrepareRunner(new File(appFiles, "prepare/4D530004"), "4D530004", 4, null, null,
                                                 null);
        PrepareRunner.CommandFactory f = PrepareService.realEnvironment(ctx, "/sdcard/Games/Sample", "4D530004",
                                                                         runner);
        java.util.List<String> cmd = f.command(3);
        String n = nat.getAbsolutePath(), files = appFiles.getAbsolutePath();
        assertEquals(n + "/libxbapython.so", cmd.get(0));
        assertEquals(java.util.Arrays.asList("-m", "tools.prepare", "/sdcard/Games/Sample", "--android"),
                     cmd.subList(1, 5));
        String joined = String.join(" ", cmd);
        assertTrue(joined, joined.contains("--work " + files + "/work/4D530004"));
        assertTrue(joined, joined.contains("--pack-dir " + files + "/packs"));
        assertTrue(joined, joined.contains("--cc " + n + "/libxbacc.so --cc-prefix-arg clang"));
        assertTrue(joined, joined.matches(".* --sysroot " + files + "/sysroot-[0-9a-f]{12} .*"));
        assertTrue(joined, joined.contains("--shell-lib " + n + "/libmain.so"));
        assertTrue(joined, joined.contains("--jobs 3"));
        assertTrue(joined, joined.contains("--opt 2"));
        assertTrue(joined, joined.contains("--shell-abi 1"));
        assertTrue(f.workingDir().getName().matches("toolkit-[0-9a-f]{12}"));
        java.util.Map<String, String> env = f.environment();
        assertTrue(env.get("PYTHONHOME").matches(files + "/python-[0-9a-f]{12}"));
        assertEquals(f.workingDir().getAbsolutePath(), env.get("PYTHONPATH"));
        assertEquals(ctx.getCacheDir() + "/pycache", env.get("PYTHONPYCACHEPREFIX"));
        assertEquals(ctx.getCacheDir() + "/tmp", env.get("TMPDIR"));
        assertEquals(files, env.get("HOME"));
        assertEquals(n, env.get("LIBCAPSTONE_PATH"));
        assertEquals(n, env.get("LD_LIBRARY_PATH"));
        assertEquals("1", env.get("PYTHONUNBUFFERED"));
        assertEquals(new File(files, "work/4D530004/prepare.log"), runner.toolLog);
        // Profile-free WP6: Settings › Enhancements off for this title leaves its title layer out.
        assertFalse(env.containsKey("XBA_NO_TITLE_LAYER"));
        assertTrue(GameStore.setEnhancements(ctx, "4d530004", false));
        assertEquals("1", f.environment().get("XBA_NO_TITLE_LAYER"));
        assertTrue(GameStore.setEnhancements(ctx, "4D530004", true));
        assertFalse(f.environment().containsKey("XBA_NO_TITLE_LAYER"));
    }

    @Test
    public void zipSlipRefused() throws Exception {
        File assetsDir = tmp.newFolder("assets");
        Toolchain.assetsOverride = TestSupport.toolAssets(assetsDir, "1");
        TestSupport.write(new File(assetsDir, "prepare/sysroot.zip"), TestSupport.zip("../../evil", "x"));
        try {
            Toolchain.install(ctx, files, null);
            fail("expected an error");
        } catch (java.io.IOException expected) {
        }
        assertFalse(new File(files.getParentFile(), "evil").exists());
    }
}
