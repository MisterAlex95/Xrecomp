# XBE File Format Reference

> Part of the [Documentation Index](../INDEX.md)

The XBE (Xbox Executable) format is the native executable format for the original Microsoft Xbox console. It is analogous to Windows PE (Portable Executable) but with Xbox-specific extensions for kernel thunks, digital signatures, and section management.

Reference: https://xboxdevwiki.net/Xbe

## Magic and Identification

- **Magic bytes**: `XBEH` (0x48454258 little-endian) at file offset 0
- File extension: `.xbe`
- Always found as `default.xbe` at the root of an Xbox disc image

## Header Layout

The XBE header begins at file offset 0 and contains the following fields:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x0000 | 4 | Magic | `XBEH` (0x48454258) |
| 0x0004 | 256 | DigitalSignature | RSA signature over header |
| 0x0104 | 4 | BaseAddress | Virtual base address (always 0x00010000) |
| 0x0108 | 4 | SizeOfHeaders | Total header size in bytes |
| 0x010C | 4 | SizeOfImage | Total image size when loaded |
| 0x0110 | 4 | SizeOfImageHeader | Size of the image header portion |
| 0x0114 | 4 | TimeDateStamp | Build timestamp (Unix epoch) |
| 0x0118 | 4 | CertificateAddress | VA of the certificate structure |
| 0x011C | 4 | NumberOfSections | Number of section headers |
| 0x0120 | 4 | SectionHeadersAddress | VA of the section header array |
| 0x0124 | 4 | InitFlags | Initialization flags |
| 0x0128 | 4 | EntryPoint | Encoded entry point address |
| 0x012C | 4 | TlsAddress | VA of TLS directory |
| 0x0130 | 4 | PeStackCommit | Default thread stack commit size |
| 0x0134 | 4 | PeHeapReserve | Default heap reserve size |
| 0x0138 | 4 | PeHeapCommit | Default heap commit size |
| 0x013C | 4 | PeBaseAddress | Base address of the original PE image |
| 0x0140 | 4 | PeSizeOfImage | PE-compatible image size |
| 0x0144 | 4 | PeChecksum | PE-compatible checksum |
| 0x0148 | 4 | PeTimeDateStamp | PE-compatible timestamp |
| 0x014C | 4 | DebugPathnameAddress | VA of debug build path string |
| 0x0150 | 4 | DebugFilenameAddress | VA of debug build filename string |
| 0x0154 | 4 | DebugUnicodeFilenameAddress | VA of Unicode debug filename |
| 0x0158 | 4 | KernelThunkAddress | Encoded kernel thunk table address |
| 0x015C | 4 | NonKernelImportDirectoryAddress | Non-kernel import directory |
| 0x0160 | 4 | NumberOfLibraryVersions | Number of library version entries |
| 0x0164 | 4 | LibraryVersionsAddress | VA of library version array |
| 0x0168 | 4 | KernelLibraryVersionAddress | VA of kernel lib version entry |
| 0x016C | 4 | XapiLibraryVersionAddress | VA of XAPI lib version entry |
| 0x0170 | 4 | LogoBitmapAddress | VA of the Xbox logo bitmap |
| 0x0174 | 4 | LogoBitmapSize | Size of the logo bitmap |

(Before XBA-124 this table skipped PeBaseAddress and gave every field from 0x013C on 4 bytes early;
`tools/xbe_parser` and the loader always used the offsets above.)

### Base Address

The base address is always **0x00010000** for all retail and debug Xbox executables. This is the virtual address at which the XBE image header is loaded. All other VA fields in the header are relative to this base. The Xbox kernel maps the executable starting at this fixed address.

### Entry Point Encoding

The entry point address at offset 0x0128 is XOR-encoded to prevent trivial modification:

- **Retail XBEs**: XOR key = `0xA8FC57AB`
- **Debug XBEs**: XOR key = `0x94859D4B`

To decode:

```c
uint32_t encoded_entry = *(uint32_t*)(xbe + 0x0128);

// Try retail key first
uint32_t entry_retail = encoded_entry ^ 0xA8FC57AB;
// If entry falls within .text section, it's retail
if (entry_retail >= text_va && entry_retail < text_va + text_size) {
    entry_point = entry_retail;
} else {
    // Try debug key
    entry_point = encoded_entry ^ 0x94859D4B;
}
```

### Kernel Thunk Address Encoding

The kernel thunk table address at offset 0x0158 is similarly XOR-encoded:

- **Retail XBEs**: XOR key = `0x5B6D40B6`
- **Debug XBEs**: XOR key = `0xEFB1F152`

The decoded address points to an array of 32-bit ordinal values (terminated by 0) that index into the Xbox kernel's export table.

### Which key pair (tools/xbe_parser)

One key set signs an image, so the entry point and the thunk address are decoded with the same pair. The parser
scores both pairs (entry point inside an executable section: 2; entry point inside the image: 1; thunk address inside
the image: 1) and keeps the better one, retail on a tie. When neither scores, it keeps the retail decoding and adds a
warning. `XBEHeader.entry_key` says which (`retail`, `debug`, `unknown`); the triage reports it.

### Odd headers (XBA-124)

What the parser and the loader (`xbox_MemoryLayoutInit`) accept, with a warning, and what they refuse:

| Case | Parser (`tools/xbe_parser`) | Loader |
|---|---|---|
| `SizeOfHeaders` over a page | VAs below base + `SizeOfHeaders` are the header's | copies up to 64 KB of header |
| No certificate (address 0 or outside the file) | title ID 0, empty name, warning | no title ID, region or window title read |
| Certificate cut by the end of the file | the missing fields read 0, warning | only the fields inside the file are read |
| Library table missing or cut short | the entries inside the file, warning | not read |
| Thunk table or TLS directory outside the file | none, warning | as before |
| Section raw data past the end of the file | warning | section left zeroed, `ERROR` line |
| Header cut before 0x178, bad magic, section table outside the file | `ValueError` | sections not loaded |

All offsets are compared in 64 bits in the loader: an odd address no longer wraps back into the buffer (a certificate
at `0x0000FFF0` read 4 GB past it). Tests: `tools/xbe_parser/test_xbe_parser.py` on images from our own writer
(`tools/xbe_parser/synth.py`), `tests/xbe_sections` (layout).

## Section Table

The section header array begins at `SectionHeadersAddress` and contains `NumberOfSections` entries. Each section header is 56 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | Flags | Section flags (see below) |
| 0x04 | 4 | VirtualAddress | VA where section is loaded |
| 0x08 | 4 | VirtualSize | Size in memory (may exceed raw size) |
| 0x0C | 4 | RawAddress | File offset of section data |
| 0x10 | 4 | RawSize | Size of data in the XBE file |
| 0x14 | 4 | SectionNameAddress | VA of the section name string |
| 0x18 | 4 | SectionReferenceCount | Reference count for demand loading |
| 0x1C | 4 | HeadSharedPageRefCountAddress | VA of head page ref count |
| 0x20 | 4 | TailSharedPageRefCountAddress | VA of tail page ref count |
| 0x24 | 20 | SectionDigest | SHA-1 hash of section contents |

### Section Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | Writable | Section is writable at runtime |
| 1 | Preload | Section is loaded at startup (vs demand-loaded) |
| 2 | Executable | Section contains executable code |
| 3 | InsertedFile | Section was inserted by the linker |
| 4 | HeadPageReadOnly | First page is read-only |
| 5 | TailPageReadOnly | Last page is read-only |

### Sections loaded on demand (XBA-124)

On the console the loader maps only the sections marked **Preload**, each with `SectionReferenceCount` 1 and one count
more on the shared head and tail pages (`HeadSharedPageRefCountAddress` / `Tail...` point at 16-bit counters in the
header: two sections that meet inside a page share it). A title brings the others in with XAPI's `XLoadSection(name)`,
which calls the kernel's `XeLoadSection(header)`: on 0 → 1 the kernel commits the pages and reads the section from the
image (file data, the rest zero); `XeUnloadSection` on 1 → 0 releases them. A section unloaded and loaded again comes
back as it is in the file. Sections may share virtual addresses: they are alternatives (overlays), one at a time.

Here (`src/kernel/xbox_xbe_sections.c`): every section is copied at boot and stays mapped, which covers a title that
only loads. On top of that the module keeps the console's reference counts (Preload: 1 at boot, the shared-page
counters), and on 0 → 1 copies a section again from the image when its bytes may have changed: a writable one (fresh
data, zeroed BSS after a full unload) or one whose addresses another section held. Read-only sections are not copied
again (nothing writes them, their pages are never released). At boot, of two overlapping sections the Preload one is in
memory (the layout copies in table order, so the later one used to win), and each overlap is printed
(`[XBE] sections A and B share ...`). `RECOMP_XBE_SECTIONS=0` keeps the old bookkeeping (the count only).

**Which section the static tools read (XBA-138).** Every tool that maps an address to file bytes (the parser for
header structures, `tools.disasm`, `tools.func_id`, `tools.abi_analysis`, the lifter's `tools/recomp/config.py`) picks,
among the sections covering it, by one rule (`lookup_order` in `tools/xbe_parser/xbe_parser.py`):

1. Code before data. Where a code section overlaps a data section, the address's code has one meaning, the code
   section's bytes, even when the data section is the one in memory at boot: the lifted code runs once
   `XeLoadSection` brought its section in, and the data section's bytes are data the lifted code reads from guest
   memory at run time.
2. Then the section in memory at boot: the last Preload one in table order, else the last one in the table (the order
   the runtime above copies them in).

Before, each tool took the first covering section of its own list (table order, or sorted by address), so preloaded
data listed before an overlay's code made the lifter decode the data. Overlapping data and code over data are thereby
supported; `image_report` (the parser's JSON, `tools.triage`) names under `read` the section whose bytes were used.
`tools/xbe_parser/test_xbe_parser.py::test_every_static_reader_picks_the_same_overlapping_section` builds one
synthetic XBE with the three cases and checks all five readers.

**Overlapping code is a blocker**, and stays one: it is not well defined statically. Which code sits at the address
depends on the last `XeLoadSection` at run time, and the lifted C has one function per address. Rule 2 picks the one
lifted (the one in memory at boot, which is also what the runtime starts with); when the title loads the other, its
calls run the first one's code. `tools.triage` and `tools.prepare` (`BLOCKER: sections A and B both hold code ...`)
name it. Supporting it means lifting each overlay's functions under its own section, giving the dispatch one table per
overlay and switching the entries on `XeLoadSection`/`XeUnloadSection`, which no title in the library has needed so
far (the triage's « Overlaps » column will say).
What breaks with everything at boot, in short: nothing for a title that loads before use and never relies on a reload;
a reloaded data section's state (fixed here); overlapping data and code over data (fixed here, and read alike by the
static tools); overlapping code (blocker).

## Title updates and downloadable content (XBA-124)

**On a console.** An Xbox Live title update is downloaded by the title (the online APIs) into the hard disk's
`E:\TDATA\<title id>\$u\` — an updated `default.xbe` and the files it replaces — and the launch path then runs that
`default.xbe` instead of the disc's, with `D:` still the disc. Downloadable content goes to
`E:\TDATA\<title id>\$c\<offering id>\` (a `ContentMeta.xbx` and the package's files). XAPI mounts `T:` on
`TDATA\<title id>`; the title lists its content with `XFindFirstContent` / `XFindNextContent` (a directory search of
`T:\$c` plus a signed metadata file per package), and `XMountAlternateTitle` mounts another title's TDATA as a drive
(sharing data between titles of one publisher). `XLaunchNewImage` starts another XBE (the dashboard, a demo, an update).
All of these except the kernel calls under them are XAPI code linked into the title, so they are lifted with it.

**Here.** `T:` maps to `<user data>/TDATA/<title id>` (the save folder, `kernel_path.c`). Nothing prepares or runs an
update XBE: the title was lifted from the disc's `default.xbe`, and that is what runs.
- With nothing installed (every player), `T:\$c` does not exist: the open fails with a not-found status and
  `XFindFirstContent` returns `INVALID_HANDLE_VALUE` — no content, cleanly. An empty `$c` lists nothing.
  `tests/kernel_content_posix` pins both.
- Content the owner copies into `TDATA/<title id>/$c/` is listed like any folder; whether the title then accepts the
  package (its signature check) is untested, and the runtime counts the packages at start-up
  (`[PATH] ... content package(s), left to the title's own XContent calls`).
- An update XBE the owner copies to `TDATA/<title id>/$u/default.xbe` is **not used**; the runtime says so at start-up
  (`a title update for <id>, not used`) and `tools.triage --tdata DIR` reports it (version, newer than the disc's or
  not). Using one would mean preparing the update XBE instead of the disc's (a different lift, its own pack): not done.

### Common Sections

| Name | Typical VA Range | Purpose |
|------|-----------------|---------|
| .text | 0x00011000+ | Executable code (game + CRT + engine) |
| .rdata | after .text | Read-only data (strings, vtables, const data) |
| .data | after .rdata | Initialized read-write data |
| .bss | (virtual only) | Zero-initialized data (extends .data's virtual size) |
| XSIMAGE | varies | Xbox dashboard section |
| XSIMAGE1 | varies | Xbox dashboard section (alternate) |
| $$XONLINE | varies | Xbox Live online code |
| $$XTIMAGE | varies | Title image data |

Games using the XDK statically link libraries, which appear as named sections:

| Section Name | Library |
|-------------|---------|
| D3D | Direct3D 8 (LTCG variant) |
| DSOUND | DirectSound |
| XMV | Xbox Media Video |
| XONLINE | Xbox Live networking |
| XNET | Xbox networking stack |
| XGRPH | Xbox graphics utilities |
| XPP | Xbox peripheral port (input) |
| WMADEC | Windows Media Audio decoder |

## Library Version Table

Located at `LibraryVersionsAddress`, containing `NumberOfLibraryVersions` entries. Each entry is 16 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 8 | LibraryName | ASCII name, null-padded to 8 chars |
| 0x08 | 2 | MajorVersion | Major version number |
| 0x0A | 2 | MinorVersion | Minor version number |
| 0x0C | 2 | BuildVersion | Build number (often indicates XDK version) |
| 0x0E | 2 | Flags | Library flags (QFE version + approval) |

The build version often corresponds to the XDK version used to compile the game. Common values include 4361, 4627, 5028, 5455, 5558, 5788, 5849, 5933.

## Certificate

Located at `CertificateAddress`, the certificate contains game metadata:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | Size | Size of certificate structure |
| 0x04 | 4 | TimeDateStamp | Certificate timestamp |
| 0x08 | 4 | TitleId | Unique title identifier |
| 0x0C | 80 | TitleName | Unicode game title (null-terminated) |
| 0x5C | 64 | AlternateTitleIds | Array of 16 alternate title IDs |
| 0x9C | 4 | AllowedMedia | Bitmask of allowed media types |
| 0xA0 | 4 | GameRegion | Region bitmask (NA=1, Japan=2, RestOfWorld=4) |
| 0xA4 | 4 | GameRatings | Content ratings bitmask |
| 0xA8 | 4 | DiskNumber | Disc number (for multi-disc games) |
| 0xAC | 4 | Version | Certificate version |
| 0xB0 | 16 | LanKey | LAN encryption key |
| 0xC0 | 16 | SignatureKey | Title signature key |
| 0xD0 | 256 | AlternateSignatureKeys | 16 alternate signature keys |
| 0x1D0 | 4 | OriginalCertificateSize | newer certificates only (`Size` 0x1EC): the fields from here on |
| 0x1D4 | 4 | OnlineServiceId | |
| 0x1D8 | 4 | SecurityFlags | |
| 0x1DC | 16 | CodeEncryptionKey | |

### Allowed Media Types

| Bit | Media Type |
|-----|-----------|
| 0 | Hard disk |
| 1 | DVD-X2 (Xbox disc) |
| 2 | DVD-CD |
| 3 | CD-ROM |
| 4 | DVD-5 RO |
| 5 | DVD-9 RO |
| 6 | DVD-5 RW |
| 7 | DVD-9 RW |
| 11 | Nonsecure hard disk |
| 12 | Nonsecure mode |

## TLS (Thread Local Storage) Directory

Located at `TlsAddress`, the TLS directory defines thread-local storage:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | RawDataStartAddress | VA of TLS data template start |
| 0x04 | 4 | RawDataEndAddress | VA of TLS data template end |
| 0x08 | 4 | TlsIndexAddress | VA of TLS index variable |
| 0x0C | 4 | TlsCallbackAddress | VA of TLS callback array |
| 0x10 | 4 | SizeOfZeroFill | Bytes of zero-fill after template |
| 0x14 | 4 | Characteristics | TLS characteristics flags |

The Xbox kernel uses the TLS directory to set up per-thread storage accessed via the FS segment register (fs:[0x28] on Xbox).

## Kernel Thunk Table

The kernel thunk table is the Xbox equivalent of an import address table. It is an array of 32-bit values at the decoded `KernelThunkAddress`:

- Before loading: each entry contains a kernel export ordinal (with bit 31 set)
- After loading: the Xbox kernel replaces each ordinal with the actual function/data pointer

The table is terminated by a 0 entry.

### Ordinal Format

```
Raw value:  0x80000001
Ordinal:    value & 0x7FFFFFFF = 1 (AvGetSavedDataAddress)
```

Games typically import 100-200 kernel functions. The Xbox kernel exports up to 366 functions and data objects. See [kernel-exports.md](kernel-exports.md) for the full list of commonly imported ordinals.

## Practical Notes for Recompilation

1. **Memory layout reproduction**: The base address 0x00010000 must be reproduced on the host system. Use `CreateFileMapping` + `MapViewOfFileEx` to place data sections at their original VAs.

2. **Section data**: Copy .rdata and .data from the XBE file at their raw offsets to their VAs. Zero-fill the BSS region (virtual size minus raw size in .data).

3. **Kernel thunks**: Decode the thunk table, read the ordinals, and replace each with a pointer to your host implementation of that kernel function.

4. **Entry point**: After decoding, the entry point is a VA within .text. For static recompilation, this is the function to call after all initialization is complete.

5. **XDK version**: The library build version (especially XAPILIB) indicates which XDK the game was compiled with, which affects structure layouts and available APIs.
