#!/usr/bin/env bash
# Upload the current descry build + the IRONLUNG jungle demo to a 3DS running ftpd.
#
#   ./tools/upload_3ds.sh 192.168.0.105
#   ./tools/upload_3ds.sh 192.168.0.105 5000
#
# Sends:
#   descry.3dsx            -> sdmc:/3ds/descry/descry.3dsx
#   dist/demo/project_00.tr3d -> sdmc:/3ds/descry/project_00.tr3d   (slot 00 = IRONLUNG)
#   dist/demo/sample_63.s16   -> sdmc:/3ds/descry/sample_63.s16     (the break audio)
#   dist/demo/sample_63.name  -> sdmc:/3ds/descry/sample_63.name
#
# A .tr3d carries NO audio (docs/BUGS.md B3) - the .s16 MUST go with it or the
# break track plays silence.
set -u

IP="${1:-}"
PORT="${2:-5000}"
if [ -z "$IP" ]; then echo "usage: $0 <3ds-ip> [port]" >&2; exit 2; fi

cd "$(dirname "$0")/.." || exit 1

# proxies break LAN uploads - always bypass (see .kota context)
CURL=(curl --noproxy '*' -sS --connect-timeout 8 --ftp-create-dirs)

if [ ! -f descry.3dsx ]; then echo "descry.3dsx missing - run: make" >&2; exit 1; fi
for required in project_00.tr3d sample_63.s16 sample_63.name; do
    if [ ! -f "dist/demo/$required" ]; then
        echo "demo missing dist/demo/$required - run: make demo" >&2
        exit 1
    fi
done

echo "probing ftp://$IP:$PORT ..."
if ! curl --noproxy '*' -sS --connect-timeout 8 "ftp://$IP:$PORT/" >/dev/null; then
    echo "no ftpd at $IP:$PORT - is ftpd running on the console, and is it on this subnet?" >&2
    exit 1
fi

put() {  # put <local> <remote-path>
    printf '  %-26s -> %s\n' "$(basename "$1")" "$2"
    "${CURL[@]}" -T "$1" "ftp://$IP:$PORT/$2" || { echo "  FAILED: $1" >&2; return 1; }
}

fail=0
put descry.3dsx                 "3ds/descry/descry.3dsx"           || fail=1
put dist/demo/project_00.tr3d   "3ds/descry/project_00.tr3d"       || fail=1
put dist/demo/sample_63.s16     "3ds/descry/sample_63.s16"         || fail=1
put dist/demo/sample_63.name    "3ds/descry/sample_63.name"        || fail=1

if [ "$fail" -ne 0 ]; then echo "upload had errors" >&2; exit 1; fi
echo
echo "done. on the console: open descry -> PROJECT screen -> load slot 00 (IRONLUNG)."
