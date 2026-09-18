#!/usr/bin/env python3
"""Find Glow Lamps on the network and drive them over their REST API.

Usable three ways:

    # a command line
    ./scripts/glowlamp.py discover
    ./scripts/glowlamp.py off --all
    ./scripts/glowlamp.py brightness 128 --host glow-lamp-051860.local
    ./scripts/glowlamp.py identify --name "Living Room"

    # from Home Assistant, as a shell_command (see docs/home-assistant.md)
    shell_command:
      lamps_off: "/config/scripts/glowlamp.py off --all"

    # as a module
    from glowlamp import discover, Lamp
    for lamp in discover():
        Lamp(lamp.address).power(False)

Standard library only, so it runs under Home Assistant's Python without adding
a dependency. Discovery uses the `zeroconf` package when it is importable --
Home Assistant ships it -- and otherwise shells out to the mDNS browser the OS
already has (`dns-sd` on macOS, `avahi-browse` on Linux).
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field

SERVICE = "_glowlamp._tcp.local."
DEFAULT_TIMEOUT = 5.0

# Every request is to a device on the LAN that answers in milliseconds or is not
# there at all. A long timeout here only means a slow failure.
HTTP_TIMEOUT = 8.0


@dataclass
class Found:
    """A lamp as mDNS describes it, before anything has talked to it."""

    hostname: str                     # "glow-lamp-051860.local"
    address: str                      # "192.168.1.42", preferred for requests
    port: int = 80
    name: str = ""                    # the friendly name, from the TXT records
    version: str = ""
    properties: dict = field(default_factory=dict)

    @property
    def url(self) -> str:
        # The resolved address, not the .local name: a cold mDNS cache
        # regularly takes longer to answer than the request timeout, which
        # makes the first call after a reboot fail and the second one work.
        host = self.address or self.hostname
        return f"http://{host}:{self.port}" if self.port != 80 else f"http://{host}"

    @property
    def label(self) -> str:
        return self.name or self.hostname.replace(".local.", "").replace(".local", "")


class LampError(RuntimeError):
    pass


class Lamp:
    """One lamp's REST API.

    `target` is whatever reaches it: an IP, a .local name, or a full URL.
    """

    def __init__(self, target: str, timeout: float = HTTP_TIMEOUT):
        if not target.startswith("http://") and not target.startswith("https://"):
            target = "http://" + target
        self.url = target.rstrip("/")
        self.timeout = timeout

    # -- plumbing ---------------------------------------------------------

    def _request(self, method: str, path: str, body: dict | None = None) -> dict:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(
            self.url + path,
            data=data,
            method=method,
            headers={"Content-Type": "application/json"} if data else {},
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode()
        except urllib.error.HTTPError as exc:
            # The lamp reports a bad value as JSON with a 400; surface its own
            # wording rather than "HTTP Error 400".
            detail = exc.read().decode(errors="replace")
            try:
                detail = json.loads(detail).get("error", detail)
            except ValueError:
                pass
            raise LampError(f"{self.url}{path}: {detail}") from exc
        except OSError as exc:
            raise LampError(f"{self.url}{path}: {exc}") from exc

        if not raw:
            return {}
        try:
            return json.loads(raw)
        except ValueError as exc:
            raise LampError(f"{self.url}{path}: response was not JSON") from exc

    # -- the API ----------------------------------------------------------
    #
    # Every mutating call returns the lamp's full state, so each of these hands
    # back the new status rather than None.

    def status(self) -> dict:
        return self._request("GET", "/api/status?pretty=0")

    def power(self, on: bool | str) -> dict:
        return self._request("POST", "/api/power", {"on": on})

    def toggle(self) -> dict:
        return self.power("toggle")

    def brightness(self, value: int) -> dict:
        if not 0 <= value <= 255:
            raise LampError("brightness must be between 0 and 255")
        return self._request("POST", "/api/brightness", {"value": value})

    def identify(self, seconds: int = 4) -> dict:
        return self._request("POST", "/api/identify", {"seconds": seconds})

    def effects(self) -> dict:
        """What this lamp's firmware can render, and its limits."""
        return self._request("GET", "/api/effects")

    def set_effect(self, effect: str, colors: list[str] | None = None,
                   seconds: int = 300) -> dict:
        """Set the effect and palette.

        `colors` is up to five hex strings; None keeps whatever palette the
        lamp is showing. `seconds` is how long before it reverts to the
        default -- 0 means until the lamp reboots.
        """
        body: dict = {"effect": effect, "seconds": seconds}
        if colors:
            if len(colors) > 5:
                raise LampError("at most 5 colors")
            body["colors"] = colors
        return self._request("POST", "/api/effect", body)

    def reset_effect(self) -> dict:
        return self._request("POST", "/api/effect/reset")

    def set_broker(self, host: str, port: int = 1883, user: str = "",
                   password: str = "") -> dict:
        """Point the lamp at an MQTT broker; an empty host turns MQTT off.

        An empty password leaves the stored one alone, so the host can be
        changed without knowing it.
        """
        body = {"host": host, "port": port, "user": user}
        if password:
            body["pass"] = password
        return self._request("POST", "/api/broker", body)

    def rename(self, name: str | None = None, hostname: str | None = None) -> dict:
        body = {}
        if name is not None:
            body["name"] = name
        if hostname is not None:
            body["hostname"] = hostname
        return self._request("POST", "/api/name", body)

    def check_for_update(self) -> dict:
        return self._request("POST", "/api/ota/check")

    def install_update(self) -> dict:
        return self._request("POST", "/api/ota/install")

    def update(self) -> dict:
        """Check, and install only if the latest release differs.

        One request, and a no-op on a lamp that is already current -- which is
        what makes it safe to run against every lamp on a schedule.
        """
        return self._request("POST", "/api/ota/update")

    def reboot(self) -> dict:
        return self._request("POST", "/api/reboot")


# =========================================================================
# Discovery
# =========================================================================


def discover(timeout: float = DEFAULT_TIMEOUT) -> list[Found]:
    """Every lamp announcing itself on this subnet.

    mDNS does not cross VLANs or a guest network, so this only sees lamps the
    calling machine shares a subnet with. An unprovisioned lamp is not on the
    network at all and will not appear.
    """
    lamps = _discover_zeroconf(timeout)
    if lamps is None:
        lamps = _discover_cli(timeout)
    return sorted(lamps, key=lambda lamp: lamp.label.lower())


def _discover_zeroconf(timeout: float) -> list[Found] | None:
    """Returns None if zeroconf is not installed, so the caller can fall back."""
    try:
        from zeroconf import ServiceBrowser, ServiceListener, Zeroconf
    except ImportError:
        return None

    found: dict[str, Found] = {}

    class Listener(ServiceListener):
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name, timeout=int(timeout * 1000))
            if not info:
                return
            props = {
                k.decode(errors="replace"): v.decode(errors="replace")
                for k, v in (info.properties or {}).items()
                if k is not None and v is not None
            }
            addresses = info.parsed_addresses() or []
            found[name] = Found(
                hostname=(info.server or "").rstrip("."),
                address=addresses[0] if addresses else "",
                port=info.port or 80,
                name=props.get("name", ""),
                version=props.get("fw", ""),
                properties=props,
            )

        # A lamp that goes away mid-browse should not be reported as present.
        def remove_service(self, zc, type_, name):
            found.pop(name, None)

        def update_service(self, zc, type_, name):
            self.add_service(zc, type_, name)

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, SERVICE, Listener())
        # mDNS has no "done": responders answer whenever they feel like it, so
        # the only way to finish a browse is to stop waiting.
        time.sleep(timeout)
    finally:
        zc.close()
    return list(found.values())


def _discover_cli(timeout: float) -> list[Found]:
    """mDNS via whichever browser the OS ships, when zeroconf is unavailable."""
    if shutil.which("dns-sd"):
        return _discover_dns_sd(timeout)
    if shutil.which("avahi-browse"):
        return _discover_avahi(timeout)
    raise LampError(
        "no way to browse mDNS: install the zeroconf package, or use a system "
        "with dns-sd (macOS) or avahi-browse (Linux)."
    )


def _run_for(cmd: list[str], timeout: float) -> str:
    """Run a browser that never exits on its own, and keep what it printed."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.terminate()
    try:
        out, _ = proc.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
    return out or ""


def _discover_dns_sd(timeout: float) -> list[Found]:
    service = SERVICE.replace(".local.", "")
    # -Z gives the zone-file form, which carries SRV (host + port) and TXT in
    # one pass instead of a browse followed by a resolve per instance.
    raw = _run_for(["dns-sd", "-Z", service, "local"], timeout)

    lamps: list[Found] = []
    for line in raw.splitlines():
        fields = line.split()
        # name._glowlamp._tcp  SRV  0 0 80 host.local. ; Replace with...
        if len(fields) >= 6 and fields[1] == "SRV":
            host = fields[5].rstrip(".")
            lamps.append(Found(hostname=host, address="", port=int(fields[4])))

    # dns-sd -Z does not resolve addresses, so each name still needs a lookup;
    # without it every request would pay the .local resolution cost instead.
    for lamp in lamps:
        lamp.address = _resolve_dns_sd(lamp.hostname, timeout=2.0)
    return lamps


def _resolve_dns_sd(hostname: str, timeout: float) -> str:
    raw = _run_for(["dns-sd", "-G", "v4", hostname], timeout)
    for line in raw.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[1] == "Add":
            return fields[5]
    return ""


def _discover_avahi(timeout: float) -> list[Found]:
    service = SERVICE.replace(".local.", "")
    raw = _run_for(["avahi-browse", "-rpt", service], timeout)

    lamps = []
    for line in raw.splitlines():
        if not line.startswith("="):
            continue
        # =;iface;proto;name;type;domain;host;address;port;txt
        parts = line.split(";")
        if len(parts) < 10:
            continue
        props = {}
        for item in re.findall(r'"([^"]*)"', parts[9]):
            key, _, value = item.partition("=")
            props[key] = value
        lamps.append(
            Found(
                hostname=parts[6],
                address=parts[7],
                port=int(parts[8]) if parts[8].isdigit() else 80,
                name=props.get("name", ""),
                version=props.get("fw", ""),
                properties=props,
            )
        )
    return lamps


# =========================================================================
# Command line
# =========================================================================


def _targets(args) -> list[Lamp]:
    """The lamps a command applies to, from --host / --name / --all."""
    if args.host:
        return [Lamp(args.host)]

    found = discover(args.timeout)
    if not found:
        raise LampError(
            "no lamps found. Check that this machine is on the same subnet -- "
            "mDNS does not cross VLANs or a guest network."
        )

    if args.name:
        wanted = args.name.lower()
        matched = [f for f in found if wanted in f.label.lower()]
        if not matched:
            names = ", ".join(f.label for f in found)
            raise LampError(f"no lamp matching {args.name!r}. Found: {names}")
        return [Lamp(f.url) for f in matched]

    if args.all:
        return [Lamp(f.url) for f in found]

    if len(found) == 1:
        return [Lamp(found[0].url)]

    names = ", ".join(f.label for f in found)
    raise LampError(f"{len(found)} lamps found ({names}). Use --all, --name or --host.")


def _describe(status: dict) -> str:
    name = status.get("name") or status.get("hostname", "?")
    ota = status.get("ota", {})
    mq = status.get("mqtt", {})
    fx = status.get("effect", {})
    effect = fx.get("name", "?")
    if fx and not fx.get("default", True):
        left = fx.get("expires_in", -1)
        effect += "*" if left < 0 else f"*{left}s"
    bits = [
        f"{name:<20}",
        f"{status.get('power', '?'):<3}",
        f"bright {status.get('brightness', '?'):>3}",
        f"{effect:<12}",
        f"v{status.get('version', '?')}",
    ]
    if mq.get("enabled"):
        bits.append("mqtt" if mq.get("connected") else "mqtt!")
    if ota.get("available"):
        bits.append(f"-> v{ota.get('latest')} available")
    return "  ".join(bits)


def main(argv=None) -> int:
    # Which lamps a command applies to. Attached to each subcommand rather than
    # to the top-level parser, so these read the way anyone would type them --
    # "off --all", not "--all off". Argparse only accepts an option before the
    # subcommand when it is defined on the parent, and only after when it is
    # defined on the child; the natural order is the one that gets to work.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--host", help="address or .local name of one lamp; skips discovery")
    common.add_argument("--name", help="match a discovered lamp by name (substring)")
    common.add_argument("--all", action="store_true", help="every lamp found")
    common.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"seconds to browse for lamps (default {DEFAULT_TIMEOUT:g})")
    common.add_argument("--json", action="store_true", help="print raw JSON")

    parser = argparse.ArgumentParser(
        description="Find and control Glow Lamps.",
        epilog="With no --host/--name/--all, a command runs against the only lamp found.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def add(name, help_text):
        return sub.add_parser(name, help=help_text, parents=[common])

    add("discover", "list the lamps on this network")
    add("status", "what each lamp is doing")
    add("on", "switch on")
    add("off", "switch off")
    add("toggle", "switch to the opposite state")
    p_bright = add("brightness", "set brightness 0-255")
    p_bright.add_argument("value", type=int)
    p_id = add("identify", "blink white to find a lamp")
    p_id.add_argument("--seconds", type=int, default=4)
    # --set-name, not --name: --name already means "which lamp" on every
    # subcommand, and one flag cannot mean both which and what.
    p_fx = add("effect", "set the effect and colors")
    p_fx.add_argument("effect", choices=["blend", "loop", "flicker", "neon"])
    p_fx.add_argument("--colors", help="up to 5, comma separated: '#ff0000,#0000ff'")
    p_fx.add_argument("--seconds", type=int, default=300,
                      help="revert after this long; 0 = until reboot (default 300)")
    add("effects", "list the effects a lamp supports")
    p_broker = add("broker", "set the MQTT broker (empty host turns MQTT off)")
    p_broker.add_argument("host", help="broker address, or '' to disable MQTT")
    p_broker.add_argument("--port", type=int, default=1883)
    p_broker.add_argument("--user", default="")
    p_broker.add_argument("--password", default="")
    add("default", "back to the default effect and palette")
    p_name = add("rename", "set the lamp name and/or hostname")
    p_name.add_argument("--set-name", dest="new_name", help="the free-text lamp name")
    p_name.add_argument("--set-hostname", dest="new_hostname", help="the mDNS label")
    p_update = add("update", "check for a new release, and install it")
    p_update.add_argument("--check", action="store_true", help="check only, install nothing")
    add("reboot", "reboot")

    args = parser.parse_args(argv)

    try:
        if args.command == "discover":
            found = discover(args.timeout)
            if args.json:
                print(json.dumps([f.__dict__ for f in found], indent=2))
            elif not found:
                print("No lamps found.")
                print("mDNS does not cross VLANs or a guest network, and an unprovisioned")
                print("lamp is not on your network at all -- look for its setup AP instead.")
            else:
                for f in found:
                    version = f" v{f.version}" if f.version else ""
                    print(f"{f.label:<20} {f.url:<28} {f.hostname}{version}")
            return 0

        lamps = _targets(args)
        results = []

        for lamp in lamps:
            if args.command == "status":
                result = lamp.status()
            elif args.command == "on":
                result = lamp.power(True)
            elif args.command == "off":
                result = lamp.power(False)
            elif args.command == "toggle":
                result = lamp.toggle()
            elif args.command == "brightness":
                result = lamp.brightness(args.value)
            elif args.command == "identify":
                result = lamp.identify(args.seconds)
            elif args.command == "effect":
                colors = [c.strip() for c in args.colors.split(",")] if args.colors else None
                result = lamp.set_effect(args.effect, colors, args.seconds)
            elif args.command == "effects":
                result = lamp.effects()
            elif args.command == "broker":
                result = lamp.set_broker(args.host, args.port, args.user, args.password)
            elif args.command == "default":
                result = lamp.reset_effect()
            elif args.command == "rename":
                result = lamp.rename(args.new_name, args.new_hostname)
            elif args.command == "reboot":
                result = lamp.reboot()
            elif args.command == "update":
                if args.check:
                    lamp.check_for_update()
                    # The check is a round trip to GitHub on the device, so the
                    # answer is not in the response that acknowledged it.
                    time.sleep(4)
                    result = lamp.status()
                else:
                    # The lamp decides: it installs only if the release differs,
                    # so there is nothing to poll and nothing to decide here.
                    result = lamp.update()
            else:  # pragma: no cover - argparse rejects anything else
                raise LampError(f"unknown command {args.command}")

            results.append(result)

        if args.command == "effects":
            for result in results:
                for e in result.get("effects", []):
                    print(f"{e['name']:<10} {e['description']}")
                print(f"up to {result.get('max_colors')} colors, "
                      f"{result.get('max_seconds')}s maximum")
            return 0

        if args.json:
            print(json.dumps(results if len(results) > 1 else results[0], indent=2))
        elif args.command in ("status", "update"):
            for result in results:
                print(_describe(result))
        else:
            for result in results:
                print(_describe(result))
        return 0

    except LampError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
