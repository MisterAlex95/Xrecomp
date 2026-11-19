"""Comparison snapshots must survive a join of different CMP operands."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand

BASE = 0x10000

def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_different_cmp_operands_join_for_setne():
    code = translate_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code

def test_different_cmp_operands_join_for_cmovne():
    code = translate_join(bytes.fromhex('0f45c7c3'))
    assert 'if (CMP_NE(_fa, _fb)) eax = edi;' in code, code

def test_unknown_path_is_not_guessed():
    assert _merge_flag_states([None, ('cmp', [])]) is None

def test_mixed_operations_are_not_guessed():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('cmp', [a, b]), ('test', [a, b])]) is None

def test_mixed_widths_are_not_guessed():
    wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
    assert _merge_flag_states([('cmp', wide), ('cmp', narrow)]) is None


def translate(image):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image), '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_float_compares_with_swapped_operands_join():
    # A clamp whose direction depends on a sign (T()NY, 0x001CE755):
    # test cl,cl; jle alt; comiss xmm3,xmm0; jmp join;
    # alt: comiss xmm0,xmm3; join: jbe skip; movaps xmm0,xmm3; skip: ret
    code = translate(bytes.fromhex('84c97e050f2fd8eb030f2fc376030f28c3c3'))
    assert 'if ((_fca <= _fcb || (_fca != _fca || _fcb != _fcb)))' in code, code
    assert 'if (_flags' not in code, code


def test_fcomi_family_joins():
    # fld st0; fcomip st1 on one edge, fucomip st1 on the other, then ja.
    # test cl,cl; jle alt; fcomip st(1); jmp join; alt: fucomip st(1); join: ja skip; nop; skip: ret
    code = translate(bytes.fromhex('84c97e04dff1eb02dfe97701' + '90c3'))
    assert 'if ((!(_fa & 1u) && !(_fa & 0x40u)) /* fucompi */)' in code, code
    assert 'if (_flags' not in code, code


def test_float_and_integer_compares_are_not_merged():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    x = Operand(type='reg', reg='xmm0')
    y = Operand(type='reg', reg='xmm1')
    assert _merge_flag_states([('comiss', [x, y]), ('cmp', [a, b])]) is None
    assert _merge_flag_states([('comiss', [x, y]), ('fcomip', [])]) is None


def test_mixed_setters_evaluate_the_join_on_each_edge():
    # test ecx,ecx; jz alt; sub eax,1; jmp join; alt: test eax,eax;
    # join: jne out; nop; out: ret
    # A result snapshot (sub) meets a compare snapshot (test): the states do
    # not merge, and the join used to read the never-assigned _flags.
    code = translate(bytes.fromhex('85c9740583e801eb0285c0750190c3'))
    assert 'if (!_jZ_0001000B) goto' in code, code
    assert 'if (_flags' not in code, code
    # the sub edge stores ZF before its jmp, the test edge before falling in
    # (the join image, XBA-317)
    assert ('_jZ_0001000B = ((_fa == 0)) ? 1 : 0; /* flags of this edge */\n'
            '    goto loc_0001000B;') in code, code
    tail = code.index('_jZ_0001000B = (CMP_EQ(_fa, _fb)) ? 1 : 0; /* flags of this edge */')
    assert code.index('loc_00010009:') < tail < code.index('loc_0001000B:'), code
    assert 'int _jZ_0001000B = 0;' in code, code


def test_edges_read_snapshots_across_a_move_at_the_join():
    # neg eax on one edge, dec ebx on the other, then the join moves eax
    # before its je. Both edges read their result snapshot, not eax.
    # test ecx,ecx; jz alt; neg eax; jmp join; alt: dec ebx; join: mov eax,1; je out; nop; out: ret
    code = translate(bytes.fromhex('85c97404f7d8eb014bb801000000740190c3'))
    assert code.count('_jZ_00010009 = ((_fa == 0)) ? 1 : 0;') == 2, code
    assert 'if (_jZ_00010009) goto' in code, code


def test_a_join_with_an_unknown_predecessor_is_not_guessed():
    from tools.recomp.translator import _edge_flag_plan
    assert _edge_flag_plan(None, set(), {}) is None
    assert _edge_flag_plan(None, {1, 2}, {1: ('cmp', []), 2: None}) is None


def test_mixed_width_compares_join_on_each_edge():
    # DOA3's "is this fighter in move list N" check reaches its jne from a
    # cmp al,1 and a cmp ecx,eax; the fallback made the stage wall clamp
    # skip both fighters every frame. js needs each compare's own sign bit.
    # test ecx,ecx; jz narrow; cmp ecx,eax; jmp join; narrow: cmp al,1;
    # join: jcc +1; inc eax; ret
    code = translate(bytes.fromhex('85c9740439c1eb023c01' + '7501' + '40c3'))
    assert code.count('_jZ_0001000A = (CMP_EQ(_fa, _fb)) ? 1 : 0;') == 2, code
    assert 'if (_flags' not in code, code
    code = translate(bytes.fromhex('85c9740439c1eb023c01' + '7801' + '40c3'))
    assert '(uint32_t)(_fas) - (uint32_t)(_fbs)) >> 31' in code, code
    assert '(uint8_t)(_fas) - (uint8_t)(_fbs)) >> 7' in code, code
    assert 'if (_flags' not in code, code


def test_ors_into_different_registers_join_on_each_edge():
    # DOA3's title input ORs a pad mask into ecx on one path and edx on the
    # other, then branches on ZF; the fallback acted on an unpressed START.
    # test ecx,ecx; jz other; mov ecx,edx; or ecx,ebx; jmp join;
    # other: or edx,ebx; join: jcc +1; inc eax; ret
    for jcc in ('74', '75'):
        code = translate(bytes.fromhex('85c9740689d109d9eb0209da' + jcc + '0140c3'))
        assert code.count('_jZ_0001000C = ((_fa == 0)) ? 1 : 0;') == 2, code
        assert 'if (_flags' not in code, code


def test_same_destination_join_reads_the_snapshot_not_the_register():
    # MSVC's signed `x % 2` followed by a reload of eax before the branch
    # (KOTOR 0x001E4FF2): and eax,0x80000001; jns join; dec eax; or eax,-2;
    # inc eax; join: mov eax,[esi+0xbc]; je out; nop; out: ret
    # Both edges leave ZF as (eax == 0), but eax is a pointer by the je.
    for jcc, cond in (('74', '(_fa == 0)'), ('75', '(_fa != 0)')):
        code = translate(bytes.fromhex('2501000080' + '7905' + '48' + '83c8fe' + '40'
                                       + '8b86bc000000' + jcc + '01' + '90c3'))
        assert f'if ({cond}) goto' in code, code
        assert '(eax == 0)' not in code and '(eax != 0)' not in code, code
        assert 'if (_flags' not in code, code

def test_same_destination_join_publishes_zf_from_the_snapshot():
    # A call or pushfd after the same join publishes ZF into g_eflags through
    # the same condition, so it must not re-read the register either.
    from tools.recomp.lifter import publish_flags

    class Reg:
        type = 'reg'
        reg = 'eax'
    (line,) = publish_flags(('__zf_from_dest', [Reg()]))
    assert '(_fa == 0)' in line and 'eax' not in line, line
