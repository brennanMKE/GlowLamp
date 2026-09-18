#!/usr/bin/env bash
# Find Glow Lamps on the local network by their mDNS announcement, so you
# can open the Settings page without knowing the IP or digging through the
# router's lease table.
#
#   ./scripts/find_devices.sh              # find Glow Lamps
#   ./scripts/find_devices.sh _http._tcp   # or any other service type
#   SECONDS_TO_WAIT=8 ./scripts/find_devices.sh
#
# The lamp announces _glowlamp._tcp on port 80 with TXT records carrying its
# device name, firmware version, and the settings path (see src/wifi_link.cpp).

set -uo pipefail

SERVICE="${1:-_glowlamp._tcp}"
WAIT="${SECONDS_TO_WAIT:-5}"

# mDNS browsers run until interrupted -- there is no "done" in a protocol built
# on unsolicited announcements. So: run one in the background, give responders
# WAIT seconds to answer, then stop it and parse what arrived.
run_for() {
    local out="$1"; shift
    "$@" >"$out" 2>&1 &
    local pid=$!
    sleep "$WAIT"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Browsing for ${SERVICE} (${WAIT}s)..."

if command -v dns-sd >/dev/null 2>&1; then
    # -Z asks for the zone-file form, which carries SRV (host + port) and TXT in
    # one pass instead of a browse followed by a resolve per instance.
    run_for "$TMP/raw" dns-sd -Z "$SERVICE" local

    # SRV lines look like:
    #   name._glowlamp._tcp  SRV  0 0 80 host.local. ; Replace with unicast...
    # so priority/weight/port/target are fields 3-6. Anchoring on those rather
    # than counting back from NF matters: the trailing "; Replace..." comment is
    # part of the line, so $NF is a word of prose, not the hostname.
    awk -v svc="$SERVICE" '
        $2 == "SRV" {
            name = $1; sub("\\." svc ".*", "", name)
            port = $5; host = $6
            sub(/\.$/, "", host)
            printf "\n  %s\n    http://%s:%s/\n", name, host, port
            printf "    settings: http://%s:%s/settings\n", host, port
        }
        $2 == "TXT" {
            txt = $0; sub(/^[^ ]+[ \t]+TXT[ \t]+/, "", txt)
            if (length(txt)) printf "    txt: %s\n", txt
        }
    ' "$TMP/raw"

    if ! awk '$2 == "SRV" { found = 1 } END { exit !found }' "$TMP/raw"; then
        echo "  none found."
        echo
        echo "  If a lamp is powered on and joined to WiFi, check that this Mac is on"
        echo "  the same subnet -- mDNS does not cross VLANs or a guest network."
    fi

elif command -v avahi-browse >/dev/null 2>&1; then
    run_for "$TMP/raw" avahi-browse -rpt "$SERVICE"
    # -p is the parseable form: =;iface;proto;name;type;domain;host;addr;port;txt
    awk -F';' '/^=/ { printf "\n  %s\n    http://%s:%s/\n    settings: http://%s:%s/settings\n    txt: %s\n", $4, $8, $9, $8, $9, $11 }' "$TMP/raw"
    grep -q '^=' "$TMP/raw" || echo "  none found."

else
    echo "Need dns-sd (macOS, built in) or avahi-browse (Linux: avahi-utils)." >&2
    exit 1
fi
