#!/usr/bin/env bash
# Find Glow Lamps on the local network.
#
#   ./scripts/find_devices.sh              # scan and report
#   ./scripts/find_devices.sh --check      # ask each lamp to check GitHub
#   ./scripts/find_devices.sh --update     # install on every lamp with an update
#   SECONDS_TO_WAIT=8 ./scripts/find_devices.sh
#
# A thin wrapper around scripts/glowlamp.py, which does the actual work.
#
# This was a second, independent implementation of discovery until it drifted
# from the firmware it was reading. It listed five lamps for two devices --
# every stale mDNS record from every rename the lamps had been through -- and
# reported "firmware unknown" for lamps that were answering perfectly well,
# because it parsed status.json with sed and that endpoint had since become
# pretty-printed and nested: `"version": "0.0.5"` has a space after the colon
# and the pattern did not.
#
# Neither bug was in the lamps. Both came from maintaining a second copy of the
# same logic in a language with no JSON parser, so the copy is gone and this
# forwards to the one implementation that has one.

set -euo pipefail
cd "$(dirname "$0")/.."

# Written as a plain string rather than an array: macOS ships bash 3.2, where
# expanding an empty array under `set -u` is an "unbound variable" error.
TIMEOUT_ARG=""
[[ -n "${SECONDS_TO_WAIT:-}" ]] && TIMEOUT_ARG="--timeout ${SECONDS_TO_WAIT}"

case "${1:-}" in
    --update) shift; exec python3 scripts/glowlamp.py update --all $TIMEOUT_ARG "$@" ;;
    --check)  shift; exec python3 scripts/glowlamp.py update --check --all $TIMEOUT_ARG "$@" ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
esac

exec python3 scripts/glowlamp.py discover $TIMEOUT_ARG "$@"
