#!/usr/bin/env bash
# Rename the lamp entities, fix the space-status dashboard, and push the
# Castor/Pollux schedule to the workshop Home Assistant.
#
#   ./scripts/ha_lamp_schedule.sh            # dry run: shows what would change
#   ./scripts/ha_lamp_schedule.sh --apply    # makes the changes
#
# Credentials come from the Workshop project's .env (HA_URL, HA_TOKEN), and
# the schedule from its GeminiLamps/automations.yaml. Neither is copied here.

set -euo pipefail

RPI="${RPI_DIR:-$HOME/Developer/Workshop}"
ENV_FILE="$RPI/.env"
AUTOMATIONS="$RPI/GeminiLamps/automations.yaml"

[[ -f "$ENV_FILE" ]] || { echo "missing $ENV_FILE" >&2; exit 1; }
[[ -f "$AUTOMATIONS" ]] || { echo "missing $AUTOMATIONS" >&2; exit 1; }

set -a
# shellcheck disable=SC1090
. "$ENV_FILE"
set +a

[[ -n "${HA_URL:-}" && -n "${HA_TOKEN:-}" ]] || { echo "HA_URL and HA_TOKEN must be set in $ENV_FILE" >&2; exit 1; }

exec python3 -I "$(dirname "$0")/ha_lamp_schedule.py" "$AUTOMATIONS" "$@"
