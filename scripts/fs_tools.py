#!/usr/bin/env python3
"""Backup / restore LittleFS data (signals, remotes, actions) over HTTP.

uploadfs wipes the device filesystem — ALWAYS backup before flashing FS,
and restore afterwards.

Usage:
  python3 scripts/fs_backup.py                  # → backups/<timestamp> + backups/latest
  python3 scripts/fs_restore.py                 # from backups/latest
  python3 scripts/fs_restore.py backups/2026…   # from a specific folder
  python3 scripts/flash_safe.py                 # firmware only (keeps FS)
  python3 scripts/flash_safe.py --fs            # backup → uploadfs → restore
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKUP_ROOT = ROOT / "backups"
DEFAULT_BASES = (
    "http://192.168.20.245",
    "http://cc1101.local",
    "http://192.168.20.243",
)


def api(base: str, path: str, data=None, method=None, timeout=20):
    url = base.rstrip("/") + path
    body = None
    headers = {}
    if data is not None:
        body = json.dumps(data).encode()
        headers["Content-Type"] = "application/json"
        method = method or "POST"
    req = urllib.request.Request(url, data=body, headers=headers, method=method or "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def find_base(bases=DEFAULT_BASES) -> str:
    last = None
    for b in bases:
        try:
            api(b, "/api/status", timeout=3)
            return b
        except Exception as e:
            last = e
    raise SystemExit(f"device not reachable ({last})")


def wait_up(base: str, seconds: float = 45) -> bool:
    end = time.time() + seconds
    while time.time() < end:
        try:
            api(base, "/api/status", timeout=2)
            return True
        except Exception:
            time.sleep(0.5)
    return False


def backup(base: str | None = None, out: Path | None = None) -> Path:
    base = base or find_base()
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out = out or (BACKUP_ROOT / stamp)
    out.mkdir(parents=True, exist_ok=True)

    sigs = []
    for s in api(base, "/api/signals").get("signals", []):
        full = api(base, f"/api/signal?id={s['id']}")
        sigs.append(full.get("signal") or full)
    rems = api(base, "/api/remotes").get("remotes", [])
    acts = api(base, "/api/actions").get("actions", [])

    (out / "signals.json").write_text(json.dumps(sigs, indent=2))
    (out / "remotes.json").write_text(json.dumps(rems, indent=2))
    (out / "actions.json").write_text(json.dumps(acts, indent=2))
    (out / "meta.json").write_text(
        json.dumps({"base": base, "signals": len(sigs), "remotes": len(rems), "actions": len(acts)}, indent=2)
    )

    latest = BACKUP_ROOT / "latest"
    if latest.exists():
        if latest.is_dir() and not latest.is_symlink():
            shutil.rmtree(latest)
        else:
            latest.unlink()
    shutil.copytree(out, latest)

    print(f"backed up {len(sigs)} signals, {len(rems)} remotes, {len(acts)} actions → {out}", flush=True)
    return out


def restore(base: str | None = None, src: Path | None = None, remotes_reboot: bool = True) -> None:
    base = base or find_base()
    src = src or (BACKUP_ROOT / "latest")
    if not src.is_dir():
        raise SystemExit(f"no backup at {src}")

    if not wait_up(base):
        raise SystemExit("device did not come up")

    sigs = json.loads((src / "signals.json").read_text()) if (src / "signals.json").exists() else []
    rems = json.loads((src / "remotes.json").read_text()) if (src / "remotes.json").exists() else []
    acts = json.loads((src / "actions.json").read_text()) if (src / "actions.json").exists() else []

    for s in sigs:
        keys = (
            "id", "name", "frequency", "modulation", "rssi", "timestamp",
            "pulseCount", "durationUs", "estimatedRepeats", "gapBetweenRepeatsUs",
            "pulses", "bitCodes", "bitMidUs",
        )
        payload = {k: s[k] for k in keys if k in s}
        # map common field names
        if "frequencyMHz" in s and "frequency" not in payload:
            payload["frequency"] = s["frequencyMHz"]
        if "rssiDbm" in s and "rssi" not in payload:
            payload["rssi"] = s["rssiDbm"]
        try:
            r = api(base, "/api/signal/import", payload)
            print("sig", payload.get("name"), r.get("id") or r)
        except Exception as e:
            print("sig FAIL", payload.get("name"), e)

    for a in acts:
        payload = {
            "id": a.get("id") or "",
            "name": a.get("name") or "Action",
            "steps": a.get("steps") or [],
        }
        try:
            r = api(base, "/api/actions", payload)
            print("action", payload["name"], r.get("id"))
        except Exception as e:
            print("action FAIL", payload["name"], e)

    for i, rmt in enumerate(rems):
        payload = {
            "id": rmt.get("id") or "",
            "name": rmt.get("name") or "Device",
            "type": rmt.get("type") or "remote",
            "buttons": rmt.get("buttons") or [],
            "weatherProtocol": rmt.get("weatherProtocol"),
            "weatherId": rmt.get("weatherId"),
            "weatherChannel": rmt.get("weatherChannel"),
            "reboot": remotes_reboot and i == len(rems) - 1,
        }
        # drop nulls
        payload = {k: v for k, v in payload.items() if v is not None}
        try:
            r = api(base, "/api/remotes", payload)
            print("remote", payload["name"], r.get("id"), "reboot" if payload.get("reboot") else "")
        except Exception as e:
            print("remote FAIL", payload["name"], e)

    print("restore done")


def flash(fs: bool = False, port: str | None = None) -> None:
    BACKUP_ROOT.mkdir(parents=True, exist_ok=True)
    base = None
    try:
        base = find_base()
        backup(base)
    except SystemExit as e:
        print(f"warning: could not backup before flash ({e})")
        if fs:
            raise SystemExit("refusing --fs without a successful backup")

    cmd = ["pio", "run", "-e", "esp32-c3-supermini", "-t", "upload"]
    if port:
        cmd += ["--upload-port", port]
    print(">>", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=ROOT)

    if fs:
        cmd = ["pio", "run", "-e", "esp32-c3-supermini", "-t", "uploadfs"]
        if port:
            cmd += ["--upload-port", port]
        print(">>", " ".join(cmd), flush=True)
        subprocess.check_call(cmd, cwd=ROOT)
        # Device reboots; restore from latest
        time.sleep(2)
        base = base or find_base()
        if not wait_up(base, 60):
            raise SystemExit("device down after uploadfs")
        restore(base, BACKUP_ROOT / "latest", remotes_reboot=True)
    else:
        print("firmware flashed; LittleFS left intact (no uploadfs)", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", nargs="?", choices=("backup", "restore", "flash"), help="or use dedicated scripts")
    p.add_argument("--base", default=None)
    p.add_argument("--src", type=Path, default=None, help="backup folder for restore")
    p.add_argument("--fs", action="store_true", help="also flash LittleFS (backup+restore)")
    p.add_argument("--port", default=None)
    args = p.parse_args()

    name = Path(sys.argv[0]).name
    if name.startswith("fs_backup"):
        backup(args.base)
    elif name.startswith("fs_restore"):
        restore(args.base, args.src)
    elif name.startswith("flash_safe") or args.cmd == "flash":
        flash(fs=args.fs, port=args.port)
    elif args.cmd == "backup":
        backup(args.base)
    elif args.cmd == "restore":
        restore(args.base, args.src)
    else:
        p.print_help()


if __name__ == "__main__":
    main()
