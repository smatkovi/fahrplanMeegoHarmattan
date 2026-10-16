#!/bin/sh
# Baut Fahrplan fuer MeeGo Harmattan (Nokia N9 / N950) mit MADDE.
#
#   tools/build-harmattan.sh          # qmake + make + Paket
#   tools/build-harmattan.sh build    # nur uebersetzen (schneller Durchlauf)
#   tools/build-harmattan.sh clean    # Zwischenstaende weg
#
# MADDE liefert das Qt 4.7.4 des Geraets samt Cross-GCC 4.4.1; die
# debian/-Ablage erwartet dpkg-buildpackage dort, wo Qt Creator sie
# sonst hinkopiert. Das fertige .deb landet eine Ebene ueber dem Baum.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
MAD=${MAD:-$HOME/QtSDK/Madde/bin/mad}
JOBS=${JOBS:-8}
# MADDE will ohne gesetztes Standardziel bei jedem Aufruf ein -t; das
# installierte Ziel steht in seiner eigenen Liste.
TARGET=${TARGET:-$("$MAD" list 2>/dev/null | sed -n 's/^\([^ ]*\) *(installed)$/\1/p' | head -1)}
MODE=${1:-package}

[ -x "$MAD" ] || { echo "MADDE fehlt: $MAD" >&2; exit 1; }
[ -n "$TARGET" ] || { echo "kein installiertes MADDE-Ziel gefunden" >&2; exit 1; }
MADT="$MAD -t $TARGET"
cd "$HERE"

if [ "$MODE" = clean ]; then
    rm -rf debian tmp Makefile fahrplan2 build-stamp configure-stamp
    echo "== aufgeraeumt"
    exit 0
fi

# Qt Creator kopiert die Harmattan-Ablage nach debian/; ohne Creator machen
# wir das selbst, und zwar jedes Mal neu, damit eine geaenderte
# changelog-Fassung auch im Paket steht.
rm -rf debian
cp -r qtc_packaging/debian_harmattan debian
chmod +x debian/rules

echo "== qmake (MADDE, Ziel $TARGET)"
nice $MADT qmake fahrplan2.pro

echo "== make -j$JOBS"
nice $MADT make -j"$JOBS"

if [ "$MODE" = build ]; then
    echo "== gebaut: $HERE/fahrplan2"
    exit 0
fi

echo "== Paket"
nice $MADT dpkg-buildpackage -b -us -uc
ls -la "$HERE"/../fahrplan2_*_armel.deb
