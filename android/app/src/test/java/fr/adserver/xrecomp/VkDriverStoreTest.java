package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.content.Context;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Collections;
import java.util.Set;

/**
 * VkDriverStore: the VkDriverIndex.Store adapter over VkDrivers (a verified download installed with its GitHub
 * origin, end to end with a fake HTTP layer), the last-start warnings, and the readable texts. Synthetic packages
 * only (VkDriversTest's fake ELF).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class VkDriverStoreTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    private Context c;
    private File files;
    private VkDrivers vk;

    @Before
    public void setUp() throws IOException {
        c = RuntimeEnvironment.getApplication();
        files = tmp.newFolder("files");
        vk = new VkDrivers(files, 33);
    }

    static String sha(byte[] b) throws Exception {
        return VkDriverIndex.hex(MessageDigest.getInstance("SHA-256").digest(b));
    }

    private File file(byte[] b) throws IOException {
        File f = tmp.newFile();
        Files.write(f.toPath(), b);
        return f;
    }

    // ---- The adapter -----------------------------------------------------------------------------------------------

    @Test
    public void adapterListsInstalledSha256AndInstallsWithGithubOrigin() throws Exception {
        VkDriverStore.Adapter a = new VkDriverStore.Adapter(vk);
        assertTrue(a.installedSha256().isEmpty());
        byte[] z = VkDriversTest.zip(VkDriversTest.good());
        String sha = sha(z);
        VkDriverIndex.Entry e = new VkDriverIndex.Entry("MrPurple666/purple-turnip", "T30", "Turnip T30", false, 1,
                                                        "turnip_T30.adpkg.zip", z.length, sha, "https://github.com/x");
        a.install(file(z), sha, e);
        assertNotNull(a.installed);
        assertEquals(sha.substring(0, 16), a.installed.id);
        VkDrivers.Driver d = vk.read(a.installed.id);
        assertEquals(VkDrivers.Origin.GITHUB, d.originKind);
        assertEquals("MrPurple666/purple-turnip", d.repo);
        assertEquals("T30", d.tag);
        assertEquals("turnip_T30.adpkg.zip", d.asset);
        assertEquals(Collections.singleton(sha), a.installedSha256());
        // The listing hides it from « To download ».
        VkDriverIndex.Listing l = new VkDriverIndex.Listing(Arrays.asList(e), Collections.emptyList());
        assertTrue(l.notInstalled(a.installedSha256()).isEmpty());
    }

    @Test
    public void adapterRefusesAPackageThatDoesNotMatch() throws Exception {
        VkDriverStore.Adapter a = new VkDriverStore.Adapter(vk);
        byte[] z = VkDriversTest.zip(VkDriversTest.good());
        VkDriverIndex.Entry e = new VkDriverIndex.Entry("MrPurple666/purple-turnip", "T30", "", false, 1, "t.zip",
                                                        z.length + 1, null, "https://github.com/x");
        try {
            a.install(file(z), sha(z), e);
            fail("installed");
        } catch (VkDrivers.InstallException ex) {
            assertEquals(VkDrivers.Reason.SIZE_MISMATCH, ex.reason);
        }
        assertNull(a.installed);
        assertTrue(vk.list().isEmpty());
    }

    /** The whole path the Drivers screen takes: VkDriverIndex.download through the adapter into the store. */
    @Test
    public void downloadThroughTheAdapter() throws Exception {
        byte[] z = VkDriversTest.zip(VkDriversTest.good());
        VkDriverIndex.Source purple = VkDriverIndex.SOURCES.get(0);
        String tag = "vturnip_mrpurple_T30-toasted.adpkg", asset = "turnip_mrpurple_T30-toasted.adpkg.zip";
        VkDriverIndex.Entry e = new VkDriverIndex.Entry(purple.id(), tag, "", false, 1, asset, z.length, sha(z),
                                                        VkDriverIndexTest.dl(purple, tag, asset));
        VkDriverIndexTest.FakeHttp http = new VkDriverIndexTest.FakeHttp();
        http.on(e.url, r -> VkDriverIndexTest.resp(302, new byte[0], "Location", VkDriverIndexTest.CDN));
        http.on(VkDriverIndexTest.CDN, r -> VkDriverIndexTest.resp(200, z, "Content-Length", "" + z.length));
        VkDriverIndex idx = new VkDriverIndex(new File(files, "vk_drivers/index"), tmp.newFolder("cache"), http,
                                              System::currentTimeMillis, true, VkDriverIndex.SOURCES);
        VkDriverStore.Adapter a = new VkDriverStore.Adapter(vk);
        String got = idx.download(e, a, null, () -> false);
        assertEquals(sha(z), got);
        assertEquals(got.substring(0, 16), a.installed.id);
        assertEquals(1, vk.list().size());
        assertTrue(vk.setActive(a.installed.id));
        assertEquals(a.installed.id, vk.active().id);
    }

    // ---- Last start ------------------------------------------------------------------------------------------------

    private void write(String rel, String text) throws IOException {
        File f = new File(files, rel);
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    @Test
    public void lastStartFromTheRuntimeFiles() throws Exception {
        String id = vk.install(new ByteArrayInputStream(VkDriversTest.zip(VkDriversTest.good())), null).id;
        assertEquals(VkDriverStore.LastStart.UNKNOWN, VkDriverStore.lastStart(vk, id));
        write("vk_drivers/last_start", id + " ok\n");
        assertEquals(VkDriverStore.LastStart.OK, VkDriverStore.lastStart(vk, id));
        assertEquals(0, VkDriverStore.lastStartWarning(VkDriverStore.LastStart.OK));
        write("vk_drivers/last_start", id + " failed\n");
        assertEquals(VkDriverStore.LastStart.FAILED, VkDriverStore.lastStart(vk, id));
        write("vk_drivers/last_start", id + " not_loaded\n");
        assertEquals(VkDriverStore.LastStart.NOT_LOADED, VkDriverStore.lastStart(vk, id));
        write("vk_drivers/last_start", id + " crashed\n");
        assertEquals(VkDriverStore.LastStart.CRASHED, VkDriverStore.lastStart(vk, id));
        // Another driver's line says nothing about this one; garbage neither.
        write("vk_drivers/last_start", "0123456789abcdef failed\n");
        assertEquals(VkDriverStore.LastStart.UNKNOWN, VkDriverStore.lastStart(vk, id));
        write("vk_drivers/last_start", id + "\n");
        assertEquals(VkDriverStore.LastStart.UNKNOWN, VkDriverStore.lastStart(vk, id));
        // The trial marker (a start that never finished) wins.
        write("vk_drivers/trying", id + "\n");
        assertEquals(VkDriverStore.LastStart.CRASHED, VkDriverStore.lastStart(vk, id));
        for (VkDriverStore.LastStart s : new VkDriverStore.LastStart[] { VkDriverStore.LastStart.CRASHED,
                VkDriverStore.LastStart.FAILED, VkDriverStore.LastStart.NOT_LOADED })
            assertNotEquals(0, VkDriverStore.lastStartWarning(s));
    }

    // ---- Texts -----------------------------------------------------------------------------------------------------

    @Test
    public void everyRefusalHasAReadableText() {
        for (VkDrivers.Reason r : VkDrivers.Reason.values()) {
            String t = VkDriverStore.errorText(c, new VkDrivers.InstallException(r, "detail"));
            if (r != VkDrivers.Reason.IO && r != VkDrivers.Reason.BAD_ID)   // a disk error: its detail is said
                assertFalse(r + ": " + t, t.contains(r.name()));
            assertFalse(t.isEmpty());
        }
        for (VkDriverIndex.DownloadException.Reason r : VkDriverIndex.DownloadException.Reason.values())
            assertFalse(VkDriverStore.errorText(c, new VkDriverIndex.DownloadException(r, "x")).isEmpty());
        assertEquals(c.getString(R.string.VKD_ERR_NOT_DRIVER),
                     VkDriverStore.errorText(c, new VkDrivers.InstallException(VkDrivers.Reason.NO_META, null)));
        assertEquals(c.getString(R.string.VKD_ERR_CORRUPT), VkDriverStore.errorText(
                c, new VkDriverIndex.DownloadException(VkDriverIndex.DownloadException.Reason.SHA256, "x")));
        assertEquals(c.getString(R.string.VKD_CANCELED), VkDriverStore.errorText(
                c, new VkDriverIndex.DownloadException(VkDriverIndex.DownloadException.Reason.CANCELLED, "x")));
        assertEquals(c.getString(R.string.VKD_ERR_HTTP, "HTTP 404"), VkDriverStore.errorText(
                c, new VkDriverIndex.DownloadException(VkDriverIndex.DownloadException.Reason.HTTP, "HTTP 404")));
        assertEquals(c.getString(R.string.VKD_ERR_NETWORK),
                     VkDriverStore.errorText(c, new UnknownHostException("api.github.com")));
    }

    private static VkDriverIndex.SourceStatus st(VkDriverIndex.State s, long fetched, long reset, int code) {
        return new VkDriverIndex.SourceStatus(VkDriverIndex.SOURCES.get(0), s, fetched, reset, code, null);
    }

    private static VkDriverIndex.Listing listing(VkDriverIndex.SourceStatus... s) {
        return new VkDriverIndex.Listing(Collections.emptyList(), Arrays.asList(s));
    }

    @Test
    public void listStateSaysWhy() {
        long t = 1_763_100_000_000L;
        String from = c.getString(R.string.VKD_LIST_FROM, VkDriverStore.date(t));
        assertEquals(c.getString(R.string.VKD_LIST_UPDATING), VkDriverStore.listState(c, null, true));
        assertEquals(c.getString(R.string.VKD_LIST_NONE), VkDriverStore.listState(c, null, false));
        assertEquals(c.getString(R.string.VKD_LIST_NONE),
                     VkDriverStore.listState(c, listing(st(VkDriverIndex.State.NONE, 0, 0, 0)), false));
        assertEquals(from, VkDriverStore.listState(c, listing(st(VkDriverIndex.State.UPDATED, t, 0, 0)), false));
        assertEquals(c.getString(R.string.VKD_LIST_OLD, from),
                     VkDriverStore.listState(c, listing(st(VkDriverIndex.State.STALE, t, 0, 0)), false));
        assertEquals(c.getString(R.string.VKD_LIST_OFFLINE) + " · " + from,
                     VkDriverStore.listState(c, listing(st(VkDriverIndex.State.OFFLINE, t, 0, 0),
                                                        st(VkDriverIndex.State.CACHED, t + 5, 0, 0)), false));
        assertEquals(c.getString(R.string.VKD_LIST_OFFLINE),
                     VkDriverStore.listState(c, listing(st(VkDriverIndex.State.OFFLINE, 0, 0, 0)), false));
        String limited = VkDriverStore.listState(c, listing(st(VkDriverIndex.State.OFFLINE, t, 0, 0),
                                                            st(VkDriverIndex.State.RATE_LIMITED, t, t + 3600_000, 0)),
                                                 false);
        assertEquals(c.getString(R.string.VKD_LIST_LIMITED_UNTIL, VkDriverStore.time(t + 3600_000)) + " · " + from,
                     limited);
        assertEquals(c.getString(R.string.VKD_LIST_FAILED, "HTTP 502"),
                     VkDriverStore.listState(c, listing(st(VkDriverIndex.State.FAILED, 0, 0, 502)), false));
    }

    @Test
    public void labelsAndOrigins() throws Exception {
        VkDrivers.Driver d = vk.install(new ByteArrayInputStream(VkDriversTest.zip(VkDriversTest.good())), null);
        assertEquals("Turnip Test T1 (26.3.0-test)", VkDriverStore.label(d));
        assertEquals(c.getString(R.string.VKD_ORIGIN_MANUAL), VkDriverStore.originText(c, d));
    }

    /** A driver directory put there by hand (no origin.json, as on the Thor before WP5) reads as a manual import. */
    @Test
    public void driverWithoutOriginJsonIsManual() throws Exception {
        VkDrivers.Driver d = vk.install(new ByteArrayInputStream(VkDriversTest.zip(VkDriversTest.good())), null);
        File o = new File(d.dir, VkDrivers.ORIGIN);
        o.setWritable(true);
        d.dir.setWritable(true);
        assertTrue(o.delete());
        VkDrivers.Driver r = vk.read(d.id);
        assertNotNull(r);
        assertEquals(VkDrivers.Origin.MANUAL, r.originKind);
        assertNull(r.sha256);
        assertEquals(1, vk.list().size());
        assertTrue(new VkDriverStore.Adapter(vk).installedSha256().isEmpty());
        assertTrue(vk.setActive(d.id));
        assertEquals(c.getString(R.string.VKD_ORIGIN_MANUAL), VkDriverStore.originText(c, r));
        // Its listed release is not offered again: the id its sha256 gives is installed.
        String fullSha = d.sha256;
        VkDriverIndex.Entry same = new VkDriverIndex.Entry("MrPurple666/purple-turnip", "T1", "", false, 1, "t.zip",
                                                           10, fullSha, "https://github.com/x");
        VkDriverIndex.Entry other = new VkDriverIndex.Entry("MrPurple666/purple-turnip", "T2", "", false, 1, "u.zip",
                                                            10, "ab" + fullSha.substring(2), "https://github.com/y");
        VkDriverIndex.Listing l = new VkDriverIndex.Listing(Arrays.asList(same, other), Collections.emptyList());
        assertEquals(2, l.notInstalled(new VkDriverStore.Adapter(vk).installedSha256()).size());
        assertEquals(Collections.singletonList(other), VkDriverStore.toDownload(l, vk));
    }
}
