package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;

/** Pack validity, one test per reason, and the XBE title ID reader. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PackTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String TID = "4D530004";

    private File files() {
        return tmp.getRoot();
    }

    private void edit(String key, Object value) throws Exception {
        File json = new File(Pack.packDir(files(), TID), "pack.json");
        JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 16));
        if (value == null)
            o.remove(key);
        else
            o.put(key, value);
        TestSupport.write(json, o.toString());
    }

    @Test
    public void valid() throws Exception {
        TestSupport.writePack(files(), TID, 1234);
        assertEquals(Pack.Check.VALID, Pack.check(files(), TID));
        assertEquals(Pack.Check.VALID, Pack.check(files(), TID));
    }

    @Test
    public void validWithAnotherSpellingOfTheTitleId() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("title_id", "0x4d530004");
        assertEquals(Pack.Check.VALID, Pack.check(files(), TID));
    }

    @Test
    public void drop() throws Exception {
        File dir = TestSupport.writePack(files(), TID, 10);
        Pack.drop(files(), "0x4d530004");
        assertFalse(dir.exists());
        assertEquals(Pack.Check.NO_PACK, Pack.check(files(), TID));
        Pack.drop(files(), TID);                  // nothing there: no error
        Pack.drop(files(), "../..");              // not a title ID: nothing removed
        assertTrue(files().isDirectory());
    }

    @Test
    public void noTitleId() {
        assertEquals(Pack.Check.NO_TITLE_ID, Pack.check(files(), null));
        assertEquals(Pack.Check.NO_TITLE_ID, Pack.check(files(), ""));
    }

    @Test
    public void noPack() {
        assertEquals(Pack.Check.NO_PACK, Pack.check(files(), TID));
    }

    @Test
    public void badJson() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        TestSupport.write(new File(Pack.packDir(files(), TID), "pack.json"), "{ not json");
        assertEquals(Pack.Check.BAD_JSON, Pack.check(files(), TID));
    }

    @Test
    public void badFormat() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("format", 2);
        assertEquals(Pack.Check.BAD_FORMAT, Pack.check(files(), TID));
        edit("format", null);
        assertEquals(Pack.Check.BAD_FORMAT, Pack.check(files(), TID));
    }

    @Test
    public void wrongTitle() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("title_id", "4D530005");
        assertEquals(Pack.Check.WRONG_TITLE, Pack.check(files(), TID));
    }

    @Test
    public void wrongShellAbi() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("shell_abi", 2);
        assertEquals(Pack.Check.WRONG_SHELL_ABI, Pack.check(files(), TID));
    }

    @Test
    public void noLib() throws Exception {
        File dir = TestSupport.writePack(files(), TID, 10);
        assertTrue(new File(dir, "libtitle.so").delete());
        assertEquals(Pack.Check.NO_LIB, Pack.check(files(), TID));
        TestSupport.writePack(files(), TID, 10);
        edit("lib", "../../evil.so");
        assertEquals(Pack.Check.NO_LIB, Pack.check(files(), TID));
    }

    @Test
    public void libSize() throws Exception {
        TestSupport.writePack(files(), TID, 10);
        edit("lib_size", 11);
        assertEquals(Pack.Check.LIB_SIZE, Pack.check(files(), TID));
        edit("lib_size", null);
        assertEquals(Pack.Check.LIB_SIZE, Pack.check(files(), TID));
    }

    @Test
    public void titleIdFromSyntheticXbe() throws Exception {
        File x = tmp.newFile("default.xbe");
        TestSupport.write(x, TestSupport.xbe(0x4D530004, "Test"));
        assertEquals("4D530004", Pack.xbeTitleId(x));
        assertEquals("Test", GameStore.xbeTitle(x));
    }

    @Test
    public void titleIdFromConformanceXbe() {
        // tools/conformance/test.xbe: our synthetic XBE (no game content); tests run from android/app.
        File x = new File("../../tools/conformance/test.xbe");
        Assume.assumeTrue(x.isFile());
        assertEquals("12345678", Pack.xbeTitleId(x));
    }

    @Test
    public void notAnXbe() throws Exception {
        File x = tmp.newFile("x.xbe");
        TestSupport.write(x, "not an xbe at all, but long enough? no".getBytes());
        assertNull(Pack.xbeTitleId(x));
        byte[] b = TestSupport.xbe(1, "x");
        TestSupport.le32(b, 0x118, 0x0FFF0);   // certificate below the base
        TestSupport.write(x, b);
        assertNull(Pack.xbeTitleId(x));
        assertNull(Pack.xbeTitleId(null));
    }

    @Test
    public void titleIdSpellings() {
        assertTrue(Pack.sameTitleId("4D530004", "4d530004"));
        assertTrue(Pack.sameTitleId("0x4D530004", "4D530004"));
        assertFalse(Pack.sameTitleId("4D530004", "4D530005"));
        assertFalse(Pack.sameTitleId("zz", "zz"));
        assertFalse(Pack.sameTitleId("", ""));
    }
}
