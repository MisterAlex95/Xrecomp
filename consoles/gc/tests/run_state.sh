#!/bin/sh
# ctest helper (XBA-264): run_state.sh GC_RUN DOL N:M|rewind [gc_run options...]
#
# Save-state round trip on our homebrew: gc_run --state-check N:M (N fields, a state saved, M fields recorded, the
# state loaded, the M fields again: every XFB and the state after them identical). Extra options go to gc_run
# (--gx: the GLES renderer on a headless context; --pad ...). With "rewind" instead of N:M the options say it all
# (--frames F --rewind C:R): the line "REWIND OK" must come. Exits 77 (skipped) when DOL is absent
# (tools/consoles/build_homebrew.sh gc) or, with --gx, when there is no GLES 3 context.
# A cross build's binaries run under the emulator ctest uses (XBR_TEST_EMULATOR, comma separated: qemu-aarch64
# for the arm64 CI); empty on a native build.
emu=$(printf '%s' "${XBR_TEST_EMULATOR:-}" | tr ',' ' ')
run=$1
dol=$2
nm=$3
shift 3
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
if [ "$nm" = rewind ]; then
    out=$(timeout 300 $emu "$run" "$dol" "$@" 2>&1)
    rc=$?
    want='^REWIND OK'
else
    out=$(timeout 300 $emu "$run" "$dol" --state-check "$nm" "$@" 2>&1)
    rc=$?
    want='^STATE OK'
fi
echo "$out" | grep -E '^STATE|^REWIND|skipped|stopped' || echo "$out" | tail -5
[ "$rc" -eq 77 ] && exit 77
[ "$rc" -eq 0 ] && echo "$out" | grep -q "$want" && exit 0
exit 1
