"""XEX2 parser (XBA-230): the Xbox 360 executable container, and the PE image inside it.

    img = parse_xex(data, keys=[...])       # keys: 16-byte AES keys from the user, never from this repository
    img.entry, img.base, img.imports, img.image (the mapped PE: bytes at img.base), img.sections, img.pdata

Layout (big-endian, as documented by the Free60 project and Xenia):

    0x00 "XEX2", module flags, PE data offset, reserved, security info offset, optional header count
    0x18 optional headers: count x (key, value). The key's low byte says where the data is:
         0x00 / 0x01 -> `value` is the data itself; 0xFF -> `value` is an offset to a block whose first word is its
         size; anything else -> `value` is an offset to (key & 0xFF) words.
    security info: image size, load address, image flags, the encrypted session key, page descriptors.
    PE data (at the PE data offset): the image, possibly encrypted (AES-128-CBC, IV 0, with the session key, itself
    AES-128-ECB encrypted with the console's key) and compressed ("basic": runs of data then zeros; "normal": LZX).

What is parsed: every optional header is kept raw (`img.headers`) and these are decoded: entry point, image base,
original base, import libraries (name, version, the import records of each library: their address, type
variable / thunk and ordinal, read from the image), TLS info, default stack / heap size, execution info (title id,
media id, version, disc), original PE name, static libraries, system flags, resource info, the file format (encryption
and compression), and the security info (image size, flags, load address, page descriptors). The image is then
decrypted when a key is given (each key is tried; the right one gives "MZ" at the start), expanded (none / basic / normal: LZX),
and its PE headers read: sections and `.pdata` (exact function starts and lengths, what discovery seeds from).

LZX ("normal") compression (XBA-270): the block chain is walked (each block's SHA-1 checked) and the LZX stream decoded
by lzx.py. Not done (documented gaps): delta patches (XEXP), RSA / page digest checks, resource extraction, exports by
name.

No key or console data lives in this repository: keys come from the caller (CLI: --key-file, or $XEX_KEY_FILE).
"""

import os
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from . import aes, lzx

MAGIC = b"XEX2"

# optional header keys
H_RESOURCE_INFO = 0x000002FF
H_FILE_FORMAT_INFO = 0x000003FF
H_BASE_REFERENCE = 0x00000405
H_DELTA_PATCH_DESCRIPTOR = 0x000005FF
H_BOUNDING_PATH = 0x000080FF
H_DEVICE_ID = 0x00008105
H_ORIGINAL_BASE_ADDRESS = 0x00010001
H_ENTRY_POINT = 0x00010100
H_IMAGE_BASE_ADDRESS = 0x00010201
H_IMPORT_LIBRARIES = 0x000103FF
H_CHECKSUM_TIMESTAMP = 0x00018002
H_ENABLED_FOR_CALLCAP = 0x00018102
H_ENABLED_FOR_FASTCAP = 0x00018200
H_ORIGINAL_PE_NAME = 0x000183FF
H_STATIC_LIBRARIES = 0x000200FF
H_TLS_INFO = 0x00020104
H_DEFAULT_STACK_SIZE = 0x00020200
H_DEFAULT_FILESYSTEM_CACHE_SIZE = 0x00020301
H_DEFAULT_HEAP_SIZE = 0x00020401
H_PAGE_HEAP_SIZE_AND_FLAGS = 0x00028002
H_SYSTEM_FLAGS = 0x00030000
H_EXECUTION_INFO = 0x00040006
H_TITLE_WORKSPACE_SIZE = 0x00040201
H_GAME_RATINGS = 0x00040310
H_LAN_KEY = 0x00040404
H_XBOX360_LOGO = 0x000405FF
H_MULTIDISC_MEDIA_IDS = 0x000406FF
H_ALTERNATE_TITLE_IDS = 0x000407FF
H_ADDITIONAL_TITLE_MEMORY = 0x00040801
H_EXPORTS_BY_NAME = 0x00E10402

HEADER_NAMES = {v: k[2:].lower() for k, v in globals().items() if k.startswith("H_")}

ENC_NONE, ENC_NORMAL = 0, 1
COMP_NONE, COMP_BASIC, COMP_NORMAL, COMP_DELTA = 0, 1, 2, 3
COMPRESSION_NAMES = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}

IMAGE_PAGE_SIZE_4KB = 0x10000000

IMPORT_VARIABLE, IMPORT_THUNK = 0, 1

PE_MACHINE_POWERPCBE = 0x01F2
SCN_CNT_CODE, SCN_MEM_EXECUTE = 0x00000020, 0x20000000


class XexError(ValueError):
    pass


def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def _u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def version_str(v):
    """xex2_version: major 4 bits, minor 4, build 16, qfe 8."""
    return "%d.%d.%d.%d" % (v >> 28, (v >> 24) & 15, (v >> 8) & 0xFFFF, v & 0xFF)


@dataclass
class ImportRecord:
    addr: int          # where the record sits in the image
    kind: int          # IMPORT_VARIABLE (the loader stores the export's address there) or IMPORT_THUNK
    ordinal: int
    lib_index: int


@dataclass
class ImportLibrary:
    name: str
    id: int
    version: int
    min_version: int
    record_addrs: List[int]
    records: List[ImportRecord] = field(default_factory=list)

    def label(self):
        return "%s %s (min %s)" % (self.name, version_str(self.version), version_str(self.min_version))


@dataclass
class Section:
    name: str
    va: int            # absolute
    size: int
    flags: int

    @property
    def code(self):
        return bool(self.flags & (SCN_CNT_CODE | SCN_MEM_EXECUTE))


@dataclass
class PageDescriptor:
    pages: int
    info: int          # 1 code, 2 data, 3 read-only data
    digest: bytes


@dataclass
class XexImage:
    module_flags: int
    pe_offset: int
    security_offset: int
    headers: Dict[int, object]      # key -> int (inline) or bytes
    entry: int = 0
    base: int = 0
    original_base: int = 0
    image_size: int = 0
    image_flags: int = 0
    load_address: int = 0
    encryption: int = ENC_NONE
    compression: int = COMP_NONE
    basic_blocks: List[tuple] = field(default_factory=list)
    lzx_window: int = 0
    lzx_block: tuple = (0, b"")     # the first compressed block: size, SHA-1
    session_key_enc: bytes = b""
    pages: List[PageDescriptor] = field(default_factory=list)
    libraries: List[ImportLibrary] = field(default_factory=list)
    tls: Optional[dict] = None
    execution: Optional[dict] = None
    stack_size: int = 0
    heap_size: int = 0
    system_flags: int = 0
    original_name: str = ""
    static_libraries: List[tuple] = field(default_factory=list)
    resources: List[tuple] = field(default_factory=list)
    image: Optional[bytes] = None   # the mapped PE image (None: encrypted with no key, or not decodable)
    image_note: str = ""
    key_used: Optional[int] = None  # index in the keys given
    sections: List[Section] = field(default_factory=list)
    pdata: List[tuple] = field(default_factory=list)  # (start, length in bytes)
    pe_machine: int = 0

    # ---- the image interface tools.ppc.discover wants ----
    def in_code(self, a):
        if self.image is None:
            return False
        for s in self.sections:
            if s.code and s.va <= a and a + 4 <= s.va + s.size:
                return a + 4 <= self.base + len(self.image)
        return False

    def u32(self, a):
        return _u32(self.image, a - self.base)

    def read(self, a, n):
        o = a - self.base
        if self.image is None or o < 0 or o + n > len(self.image):
            return None
        return self.image[o:o + n]

    @property
    def page_size(self):
        return 0x1000 if self.image_flags & IMAGE_PAGE_SIZE_4KB else 0x10000

    @property
    def imports(self):
        return [r for lib in self.libraries for r in lib.records]

    def code_ranges(self):
        return [(s.va, s.va + s.size) for s in self.sections if s.code]


def _opt_header(data, key, value):
    kind = key & 0xFF
    if kind in (0x00, 0x01):
        return value
    if kind == 0xFF:
        if value + 4 > len(data):
            raise XexError("optional header %08X: offset out of file" % key)
        size = _u32(data, value)
        return bytes(data[value:value + size])
    return bytes(data[value:value + kind * 4])


def _strings(blob):
    out = []
    o = 0
    while o < len(blob):
        e = blob.find(b"\0", o)
        if e < 0:
            e = len(blob)
        s = blob[o:e]
        if s:
            out.append(s.decode("latin-1"))
        o = e + 1
        while o < len(blob) and blob[o] == 0:
            o += 1
    return out


def _import_libraries(blob):
    """xex2_opt_import_libraries: size, string table (size, count, names), then the libraries."""
    if len(blob) < 12:
        raise XexError("import libraries header too short")
    size, str_size, str_count = struct.unpack_from(">III", blob, 0)
    names = _strings(blob[12:12 + str_size])
    libs = []
    o = 12 + str_size
    while o + 0x28 <= min(size, len(blob)):
        lib_size = _u32(blob, o)
        lib_id, ver, ver_min = struct.unpack_from(">III", blob, o + 0x18)
        name_index, count = struct.unpack_from(">HH", blob, o + 0x24)
        addrs = list(struct.unpack_from(">%dI" % count, blob, o + 0x28))
        name = names[name_index] if name_index < len(names) else "lib%d" % name_index
        libs.append(ImportLibrary(name, lib_id, ver, ver_min, addrs))
        if lib_size < 0x28:
            raise XexError("import library record too short")
        o += lib_size
    if str_count and len(libs) != str_count:
        # the string table counts names, which is one per library in every XEX seen; keep going but say so
        pass
    return libs


def _security(img, data):
    o = img.security_offset
    if o + 0x184 > len(data):
        raise XexError("security info out of file")
    img.image_size = _u32(data, o + 4)
    img.image_flags = _u32(data, o + 0x10C)
    img.load_address = _u32(data, o + 0x110)
    img.session_key_enc = bytes(data[o + 0x150:o + 0x160])
    n = _u32(data, o + 0x180)
    p = o + 0x184
    for _ in range(n):
        if p + 24 > len(data):
            raise XexError("page descriptors out of file")
        v = _u32(data, p)
        img.pages.append(PageDescriptor(v >> 4, v & 15, bytes(data[p + 4:p + 24])))
        p += 24


def _file_format(img, blob):
    if len(blob) < 8:
        raise XexError("file format header too short")
    size, enc, comp = struct.unpack_from(">IHH", blob, 0)
    img.encryption, img.compression = enc, comp
    if comp == COMP_BASIC:
        img.basic_blocks = [struct.unpack_from(">II", blob, o) for o in range(8, min(size, len(blob)) - 7, 8)]
    elif comp == COMP_NORMAL:
        if len(blob) >= 36:  # (shorter: no block chain, the image is reported as not decodable)
            img.lzx_window = _u32(blob, 8)
            img.lzx_block = (_u32(blob, 12), bytes(blob[16:36]))


def _decrypt(img, payload, keys):
    """Try each key; the right one gives a PE ("MZ") once the image is expanded. Returns (bytes, key index)."""
    for k, key in enumerate(keys):
        session = aes.ecb_decrypt(key, img.session_key_enc)
        plain = aes.cbc_decrypt(session, payload)
        try:
            image = _expand(img, plain)
        except XexError:
            continue
        if image[:2] == b"MZ":
            return image, k
    return None, None


def _expand(img, payload):
    if img.compression == COMP_NONE:
        return bytes(payload[:img.image_size]) if img.image_size else bytes(payload)
    if img.compression == COMP_BASIC:
        out = bytearray()
        o = 0
        for data_size, zero_size in img.basic_blocks:
            if o + data_size > len(payload):
                raise XexError("basic compression block runs past the data")
            out += payload[o:o + data_size]
            out += bytes(zero_size)
            o += data_size
        return bytes(out)
    if img.compression == COMP_NORMAL:
        try:
            if not img.lzx_block[0]:
                raise lzx.LzxError("no compressed block in the file format header")
            stream = lzx.xex_deblock(payload, img.lzx_block[0], img.lzx_block[1])
            return lzx.decompress(stream, lzx.window_bits_of(img.lzx_window), img.image_size)
        except lzx.LzxError as ex:
            raise XexError("normal (LZX) compression: %s" % ex)
    raise XexError("compression type %d (%s) not supported" % (img.compression,
                                                               COMPRESSION_NAMES.get(img.compression, "?")))


def _pe(img):
    """The PE headers at the start of the mapped image (little-endian, as on Windows)."""
    b = img.image
    if b[:2] != b"MZ" or len(b) < 0x40:
        raise XexError("the image does not start with a PE (MZ) header")
    lfanew = struct.unpack_from("<I", b, 0x3C)[0]
    if b[lfanew:lfanew + 4] != b"PE\0\0":
        raise XexError("no PE signature")
    machine, nsec = struct.unpack_from("<HH", b, lfanew + 4)
    opt_size = struct.unpack_from("<H", b, lfanew + 20)[0]
    img.pe_machine = machine
    so = lfanew + 24 + opt_size
    for k in range(nsec):
        o = so + 40 * k
        name = b[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, _raw = struct.unpack_from("<IIII", b, o + 8)
        flags = struct.unpack_from("<I", b, o + 36)[0]
        img.sections.append(Section(name, img.base + va, vsize or rsize, flags))
    for s in img.sections:
        if s.name == ".pdata":
            raw = img.read(s.va, s.size) or b""
            for o in range(0, len(raw) - 7, 8):
                start, info = struct.unpack_from(">II", raw, o)
                if start == 0:
                    break
                # IMAGE_CE_RUNTIME_FUNCTION_ENTRY: prolog length 8 bits, function length 22 bits (in words), flags
                img.pdata.append((start, ((info >> 8) & 0x3FFFFF) * 4))


def _resolve_imports(img):
    for li, lib in enumerate(img.libraries):
        lib.records = []
        for a in lib.record_addrs:
            raw = img.read(a, 4)
            if raw is None:
                continue
            v = _u32(raw, 0)
            lib.records.append(ImportRecord(a, v >> 24, v & 0xFFFF, (v >> 16) & 0xFF))


def parse_xex(data, keys=()):
    """An XexImage; XexError on a malformed file. keys: 16-byte AES keys to try on an encrypted image."""
    try:
        return _parse(bytes(data), keys)
    except struct.error as ex:
        raise XexError("truncated or malformed XEX2: %s" % ex)


def _parse(data, keys):
    if len(data) < 0x18 or data[:4] != MAGIC:
        raise XexError("not an XEX2 file")
    flags, pe_off, _res, sec_off, count = struct.unpack_from(">IIIII", data, 4)
    if 0x18 + 8 * count > len(data) or pe_off > len(data):
        raise XexError("header directory out of file")
    headers = {}
    for k in range(count):
        key, value = struct.unpack_from(">II", data, 0x18 + 8 * k)
        headers[key] = _opt_header(data, key, value)
    img = XexImage(flags, pe_off, sec_off, headers)
    _security(img, data)
    h = headers.get
    img.entry = h(H_ENTRY_POINT, 0)
    img.base = h(H_IMAGE_BASE_ADDRESS, 0) or img.load_address
    img.original_base = h(H_ORIGINAL_BASE_ADDRESS, 0)
    img.stack_size = h(H_DEFAULT_STACK_SIZE, 0)
    img.heap_size = h(H_DEFAULT_HEAP_SIZE, 0)
    img.system_flags = h(H_SYSTEM_FLAGS, 0)
    if H_FILE_FORMAT_INFO in headers:
        _file_format(img, headers[H_FILE_FORMAT_INFO])
    if H_IMPORT_LIBRARIES in headers:
        img.libraries = _import_libraries(headers[H_IMPORT_LIBRARIES])
    t = h(H_TLS_INFO)
    if isinstance(t, bytes) and len(t) >= 16:
        slots, raw_addr, data_size, raw_size = struct.unpack_from(">IIII", t, 0)
        img.tls = {"slots": slots, "raw_data_address": raw_addr, "data_size": data_size, "raw_data_size": raw_size}
    e = h(H_EXECUTION_INFO)
    if isinstance(e, bytes) and len(e) >= 24:
        media, ver, base_ver, title = struct.unpack_from(">IIII", e, 0)
        plat, table, disc, discs = struct.unpack_from(">BBBB", e, 16)
        img.execution = {"media_id": media, "version": version_str(ver), "base_version": version_str(base_ver),
                         "title_id": title, "platform": plat, "executable_table": table, "disc": disc,
                         "disc_count": discs, "savegame_id": _u32(e, 20)}
    n = h(H_ORIGINAL_PE_NAME)
    if isinstance(n, bytes) and len(n) > 4:
        img.original_name = n[4:].split(b"\0")[0].decode("latin-1")
    s = h(H_STATIC_LIBRARIES)
    if isinstance(s, bytes):
        for o in range(4, len(s) - 15, 16):
            name = s[o:o + 8].rstrip(b"\0").decode("latin-1")
            major, minor, build, qfe = struct.unpack_from(">HHHH", s, o + 8)
            img.static_libraries.append((name, "%d.%d.%d.%d" % (major, minor, build, qfe & 0x7FFF)))
    r = h(H_RESOURCE_INFO)
    if isinstance(r, bytes):
        for o in range(4, len(r) - 15, 16):
            name = r[o:o + 8].rstrip(b"\0").decode("latin-1")
            addr, size = struct.unpack_from(">II", r, o + 8)
            img.resources.append((name, addr, size))

    payload = data[pe_off:]
    try:
        if img.encryption == ENC_NONE:
            img.image = _expand(img, payload)
        elif not keys:
            img.image_note = "encrypted: pass the console key (--key-file) to decrypt the image"
        else:
            img.image, img.key_used = _decrypt(img, payload, list(keys))
            if img.image is None:
                img.image_note = "encrypted: none of the keys given decrypts the image"
    except XexError as ex:
        img.image = None
        img.image_note = str(ex)
    if img.image is not None:
        _pe(img)
        _resolve_imports(img)
    return img


def load_keys(paths=(), env=True):
    """16-byte AES keys from files (raw 16 bytes, or 32 hex digits on the first line). $XEX_KEY_FILE adds one more
    (several paths separated by os.pathsep)."""
    paths = list(paths)
    if env and os.environ.get("XEX_KEY_FILE"):
        paths += os.environ["XEX_KEY_FILE"].split(os.pathsep)
    keys = []
    for p in paths:
        with open(p, "rb") as f:
            raw = f.read()
        if len(raw) == 16:
            keys.append(raw)
            continue
        text = raw.decode("ascii", "replace").strip().split()[0] if raw.strip() else ""
        try:
            k = bytes.fromhex(text)
        except ValueError:
            k = b""
        if len(k) != 16:
            raise XexError("%s: expected 16 raw bytes or 32 hex digits" % p)
        keys.append(k)
    return keys


def summary(img):
    """A dict for --json and the tests."""
    return {
        "entry": img.entry, "base": img.base, "image_size": img.image_size, "module_flags": img.module_flags,
        "encryption": img.encryption, "compression": COMPRESSION_NAMES.get(img.compression, str(img.compression)),
        "page_size": img.page_size, "pages": len(img.pages),
        "libraries": [{"name": lib.name, "version": version_str(lib.version),
                       "imports": [(r.ordinal, "thunk" if r.kind == IMPORT_THUNK else "variable", r.addr)
                                   for r in lib.records]} for lib in img.libraries],
        "tls": img.tls, "execution": img.execution, "stack_size": img.stack_size, "heap_size": img.heap_size,
        "original_name": img.original_name, "static_libraries": img.static_libraries,
        "sections": [(s.name, s.va, s.size, s.flags) for s in img.sections],
        "pdata_functions": len(img.pdata), "image": img.image is not None, "note": img.image_note,
        "headers": sorted(HEADER_NAMES.get(k, "%08X" % k) for k in img.headers),
    }


def plain_xex(data, img):
    """The same XEX2 with its image stored plain (no encryption, no compression): what the runtime's loader
    (consoles/x360/runtime/x360_load.c) reads. img: parse_xex(data, keys) with the image decoded."""
    if img.image is None:
        raise XexError("no image to write: %s" % img.image_note)
    out = bytearray(data[:img.pe_offset])
    count = _u32(out, 0x14)
    for k in range(count):
        key, value = struct.unpack_from(">II", out, 0x18 + 8 * k)
        if key == H_FILE_FORMAT_INFO:
            struct.pack_into(">HH", out, value + 4, ENC_NONE, COMP_NONE)
            break
    else:
        raise XexError("no file format header")
    return bytes(out) + img.image
