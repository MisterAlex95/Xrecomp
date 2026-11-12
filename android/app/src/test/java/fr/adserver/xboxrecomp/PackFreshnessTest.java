package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.json.JSONObject;
import org.junit.After;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;

/** XBA-127: when a valid pack no longer matches (another XBE, an older toolkit), and the toolkit digest of the APK. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PackFreshnessTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String TID = "4D530004";
    private static final String A = hex('a'), B = hex('b'), T1 = hex('1'), T2 = hex('2');

    private static String hex(char c) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 64; i++)
            b.append(c);
        return b.toString();
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        PackFreshness.forget();
    }

    @Test
    public void judge() {
        assertEquals(PackFreshness.Verdict.CURRENT, PackFreshness.judge(A, A, T1, T1));
        assertEquals(PackFreshness.Verdict.CURRENT, PackFreshness.judge(A, A.toUpperCase(), T1, T1.toUpperCase()));
        assertEquals(PackFreshness.Verdict.OTHER_XBE, PackFreshness.judge(A, B, T1, T1));
        assertEquals("another XBE wins", PackFreshness.Verdict.OTHER_XBE, PackFreshness.judge(A, B, T1, T2));
        assertEquals(PackFreshness.Verdict.OLDER_TOOLKIT, PackFreshness.judge(A, A, T1, T2));
        assertEquals("a pack from before toolkit_sha256", PackFreshness.Verdict.OLDER_TOOLKIT,
                     PackFreshness.judge(A, A, null, T2));
        // Unknown sides are not a mismatch.
        assertEquals(PackFreshness.Verdict.CURRENT, PackFreshness.judge(null, B, T1, T1));
        assertEquals(PackFreshness.Verdict.CURRENT, PackFreshness.judge("00", B, T1, T1));
        assertEquals(PackFreshness.Verdict.CURRENT, PackFreshness.judge(A, null, T1, T1));
        assertEquals("an APK without the digest", PackFreshness.Verdict.CURRENT, PackFreshness.judge(A, A, T1, ""));
        assertEquals(PackFreshness.Verdict.CURRENT, PackFreshness.judge(A, A, null, null));
    }

    @Test
    public void fromPackJson() throws Exception {
        File files = tmp.getRoot();
        assertEquals("no pack", PackFreshness.Verdict.CURRENT, PackFreshness.of(files, TID, B, T1));
        TestSupport.writePack(files, TID, 10);
        File json = new File(Pack.packDir(files, TID), "pack.json");
        JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 16));
        o.put("xbe_sha256", A.toUpperCase());
        o.put("toolkit_sha256", T1);
        TestSupport.write(json, o.toString());
        assertEquals(A, Pack.xbeSha256(files, TID));
        assertEquals(T1, Pack.toolkitSha256(files, TID));
        assertEquals(PackFreshness.Verdict.CURRENT, PackFreshness.of(files, TID, A, T1));
        assertEquals(PackFreshness.Verdict.OTHER_XBE, PackFreshness.of(files, TID, B, T1));
        assertEquals(PackFreshness.Verdict.OLDER_TOOLKIT, PackFreshness.of(files, TID, A, T2));
    }

    @Test
    public void xbeHashedOncePerChange() throws Exception {
        File xbe = tmp.newFile("default.xbe");
        TestSupport.write(xbe, "XBEH one");
        assertNull("not hashed yet", PackFreshness.knownXbeSha(xbe));
        String one = PackFreshness.xbeSha(xbe);
        assertEquals(GameStore.sha256(xbe), one);
        assertEquals(one, PackFreshness.knownXbeSha(xbe));
        TestSupport.write(xbe, "XBEH two, another revision");
        assertNull("changed since: unknown again", PackFreshness.knownXbeSha(xbe));
        assertEquals(GameStore.sha256(xbe), PackFreshness.xbeSha(xbe));
        assertNull(PackFreshness.xbeSha(new File(tmp.getRoot(), "missing.xbe")));
    }

    @Test
    public void toolkitDigestOfTheApk() throws Exception {
        File assets = tmp.newFolder("assets");
        Toolchain.assetsOverride = TestSupport.toolAssets(assets, "1");
        assertEquals("no toolkit.zip.id: no digest", "",
                     Toolchain.toolkitDigest(RuntimeEnvironment.getApplication()));
        File assets2 = tmp.newFolder("assets2");
        Toolchain.assetsOverride = TestSupport.toolAssets(assets2, "1");
        TestSupport.write(new File(assets2, "prepare/toolkit.zip.id"), T1.toUpperCase() + "\n");
        assertEquals(T1, Toolchain.toolkitDigest(RuntimeEnvironment.getApplication()));
        File assets3 = tmp.newFolder("assets3");
        Toolchain.assetsOverride = TestSupport.toolAssets(assets3, "1");
        TestSupport.write(new File(assets3, "prepare/toolkit.zip.id"), "files-123\n");
        assertEquals("not a digest", "", Toolchain.toolkitDigest(RuntimeEnvironment.getApplication()));
    }
}
