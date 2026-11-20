#!/bin/sh
# The network library's engine against real libtorrent on a Linux x86-64 host (XBA-321): see NetHostCheck.java.
# Runs in the app's unit-test image (a JDK); downloads the libtorrent4j jars (the version in android/app/build.gradle)
# from Maven Central into a temporary folder. Usage: tools/android/net_host_check.sh   (from the repository root)
set -eu
V=$(sed -n "s/.*org.libtorrent4j:libtorrent4j:\([^']*\)'.*/\1/p" android/app/build.gradle)
docker build -q -f tools/docker/android.Dockerfile --target app-unit-tests-deps -t xba-unit-deps . >/dev/null
docker run --rm -v "$PWD":/repo:ro -e V="$V" xba-unit-deps sh -euc '
  mkdir -p /w/jars /w/src /w/out && cd /w/jars
  B=https://repo1.maven.org/maven2/org/libtorrent4j
  for a in libtorrent4j libtorrent4j-linux; do curl -sfO $B/$a/$V/$a-$V.jar; done
  mkdir -p /w/native && cd /w/native && unzip -q -o /w/jars/libtorrent4j-linux-$V.jar "lib/x86_64/*"
  S=/repo/android/app/src/main/java/fr/adserver/xrecomp
  javac -encoding UTF-8 -nowarn -d /w/out -cp /w/jars/libtorrent4j-$V.jar $S/NetEngine.java $S/LibtorrentEngine.java \
        $S/TorrentMeta.java /repo/tools/android/net_host_check/NetHostCheck.java
  java -Djava.library.path=/w/native/lib/x86_64 -cp /w/out:/w/jars/libtorrent4j-$V.jar fr.adserver.xrecomp.NetHostCheck
'
