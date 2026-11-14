"""Point the workshop Home Assistant at Castor and Pollux, and push their schedule.

    python3 -I scripts/ha_lamp_schedule.py            # dry run: report only
    python3 -I scripts/ha_lamp_schedule.py --apply    # make the changes

Normally run through scripts/ha_lamp_schedule.sh, which loads HA_URL and
HA_TOKEN from ~/Developer/Workshop/.env and passes the automations file.

Three changes, each skipped when it is already done, so running it twice is safe:
  1. Rename light.castor_castor -> light.castor and light.pollux_pollux ->
     light.pollux in the entity registry. Firmware before 0.1.1 produced the
     doubled names; the registry keeps them until they are renamed here.
  2. Swap those IDs on the space-status dashboard.
  3. Create or replace the four glowlamp_* automations from
     GeminiLamps/automations.yaml.
"""
import argparse
import asyncio
import json
import os
import re
import sys
import urllib.error
import urllib.request

import websockets
import yaml

RENAMES = {"light.castor_castor": "light.castor", "light.pollux_pollux": "light.pollux"}
DASHBOARD = "space-status"

HA = os.environ["HA_URL"].rstrip("/")
TOKEN = os.environ["HA_TOKEN"]


async def connect():
    ws = await websockets.connect(HA.replace("http", "ws", 1) + "/api/websocket", max_size=None)
    await ws.recv()
    await ws.send(json.dumps({"type": "auth", "access_token": TOKEN}))
    if json.loads(await ws.recv())["type"] != "auth_ok":
        sys.exit("Home Assistant rejected HA_TOKEN")
    counter = 0

    async def call(**cmd):
        nonlocal counter
        counter += 1
        await ws.send(json.dumps({"id": counter, **cmd}))
        while True:
            msg = json.loads(await ws.recv())
            if msg.get("id") == counter and msg.get("type") == "result":
                if not msg["success"]:
                    raise RuntimeError("%s: %s" % (cmd["type"], msg["error"]))
                return msg["result"]

    return ws, call


def rest(method, path, body=None):
    req = urllib.request.Request(
        HA + path,
        method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def swap_ids(text):
    # Whole IDs only, whatever quotes surround them: a plain "entity" field
    # and a single-quoted ID inside a template both need changing.
    for old, new in RENAMES.items():
        text = re.sub(r"(?<![\w.])%s(?![\w])" % re.escape(old), new, text)
    return text


async def main(yaml_path, apply):
    verb = "" if apply else "would "
    ws, call = await connect()

    # 1. Entity registry.
    registry = {e["entity_id"] for e in await call(type="config/entity_registry/list")}
    for old, new in RENAMES.items():
        if new in registry:
            print("ok       %s already exists" % new)
        elif old not in registry:
            sys.exit("neither %s nor %s is in the registry -- stopping" % (old, new))
        else:
            if apply:
                await call(type="config/entity_registry/update", entity_id=old, new_entity_id=new)
            print("%srename %s -> %s" % (verb, old, new))

    # 2. Dashboard.
    config = await call(type="lovelace/config", url_path=DASHBOARD)
    before = json.dumps(config)
    after = swap_ids(before)
    if after == before:
        print("ok       dashboard %s has no old IDs" % DASHBOARD)
    else:
        count = sum(before.count(old) for old in RENAMES)
        if apply:
            await call(type="lovelace/config/save", url_path=DASHBOARD, config=json.loads(after))
        print("%supdate dashboard %s (%d references)" % (verb, DASHBOARD, count))
    await ws.close()

    # 3. Automations. Home Assistant reloads automations after each POST.
    with open(yaml_path) as f:
        automations = yaml.safe_load(f)
    for auto in automations:
        existing = rest("GET", "/api/config/automation/config/" + auto["id"])
        if existing == auto:
            print("ok       automation %s is current" % auto["id"])
            continue
        if apply:
            rest("POST", "/api/config/automation/config/" + auto["id"], auto)
        print("%s%s automation %s" % (verb, "replace" if existing else "create", auto["id"]))

    if not apply:
        print("\nDry run: nothing was changed. Run with --apply to make these changes.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("automations", help="path to GeminiLamps/automations.yaml")
    parser.add_argument("--apply", action="store_true", help="make the changes (default: dry run)")
    args = parser.parse_args()
    asyncio.run(main(args.automations, args.apply))
