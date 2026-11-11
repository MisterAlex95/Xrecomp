package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.util.Arrays;

/** The certificate fields the library shows, on synthetic XBEs (plain JUnit: no Android in XbeInfo). */
public class XbeInfoTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File file(byte[] b) throws Exception {
        File f = tmp.newFile();
        TestSupport.write(f, b);
        return f;
    }

    @Test
    public void certificateFields() throws Exception {
        XbeInfo i = XbeInfo.parse(file(TestSupport.xbe(0x4D5300D1, "Synthetic Game", 4, 2, 0x0103)));
        assertNotNull(i);
        assertEquals("4D5300D1", i.titleId);
        assertEquals("Synthetic Game", i.title);
        assertEquals(4, i.region);
        assertEquals(2, i.disc);
        assertEquals(0x0103, i.version);
    }

    @Test
    public void sameTitleIdAsPack() throws Exception {
        File f = file(TestSupport.xbe(0x0000abcd, "x", 1, 0, 0));
        assertEquals(Pack.xbeTitleId(f), XbeInfo.parse(f).titleId);
        assertEquals("0000ABCD", XbeInfo.parse(f).titleId);
    }

    @Test
    public void fortyCharacterTitleAndUnicode() throws Exception {
        String forty = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd";
        assertEquals(forty, XbeInfo.parse(file(TestSupport.xbe(1, forty + "IGNORED"))).title);
        assertEquals("Jeu été", XbeInfo.parse(file(TestSupport.xbe(1, "  Jeu été  "))).title);
        assertEquals("", XbeInfo.parse(file(TestSupport.xbe(1, ""))).title);
    }

    @Test
    public void shortCertificateLeavesTheRestAtZero() throws Exception {
        // Cut right after the title name: region, disc and version are past the end.
        byte[] b = Arrays.copyOf(TestSupport.xbe(0x12345678, "Short", 4, 1, 9), 0x200 + 0x0c + 80);
        XbeInfo i = XbeInfo.parse(file(b));
        assertNotNull(i);
        assertEquals("12345678", i.titleId);
        assertEquals("Short", i.title);
        assertEquals(0, i.region);
        assertEquals(0, i.disc);
        assertEquals(0, i.version);
    }

    @Test
    public void notAnXbe() throws Exception {
        assertNull(XbeInfo.parse(null));
        assertNull(XbeInfo.parse(new File(tmp.getRoot(), "missing.xbe")));
        assertNull(XbeInfo.parse(file(new byte[0x400])));
        assertNull(XbeInfo.parse(file(new byte[10])));
        byte[] b = TestSupport.xbe(1, "x");
        TestSupport.le32(b, 0x118, 0x100);           // certificate before the base address
        assertNull(XbeInfo.parse(file(b)));
        b = TestSupport.xbe(1, "x");
        TestSupport.le32(b, 0x118, 0x10000 + 0x3f0); // certificate name past the end
        assertNull(XbeInfo.parse(file(b)));
    }

    @Test
    public void regionCodes() {
        assertEquals("NA", XbeInfo.regionCode(1));
        assertEquals("JP", XbeInfo.regionCode(2));
        assertEquals("EU", XbeInfo.regionCode(4));
        assertEquals("NA/JP/EU", XbeInfo.regionCode(7));
        assertEquals("", XbeInfo.regionCode(0));
        assertEquals("NA", XbeInfo.regionCode(0x80000001));
    }
}
