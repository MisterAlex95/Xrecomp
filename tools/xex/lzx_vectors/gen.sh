#!/bin/sh
# gen.sh -- remake the LZX test vectors of tools/xex (XBA-270). Not run by the tests: its output is committed.
#
# The data (vector_data.py) is ours. It is compressed by two encoders that are not ours, in a throwaway Debian
# container, so the decoder (tools/xex/lzx.py) is tested against streams it had no part in making:
#   *.wim.lzx   wimlib's LZX compressor (libwim-dev, LGPL-3.0+; called through wim_lzx.c), WIM framing: one chunk,
#               levels 20 / 50 / 100 (lazy, default, near-optimal: verbatim and aligned blocks)
#   *.stored    our own stored-block writer (lzx.py store(), what synth.py uses): only its SHA-256 is kept, after
#               cabextract decoded it (uncompressed blocks, which neither encoder here makes)
#   *.chm.lzx   Free Pascal's chmcmd (fp-utils, its LZX compressor; LGPL with the static linking exception), the
#               CAB / CHM framing XEX uses: 32 KB frames, E8 header bit, 24-bit block sizes, reset interval
# Each CHM stream is checked by decoders that are not ours before it is kept: Free Pascal's own CHM reader (chmls
# extract) must give back the data, and the streams without a reset (one CHM reset interval: small, mid, x86) are
# also put in a cabinet (cab_wrap.py) that cabextract (libmspack, LGPL-2.1) must decode to the same bytes. (libmspack's
# CHM reader refuses chmcmd's files, error DATAFORMAT, hence the cabinet.) Only compressed streams and their metadata
# (vectors.json) are committed: no code of these projects is in the repository.
#
#   sh tools/xex/lzx_vectors/gen.sh        (needs docker and the network for apt)
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
docker run --rm -v "$repo:/r" -e PYTHONDONTWRITEBYTECODE=1 debian:trixie-slim sh -euc '
ln -s /r/tools/xex/lzx_vectors /v
apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends gcc libc6-dev libwim-dev cabextract fp-utils python3 >/dev/null
w=/tmp/w; mkdir -p $w/raw; cd $w
python3 /v/vector_data.py raw
gcc -O2 -o wim_lzx /v/wim_lzx.c -lwim
echo "{" > meta.json
first=1
add() { if [ $first = 0 ]; then echo "," >> meta.json; fi; first=0; printf "\"%s\": %s" "$1" "$2" >> meta.json; }
wim() { # name chunk level
    ./wim_lzx raw/$1.raw /v/$1-$3.wim.lzx $2 $3
    bits=$(python3 -c "import sys; n=int(sys.argv[1]); print((n-1).bit_length())" $2)
    add "$1-$3.wim" "{\"data\": \"$1\", \"flavour\": \"wim\", \"window_bits\": $bits, \"encoder\": \"wimlib $(dpkg-query -W -f "\${Version}" libwim-dev) level $3\"}"
}
wim small 32768 20; wim small 32768 50; wim small 32768 100
wim x86 32768 100
wim mixed 262144 100
wim long 524288 100
chm() { # name
    mkdir -p chm_$1; cd chm_$1
    cp ../raw/$1.raw data.bin
    printf "<html><body>%s</body></html>\n" "$1" > index.htm
    printf "[OPTIONS]\nCompiled file=t.chm\nDefault topic=index.htm\nTitle=%s\n\n[FILES]\nindex.htm\ndata.bin\n" "$1" > t.hhp
    chmcmd t.hhp >/dev/null
    chmls extract t.chm /data.bin check.bin >/dev/null
    cmp check.bin data.bin
    meta=$(python3 /v/chm_extract.py t.chm /v/$1.chm.lzx data.bin)
    if [ "${2:-}" = cab ]; then
        set -- $1 cab $(python3 -c "import json,sys; m=json.loads(sys.argv[1]); print(m[\"window_bits\"], m[\"out_size\"], m[\"file_offset\"], m[\"file_length\"])" "$meta")
        python3 /v/cab_wrap.py /v/$1.chm.lzx t.cab $3 $4
        cabextract -q t.cab
        tail -c +$(($5 + 1)) content.bin | head -c $6 | cmp - data.bin
        echo "$1: cabextract agrees"
    fi
    cd ..
    add "$1.chm" "{\"data\": \"$1\", \"flavour\": \"cab\", \"chm\": $meta, \"encoder\": \"chmcmd $(dpkg-query -W -f "\${Version}" fp-utils)\"}"
}
chm small cab; chm mid cab; chm x86 cab; chm mixed; chm long
stored() { # name: our own stored-block writer (synth.py "normal" XEX), checked by cabextract
    mkdir -p st_$1; cd st_$1
    PYTHONPATH=/r:/v python3 -c "import sys; from vector_data import data; from tools.xex.lzx import store; sys.stdout.buffer.write(store(data(sys.argv[1])))" $1 > s.lzx
    python3 /v/cab_wrap.py s.lzx t.cab 15 $(wc -c < ../raw/$1.raw) exact
    cabextract -q t.cab
    cmp content.bin ../raw/$1.raw
    echo "$1 stored: cabextract agrees"
    sum=$(sha256sum s.lzx | cut -c1-64)
    cd ..
    add "$1.stored" "{\"data\": \"$1\", \"flavour\": \"cab\", \"window_bits\": 15, \"sha256\": \"$sum\", \"encoder\": \"tools/xex/lzx.py store()\"}"
}
stored small; stored mixed
echo "}" >> meta.json
python3 -c "import json; json.dump(json.load(open(\"meta.json\")), open(\"/v/vectors.json\", \"w\"), indent=1, sort_keys=True)"
chown -R '"$(id -u):$(id -g)"' /v/
'
