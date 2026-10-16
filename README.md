# fahrplanMeegoHarmattan
this is a meego fork of poetasters maintained fahrplan app backported to qt4 with chatgpt. a compiled package can be downloaded at openrepos: https://openrepos.net/content/aviarus/fahrplan-0

## backends

the same list as upstream, minus the ones that do not compile against qt 4.7.
newest addition: **Worldwide (Transitous)** -- the MOTIS service at
api.transitous.org, built from open GTFS feeds, backported from upstream
2.0.58. it covers austria too and needs no api key.

measured on an N950: station search about half a second, journey search
including parsing the few hundred kilobytes of json about 1.2 seconds (the
device has the 1.0.2 libssl in /usr/local/lib next to the factory 0.9.8, which
is what https to that service needs).

the backend list in `src/fahrplan_backend_manager.cpp` and the switch in
`src/fahrplan_parser_thread.cpp` have to count the same way -- new backends go
at the end of both, otherwise a `currentBackend` already stored on a device
starts pointing at a different one.

not backported from upstream yet: `parser_ns` (needs a lambda), `parser_ninetwo`
(`auto`, `nullptr`, range-for) and `parser_trentino` (`QTimeZone`) -- none of
that exists in the gcc 4.4 / qt 4.7 of MADDE.

## building the harmattan package

needs the nokia QtSDK with the MADDE harmattan target installed:

    tools/build-harmattan.sh          # qmake + make + .deb (one level above the tree)
    tools/build-harmattan.sh build    # only compile
    tools/build-harmattan.sh clean

the package version comes from `qtc_packaging/debian_harmattan/changelog`, not
from `fahrplan2.pro`. install it on the device with `aegis-dpkg -i`, not with
plain `dpkg -i`: out of a package whose origin aegis does not know, nothing
starts.

## trying a backend without the gui

`tools/transitous-probe` runs the transitous backend from the command line:

    ./transitous-probe "Wien Hbf" "Graz Hbf"       # on the device
    ./transitous-probe --dateien geocode.json plan.json

the first form is the only way to see whether a service's TLS works with the
libssl installed on the device. the second replays saved replies, which also
works under qemu-arm with the MADDE sysroot, or on a machine whose openssl is
too new for qt 4 to use at all:

    qemu-arm -L $SYSROOT -E LD_LIBRARY_PATH=$SYSROOT/usr/lib \
        ./transitous-probe --dateien geocode.json plan.json
