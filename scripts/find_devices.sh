#!/usr/bin/env bash
# Find Glow Lamps on the local network by their mDNS announcement, report what
# firmware each is running, and optionally tell them to update.
#
#   ./scripts/find_devices.sh              # scan and report
#   ./scripts/find_devices.sh --check      # ask each lamp to check GitHub first
#   ./scripts/find_devices.sh --update     # install on every lamp with an update
#   SECONDS_TO_WAIT=8 ./scripts/find_devices.sh
#
# Each lamp announces _glowlamp._tcp on port 80 with TXT records carrying its
# device name, firmware version, and the settings path (see src/wifi_link.cpp),
# and serves /status.json with the live version and update state.
#
# mDNS does not cross VLANs or a guest network, so this only sees lamps on the
# same subnet as this Mac.

set -uo pipefail

SERVICE="_glowlamp._tcp"
WAIT="${SECONDS_TO_WAIT:-5}"
DO_CHECK=0
DO_UPDATE=0

for arg in "$@"; do
    case "$arg" in
        --check)  DO_CHECK=1 ;;
        # Updating implies checking: a lamp that has not talked to GitHub this
        # boot reports no update available simply because it has not looked.
        --update) DO_UPDATE=1; DO_CHECK=1 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        _*) SERVICE="$arg" ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

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

# Resolve a .local name to an address before curl sees it.
#
# curl hands .local names to the system resolver, and a cold mDNS cache
# regularly takes longer than curl's timeout to answer -- the first scan after
# a reboot would report every lamp as unreachable, then work on the second run
# once the cache was warm. That looked exactly like a flaky lamp and was not.
# Asking the mDNS responder directly avoids the slow path entirely.
#
# Falls back to the name itself if resolution turns up nothing, so a machine
# with neither browser installed still gets whatever the resolver can manage.
resolve_host() {
    local host="$1" ip=""
    if command -v dns-sd >/dev/null 2>&1; then
        local out="$TMP/resolve.$$"
        dns-sd -G v4 "$host" >"$out" 2>&1 &
        local pid=$!
        # Poll instead of sleeping the full budget: a responder on the LAN
        # normally answers in well under a second.
        for _ in 1 2 3 4 5 6 7 8; do
            sleep 0.25
            ip="$(awk '$2 == "Add" { print $6; exit }' "$out" 2>/dev/null)"
            [[ -n "$ip" ]] && break
        done
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -f "$out"
    elif command -v avahi-resolve >/dev/null 2>&1; then
        ip="$(avahi-resolve -4 -n "$host" 2>/dev/null | awk '{print $2; exit}')"
    fi
    printf '%s' "${ip:-$host}"
}

# One field out of a flat JSON object. Deliberately not a JSON parser: the only
# consumer is status.json, whose shape this repo controls, and requiring jq or
# python3 to find a lamp is a dependency this does not need.
json_field() {
    printf '%s' "$1" | sed -n 's/.*"'"$2"'":"\([^"]*\)".*/\1/p'
}
json_flag() {
    printf '%s' "$1" | sed -n 's/.*"'"$2"'":\(true\|false\).*/\1/p'
}

echo "Browsing for ${SERVICE} (${WAIT}s)..."

# Collect "name<TAB>host<TAB>port" for every instance found, whichever browser
# this machine has.
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
            host = $6; sub(/\.$/, "", host)
            printf "%s\t%s\t%s\n", name, host, $5
        }
    ' "$TMP/raw" | sort -u > "$TMP/lamps"
elif command -v avahi-browse >/dev/null 2>&1; then
    run_for "$TMP/raw" avahi-browse -rpt "$SERVICE"
    # -p is the parseable form: =;iface;proto;name;type;domain;host;addr;port;txt
    awk -F';' '/^=/ { printf "%s\t%s\t%s\n", $4, $8, $9 }' "$TMP/raw" | sort -u > "$TMP/lamps"
else
    echo "Need dns-sd (macOS, built in) or avahi-browse (Linux: avahi-utils)." >&2
    exit 1
fi

if [[ ! -s "$TMP/lamps" ]]; then
    echo "  none found."
    echo
    echo "  If a lamp is powered on and joined to WiFi, check that this Mac is on"
    echo "  the same subnet -- mDNS does not cross VLANs or a guest network."
    echo "  An unprovisioned lamp is not on your network at all: look for its"
    echo "  setup AP instead, named <device>-Setup-<mac>."
    exit 0
fi

# A check is a GitHub round trip on the device, so ask them all first and read
# the answers afterwards rather than waiting out each lamp in turn.
if (( DO_CHECK )); then
    while IFS=$'\t' read -r name host port; do
        addr="$(resolve_host "$host")"
        curl -fsS -m 8 -X POST "http://${addr}:${port}/ota/check" >/dev/null 2>&1 || true
    done < "$TMP/lamps"
    echo "  asked every lamp to check GitHub, waiting for answers..."
    sleep 4
fi

FOUND=0
UPDATED=0
while IFS=$'\t' read -r name host port; do
    FOUND=$((FOUND + 1))
    addr="$(resolve_host "$host")"
    printf '\n  %s\n    http://%s:%s/' "$name" "$host" "$port"
    [[ "$addr" != "$host" ]] && printf '  (%s)' "$addr"
    printf '\n'

    STATUS="$(curl -fsS -m 8 "http://${addr}:${port}/status.json" 2>/dev/null || true)"
    if [[ -z "$STATUS" ]]; then
        # Announced but not answering: mDNS records outlive the device that
        # published them, so this is usually a lamp that has since gone away.
        printf '    not answering on HTTP (stale mDNS record?)\n'
        continue
    fi

    VERSION="$(json_field "$STATUS" version)"
    LATEST="$(json_field "$STATUS" latest)"
    AVAILABLE="$(json_flag "$STATUS" available)"
    ERROR="$(json_field "$STATUS" error)"

    printf '    firmware %s' "${VERSION:-unknown}"
    if [[ -n "$LATEST" ]]; then printf ', latest release %s' "$LATEST"; fi
    if [[ "$AVAILABLE" == "true" ]]; then printf '  <-- update available'; fi
    printf '\n'
    if [[ -n "$ERROR" ]]; then printf '    last check failed: %s\n' "$ERROR"; fi

    if (( DO_UPDATE )) && [[ "$AVAILABLE" == "true" ]]; then
        printf '    installing %s...\n' "$LATEST"
        # The lamp answers before it starts downloading, then goes silent for
        # 10-30 s and reboots. Nothing here waits for it: re-run the scan to see
        # the new version once it is back.
        curl -fsS -m 8 -X POST "http://${addr}:${port}/ota/install" >/dev/null 2>&1 \
            && UPDATED=$((UPDATED + 1)) \
            || printf '    could not reach the install endpoint\n'
    fi
done < "$TMP/lamps"

printf '\n  %d lamp(s) found' "$FOUND"
if (( DO_UPDATE )); then printf ', %d updating' "$UPDATED"; fi
printf '\n'
if (( DO_UPDATE )) && (( UPDATED > 0 )); then
    printf '  Re-run in a minute to confirm they came back on the new version.\n'
fi
