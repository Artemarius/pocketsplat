#!/usr/bin/env python3
"""Hand-held interactive session on the Android host, recorded as an interactive trace.

Launches the viewer on a scene with an interactive trace (intent extra interactive_trace: every
quality-controller sample of interactive use - rendered frames and 1 Hz thermal ticks - in the
bench CSV format, column `drawn` 0 for ticks), samples thermal state and power from the host
while someone uses the phone, then pulls the trace and prints a summary: time spent
interacting, frame rate and misses while interacting, time at each rung, the controller's
decisions, and whether `splatconv replay` reproduces them.

Outputs in --out-dir:
  trace-<device>-<hash>-<scene>-<name>.csv     the app's trace (replay input)
  thermal-<device>-<hash>-<scene>-<name>.csv   host-side samples, the bench_android.py columns

Run hygiene as in the benchmark protocol (docs/benchmarks/README.md): fixed brightness, Do Not
Disturb and heads-up notifications off for the session, all restored afterwards; Wi-Fi adb with
the phone unplugged (`adb tcpip 5555`, `adb connect <phone>:5555`).

Usage:
  python scripts/session_android.py --serial <ip>:5555 --device s24u --name handheld1 --minutes 20
  python scripts/session_android.py --summarize bench-out/<dir>/trace-....csv
"""
import argparse
import json
import math
import subprocess
import sys
import time
from pathlib import Path

from bench_android import ACTIVITY, PACKAGE, REPO, adb, apk_build, device_state, gpu_power, pull_app_file, shell, thermal

REASONS = ["none", "frame_miss", "thermal_high", "thermal_low", "ladder_exhausted"]
SETTINGS = [("system", "screen_brightness_mode"), ("system", "screen_brightness"),
            ("system", "screen_off_timeout"), ("global", "zen_mode"),
            ("global", "heads_up_notifications_enabled")]
DND = {"0": "off", "1": "priority", "2": "on", "3": "alarms"}  # zen_mode -> cmd notification set_dnd


def read_trace(path: Path) -> tuple[dict[str, str], list[dict[str, float]]]:
    """(header key=value pairs, rows) of a trace; comment lines may appear between rows."""
    meta: dict[str, str] = {}
    names: list[str] | None = None
    rows: list[dict[str, float]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        if line.startswith("#"):
            for part in line[1:].split(","):
                key, eq, value = part.strip().partition("=")
                if eq:
                    meta.setdefault(key, value)
        elif names is None:
            names = line.split(",")
        else:
            fields = line.split(",")
            if len(fields) == len(names):  # a truncated last line is skipped
                rows.append({n: float(v) for n, v in zip(names, fields)})
    return meta, rows


def replay(path: Path) -> dict | None:
    """`splatconv replay` on the trace, None when the tool is not built."""
    for exe in (REPO / "build/tools/splatconv/Release/splatconv.exe", REPO / "build/tools/splatconv/splatconv"):
        if exe.exists():
            out = subprocess.run([str(exe), "replay", str(path)], capture_output=True, text=True)
            return json.loads(out.stdout) if out.stdout.strip() else None
    return None


def summarize(path: Path, thermal_csv: Path | None = None) -> None:
    meta, rows = read_trace(path)
    if not rows:
        print(f"{path.name}: no rows")
        return
    target = float(meta.get("target_fps", "60"))
    t = [r["t_s"] for r in rows]
    frames = [r for r in rows if r.get("drawn", 1.0) == 1.0]
    counted = [r for r in frames if math.isfinite(r["cpu_ms"])]
    active_s = sum(r["cpu_ms"] for r in counted) / 1000.0
    misses = sum(1 for r in counted if r["cpu_ms"] > 1.1 * 1000.0 / target)
    gpu = [r["gpu_ms"] for r in counted if math.isfinite(r["gpu_ms"]) and r["gpu_ms"] > 0.0]
    mhz = [r["gpu_mhz"] for r in counted if r["gpu_mhz"] > 0]
    print(f"{path.name}: {t[-1] / 60.0:.1f} min, {len(frames)} frames, {len(rows) - len(frames)} ticks; "
          f"{meta.get('scene', '?')} at {meta.get('resolution', '?')}")
    if counted:
        print(f"  interacting {active_s / 60.0:.1f} min ({100.0 * active_s / max(t[-1], 1e-9):.0f} % of the session): "
              f"{len(counted) / active_s:.1f} fps, {100.0 * misses / len(counted):.1f} % of intervals > 1.1x "
              f"{1000.0 / target:.2f} ms, GPU {sum(gpu) / max(len(gpu), 1):.2f} ms at "
              f"{sum(mhz) / max(len(mhz), 1):.0f} MHz mean")
    # Wall-clock time at each threshold, as splatconv replay counts it.
    at: dict[float, float] = {}
    for r, nxt in zip(rows, rows[1:]):
        at[r["min_contribution"]] = at.get(r["min_contribution"], 0.0) + nxt["t_s"] - r["t_s"]
    print("  time at threshold: " + ", ".join(f"c {c:g} {s / 60.0:.1f} min" for c, s in sorted(at.items()) if c > 0))
    forecasts = [r["thermal_forecast"] for r in rows if r["thermal_sample"] == 1]
    currents = [r["thermal_headroom"] for r in rows if r["thermal_sample"] == 2]
    print(f"  thermal: status max {max(r['thermal_status'] for r in rows):.0f}, forecast max "
          f"{max(forecasts, default=float('nan')):.3f}, current max {max(currents, default=float('nan')):.3f} "
          f"(LIGHT {float(meta.get('light_headroom', 'nan')):.3f})")
    if thermal_csv is not None and thermal_csv.exists():
        lines = [l.split(",") for l in thermal_csv.read_text().splitlines()[1:] if l]
        skins = [float(l[2]) for l in lines if l[2] != "nan"]
        if skins:
            print(f"  skin (host): {skins[0]:.1f} -> {skins[-1]:.1f} C, max {max(skins):.1f} C")
    for r in rows:
        if r["reason"] != 0:
            print(f"  t={r['t_s']:7.1f} s  {REASONS[int(r['reason'])]:16s} after c {r['min_contribution']:g} "
                  f"(forecast {r['thermal_forecast']:.3f}, clock {r['gpu_mhz']:.0f} MHz)")
    result = replay(path)
    if result is not None:
        print(f"  replay: reproduces_log {result.get('reproduces_log')}, "
              f"{result.get('logged_decisions', 0)} logged decisions")


def save_settings(serial: str) -> dict[tuple[str, str], str]:
    return {key: shell(f"settings get {key[0]} {key[1]}", serial=serial).strip() for key in SETTINGS}


def restore_settings(serial: str, saved: dict[tuple[str, str], str]) -> None:
    for (namespace, name), value in saved.items():
        if name == "zen_mode":
            if value in DND:
                shell(f"cmd notification set_dnd {DND[value]}", serial=serial)
        elif value and value != "null":
            shell(f"settings put {namespace} {name} {value}", serial=serial)
    shell("svc power stayon false", serial=serial)


def wait_cool_held(serial: str, skin_max: float, hold: int) -> None:
    """Skin <= skin_max on `hold` consecutive samples 30 s apart (protocol step 4)."""
    held = 0
    while held < hold:
        status, skin, _ = thermal(serial)
        held = held + 1 if status == 0 and skin <= skin_max else 0
        print(f"  cooling: status {status}, skin {skin:.1f} C, held {held}/{hold}")
        if held < hold:
            time.sleep(30)


def run_session(args) -> int:
    serial = args.serial
    build = apk_build(serial) or "unknown"
    tag = f"{args.device}-{build}-{Path(args.scene).stem}-{args.name}"
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    if "powered yes" in device_state(serial):
        print("WARNING: the phone is on external power (charging heat skews the session)", file=sys.stderr)
    saved = save_settings(serial)
    print("saved settings: " + ", ".join(f"{k[1]}={v}" for k, v in saved.items()))
    adb("shell", "am", "force-stop", PACKAGE, serial=serial)
    try:
        shell("settings put system screen_brightness_mode 0", serial=serial)
        shell("settings put system screen_brightness 128", serial=serial)
        shell("settings put system screen_off_timeout 1800000", serial=serial)
        shell("svc power stayon true", serial=serial)
        shell("cmd notification set_dnd on", serial=serial)
        shell("settings put global heads_up_notifications_enabled 0", serial=serial)  # DND lets some through
        if args.cool_skin > 0:
            wait_cool_held(serial, args.cool_skin, args.cool_hold)
        print(f"[{tag}] {device_state(serial)}")
        adb("shell", "input", "keyevent", "KEYCODE_WAKEUP", serial=serial)
        adb("shell", "wm", "dismiss-keyguard", serial=serial)
        adb("shell", "am", "start", "-n", ACTIVITY, "--es", "open_scene", args.scene,
            "--es", "interactive_trace", args.name, serial=serial)
        print(f"[{tag}] viewer open for {args.minutes:g} min - use the phone now")

        rows = ["t_s,thermal_status,skin_c,ap_c,gpu_busy_pct,gpu_mhz,current_ma,voltage_mv,charge_uah"]
        start = time.monotonic()
        while (t_s := time.monotonic() - start) < 60.0 * args.minutes:
            status, skin, ap = thermal(serial)
            busy, mhz, current, voltage, charge = gpu_power(serial)
            rows.append(f"{t_s:.1f},{status},{skin:.2f},{ap:.2f},{busy:.0f},{mhz:.0f},{current:.0f},"
                        f"{voltage:.0f},{charge:.0f}")
            if int(t_s) % 60 < args.interval:
                print(f"  t={t_s / 60.0:4.1f} min  status {status}, skin {skin:.1f} C, gpu {busy:.0f} % at {mhz:.0f} MHz")
            time.sleep(args.interval)
        (out_dir / f"thermal-{tag}.csv").write_text("\n".join(rows) + "\n")
        adb("shell", "input", "keyevent", "KEYCODE_HOME", serial=serial)  # onPause flushes the trace
        time.sleep(3)
        trace = out_dir / f"trace-{tag}.csv"
        trace.write_bytes(pull_app_file(f"files/trace/{args.name}.csv", serial=serial))
    finally:
        adb("shell", "am", "force-stop", PACKAGE, serial=serial)
        restore_settings(serial, saved)
        print("settings restored")
    summarize(trace, out_dir / f"thermal-{tag}.csv")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--summarize", type=Path, help="only summarize an existing trace CSV")
    parser.add_argument("--serial", default=None, help="adb serial (Wi-Fi adb: <ip>:5555)")
    parser.add_argument("--device", default="s24u", help="device tag used in output filenames")
    parser.add_argument("--scene", default="stump-full.sh0.pspk", help="scene inside the app's files/scenes/")
    parser.add_argument("--name", default="handheld", help="trace name (files/trace/<name>.csv on the phone)")
    parser.add_argument("--minutes", type=float, default=20.0)
    parser.add_argument("--interval", type=float, default=2.0, help="host-side sampling period, seconds")
    parser.add_argument("--cool-skin", type=float, default=33.0, help="start only below this skin C (0 = no wait)")
    parser.add_argument("--cool-hold", type=int, default=6, help="... on this many samples 30 s apart")
    parser.add_argument("--out-dir", default=str(REPO / "bench-out"))
    args = parser.parse_args()
    if args.summarize is not None:
        name = args.summarize.name
        thermal_csv = args.summarize.with_name("thermal-" + name[6:]) if name.startswith("trace-") else None
        summarize(args.summarize, thermal_csv)
        return 0
    return run_session(args)


if __name__ == "__main__":
    sys.exit(main())
