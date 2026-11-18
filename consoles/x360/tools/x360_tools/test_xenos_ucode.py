"""xenos_ucode (XBA-270): instructions packed from Mesa freedreno's a2xx field order (instr-a2xx.h, MIT; Adreno 200
descends from Xenos), decoded with the Xenia-based layouts of xenos_ucode.py: two independent descriptions of the same
bits must agree (where they differ, see xenos_ucode.py's docstring: the tests stay inside the shared ranges)."""

from .xenos_ucode import decode_alu, decode_cf, decode_fetch, disassemble, unpack_cf

# Mesa's structures, lowest bits first: (field, width)
MESA_CF_EXEC = [("address", 9), ("reserved0", 3), ("count", 3), ("yeild", 1), ("serialize", 12), ("vc", 6),
                ("bool_addr", 8), ("condition", 1), ("address_mode", 1), ("opc", 4)]
MESA_CF_LOOP = [("address", 10), ("reserved0", 6), ("loop_id", 5), ("reserved1", 22), ("address_mode", 1),
                ("opc", 4)]
MESA_CF_JMP = [("address", 10), ("reserved0", 3), ("force_call", 1), ("predicated_jmp", 1), ("reserved1", 18),
               ("direction", 1), ("bool_addr", 8), ("condition", 1), ("address_mode", 1), ("opc", 4)]
MESA_CF_ALLOC = [("size", 4), ("reserved0", 36), ("no_serial", 1), ("buffer_select", 2), ("alloc_mode", 1),
                 ("opc", 4)]
MESA_ALU = [  # dword 0
    ("vector_dest", 6), ("vector_dest_rel", 1), ("low_precision_16b_fp", 1), ("scalar_dest", 6),
    ("scalar_dest_rel", 1), ("export_data", 1), ("vector_write_mask", 4), ("scalar_write_mask", 4),
    ("vector_clamp", 1), ("scalar_clamp", 1), ("scalar_opc", 6),
    # dword 1
    ("src3_swiz", 8), ("src2_swiz", 8), ("src1_swiz", 8), ("src3_reg_negate", 1), ("src2_reg_negate", 1),
    ("src1_reg_negate", 1), ("pred_select", 2), ("relative_addr", 1), ("const_1_rel_abs", 1), ("const_0_rel_abs", 1),
    # dword 2
    ("src3_reg", 8), ("src2_reg", 8), ("src1_reg", 8), ("vector_opc", 5), ("src3_sel", 1), ("src2_sel", 1),
    ("src1_sel", 1)]
MESA_VTX = [("opc", 5), ("src_reg", 6), ("src_reg_am", 1), ("dst_reg", 6), ("dst_reg_am", 1), ("must_be_one", 1),
            ("const_index", 5), ("const_index_sel", 2), ("reserved0", 3), ("src_swiz", 2),
            ("dst_swiz", 12), ("format_comp_all", 1), ("num_format_all", 1), ("signed_rf_mode_all", 1),
            ("reserved1", 1), ("format", 6), ("reserved2", 2), ("exp_adjust_all", 6), ("reserved3", 1),
            ("pred_select", 1),
            ("stride", 8), ("offset", 22), ("reserved4", 1), ("pred_condition", 1)]
MESA_TEX = [("opc", 5), ("src_reg", 6), ("src_reg_am", 1), ("dst_reg", 6), ("dst_reg_am", 1),
            ("fetch_valid_only", 1), ("const_idx", 5), ("tx_coord_denorm", 1), ("src_swiz", 6),
            ("dst_swiz", 12), ("mag_filter", 2), ("min_filter", 2), ("mip_filter", 2), ("aniso_filter", 3),
            ("arbitrary_filter", 3), ("vol_mag_filter", 2), ("vol_min_filter", 2), ("use_comp_lod", 1),
            ("use_reg_lod", 2), ("pred_select", 1),
            ("use_reg_gradients", 1), ("sample_location", 1), ("lod_bias", 7), ("unused", 5), ("dimension", 2),
            ("offset_x", 5), ("offset_y", 5), ("offset_z", 5), ("pred_condition", 1)]


def pack(layout, **values):
    v, at = 0, 0
    for name, width in layout:
        x = values.pop(name, 0)
        assert 0 <= x < 1 << width, (name, x)
        v |= x << at
        at += width
    assert not values, values
    return v


def cf_unit(a, b):
    """Two 48-bit control-flow instructions -> three dwords (packed one after the other, as Mesa's array is)."""
    v = a | b << 48
    return [v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF, (v >> 64) & 0xFFFFFFFF]


def dwords(v):
    return [v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF, (v >> 64) & 0xFFFFFFFF]


def test_cf_unpacking_matches_mesas_packed_array():
    a = pack(MESA_CF_EXEC, address=300, count=5, serialize=0b1001, vc=0x2A, bool_addr=0x81, condition=1, opc=3)
    b = pack(MESA_CF_LOOP, address=700, loop_id=17, address_mode=1, opc=7)
    (a0, a1), (b0, b1) = unpack_cf(*cf_unit(a, b))
    x = decode_cf(a0, a1)
    assert (x["name"], x["address"], x["count"], x["sequence"], x["vc"], x["bool_address"], x["condition"]) == \
        ("cexec", 300, 5, 0b1001, 0x2A, 0x81, 1)
    y = decode_cf(b0, b1)
    assert (y["name"], y["address"], y["loop_id"], y["absolute"]) == ("loop", 700, 17, 1)


def test_cf_jump_call_alloc():
    j = pack(MESA_CF_JMP, address=1000, force_call=1, predicated_jmp=1, direction=1, bool_addr=7, condition=1, opc=11)
    c = pack(MESA_CF_ALLOC, size=5, no_serial=1, buffer_select=2, opc=12)
    (j0, j1), (c0, c1) = unpack_cf(*cf_unit(j, c))
    x = decode_cf(j0, j1)
    assert (x["name"], x["address"], x["unconditional"], x["predicated"], x["direction"], x["bool_address"],
            x["condition"]) == ("cjmp", 1000, 1, 1, 1, 7, 1)
    y = decode_cf(c0, c1)
    assert (y["name"], y["size"], y["unserialized"], y["buffer"]) == ("alloc", 5, 1, 2)


def test_alu_fields():
    v = pack(MESA_ALU, vector_dest=33, scalar_dest=12, export_data=1, vector_write_mask=0b1011, scalar_write_mask=4,
             vector_clamp=1, scalar_opc=19, src3_swiz=0x1B, src2_swiz=0x55, src1_swiz=0xE4, src1_reg_negate=1,
             pred_select=3, src3_reg=0x85, src2_reg=200, src1_reg=7, vector_opc=11, src1_sel=1, src3_sel=1)
    a = decode_alu(*dwords(v))
    assert (a["vector_dest"], a["scalar_dest"], a["export"], a["vector_mask"], a["scalar_mask"], a["vector_clamp"],
            a["scalar_op"], a["vector_op"]) == (33, 12, 1, 0b1011, 4, 1, 19, 11)
    assert a["predicated"] == 1 and a["pred_condition"] == 1  # Mesa's pred_select 3
    assert a["src1"] == {"swizzle": 0xE4, "negate": 1, "reg": 7, "temp": 1}
    assert a["src2"] == {"swizzle": 0x55, "negate": 0, "reg": 200, "temp": 0}
    assert a["src3"] == {"swizzle": 0x1B, "negate": 0, "reg": 0x85, "temp": 1}


def test_fetch_fields():
    v = pack(MESA_VTX, opc=0, src_reg=3, dst_reg=9, must_be_one=1, const_index=21, const_index_sel=2, src_swiz=1,
             dst_swiz=0o7654, format_comp_all=1, format=57, exp_adjust_all=3, stride=12, offset=0x12345,
             pred_select=1, pred_condition=1)
    f = decode_fetch(*dwords(v))
    assert (f["name"], f["src_reg"], f["dst_reg"], f["const_index"], f["const_index_sel"], f["src_swizzle"],
            f["dst_swizzle"], f["signed"], f["format"], f["exp_adjust"], f["stride"], f["offset"], f["predicated"],
            f["pred_condition"]) == ("vfetch", 3, 9, 21, 2, 1, 0o7654, 1, 57, 3, 12, 0x12345, 1, 1)
    v = pack(MESA_TEX, opc=1, src_reg=4, dst_reg=5, const_idx=13, tx_coord_denorm=1, src_swiz=0b100100, dst_swiz=0o3210,
             mag_filter=1, min_filter=2, mip_filter=1, aniso_filter=3, use_comp_lod=1, use_reg_lod=1,
             use_reg_gradients=1, lod_bias=0x7F, dimension=1, offset_x=1, offset_y=0x1F, offset_z=2)
    f = decode_fetch(*dwords(v))
    assert (f["name"], f["src_reg"], f["dst_reg"], f["const_index"], f["unnormalized"], f["src_swizzle"],
            f["dst_swizzle"], f["mag_filter"], f["min_filter"], f["mip_filter"], f["aniso_filter"],
            f["use_comp_lod"], f["use_reg_lod"], f["use_reg_gradients"], f["lod_bias"], f["dimension"],
            f["offset_x"], f["offset_y"], f["offset_z"]) == \
        ("tfetch", 4, 5, 13, 1, 0b100100, 0o3210, 1, 2, 1, 3, 1, 1, 1, -1, 1, 1, -1, 2)


def test_disassembly_of_a_small_vertex_shader():
    nop = pack(MESA_CF_EXEC, opc=0)
    words = []
    # control flow: exec 2 instructions at 2 (fetch, then ALU), alloc position; exece 1 instruction at 4
    words += cf_unit(pack(MESA_CF_EXEC, address=2, count=2, serialize=0b0011, opc=1),
                     pack(MESA_CF_ALLOC, size=0, buffer_select=1, opc=12))
    words += cf_unit(pack(MESA_CF_EXEC, address=4, count=1, opc=2), nop)
    # 2: vfetch r0.xyzw from r0.x, vertex fetch constant 0, 12-byte stride
    words += dwords(pack(MESA_VTX, opc=0, src_reg=0, dst_reg=0, must_be_one=1, dst_swiz=0o3210, format=57, stride=3))
    # 3: mul r1, r0, c4 (no scalar op)
    words += dwords(pack(MESA_ALU, vector_dest=1, vector_write_mask=0xF, scalar_opc=63, src1_reg=0, src1_sel=1,
                         src2_reg=4, vector_opc=1))
    # 4: max oPos, r1, r1 and, co-issued, rcp of r1.w to export 2 (the export bit covers both destinations)
    words += dwords(pack(MESA_ALU, vector_dest=62, export_data=1, vector_write_mask=0xF, scalar_dest=2,
                         scalar_write_mask=1, scalar_opc=19, src1_reg=1, src1_sel=1, src2_reg=1, src2_sel=1,
                         src3_reg=1, src3_sel=1, src3_swiz=0xFF & (3 | 2 << 2 | 1 << 4 | 0 << 6), vector_opc=2))
    text = disassemble(words, "vertex")
    assert text == [
        "exec addr 2, count 2",
        "    2: (S) vfetch r0.xyzw, r0.x, vf0, format 57, stride 3, offset 0",
        "    3: mul r1, r0, c4",
        "alloc position, size 0",
        "exece addr 4, count 1",
        "    4: max oPos, r1, r1",
        "       + rcp o2.x___, r1.wwww",
    ], text
