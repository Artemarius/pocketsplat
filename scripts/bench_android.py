#!/usr/bin/env python3
"""On-device benchmark driver for the Android host (docs/benchmarks/README.md protocol).

Per scene: force-stops the app, launches the intent-driven benchmark (deterministic auto-orbit,
vsync off/MAILBOX, protocol internal resolution, per-frame CSV recorded by the native host),
samples `dumpsys thermalservice` alongside from the host, then pulls the CSV via run-as
(debug builds only) and emits into --out-dir:

  frame-<device>-<hash>-<scene><variant>-raw.csv    per-frame rows (analysis; too big to commit)
  frame-<device>-<hash>-<scene><variant>-cold.csv   per-frame rows, first --cold-frames frames
  frame-<device>-<hash>-<scene><variant>-5s.csv     5-second aggregate, protocol columns
  thermal-<device>-<hash>-<scene><variant>.csv      host-side samples: thermal status, skin/AP
                                                    temperature, kgsl GPU busy % and clock, battery
                                                    current / voltage / charge counter (dumpsys battery)

where <variant> is empty for the protocol defaults, else e.g. "-16bit", "-c0.1" (contribution
cull threshold) or "-ctl" (the ADR-0009 quality controller moves the threshold), "-540x975"
(resolution override), "-cap53" (frame cap; "-cap53-vote" with --vote-fps), "-home" (--pose home), in that order, and <hash> is the git hash the
installed APK was built from (its versionName), "-dirty" when the build had uncommitted changes.

and prints the cold (first 30 s) vs sustained (minute 8-10) summary the protocol asks for.

Protocol reminders (docs/benchmarks/README.md): phone at room temperature per scene, screen
brightness fixed, charger disconnected — use Wi-Fi adb for sustained runs (`adb tcpip 5555`,
`adb connect <phone>:5555`, set ANDROID_SERIAL or --serial). Scenes must already be in the
app's files/scenes/ (see android/README.md); --push stages local files first.

Usage:
  python scripts/bench_android.py --scenes museum1.pspk truck_1m.pspk
  python scripts/bench_android.py --scenes truck_1m.pspk --seconds 60 --key-bits 16
  python scripts/bench_android.py --scenes stump-full.sh0.pspk --seconds 60 --min-contribution 0.1
  python scripts/bench_android.py --scenes stump-full.sh0.pspk --target-fps 53   # equal-work run
  python scripts/bench_android.py --scenes stump-full.sh0.pspk --key-bits 16 --controller --target-fps 60
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PACKAGE = "com.pocketsplat.app"
ACTIVITY = f"{PACKAGE}/.MainActivity"
# SplatRenderer::DrawMode order (intent extra bench_draw_mode = index)
DRAW_MODES = ["vertex6", "instanced", "indexed", "triangle"]
# SplatRenderer::DrawProbe order: timing-only variants of the triangle draw (wrong image by design),
# sent in the same extra as bench_draw_mode + len(DRAW_MODES) * probe index
DRAW_PROBES = ["none", "seq", "tiny", "clip", "noblend", "nofrag", "copy"]


def adb(*args: str, serial: str | None = None, binary: bool = False, timeout: int = 120):
    cmd = ["adb"] + (["-s", serial] if serial else []) + list(args)
    result = subprocess.run(cmd, capture_output=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)} failed: {result.stderr.decode(errors='replace')}")
    return result.stdout if binary else result.stdout.decode(errors="replace")


def shell(command: str, serial: str | None = None) -> str:
    """One command string, re-parsed by the device shell (quoting inside works)."""
    return adb("shell", command, serial=serial)


def pull_app_file(path: str, serial: str | None = None) -> bytes:
    """Reads a file from the debug app's sandbox (exec-out keeps the bytes intact)."""
    return adb("exec-out", "run-as", PACKAGE, "cat", path, serial=serial, binary=True)


def thermal(serial: str | None) -> tuple[int, float, float]:
    """(status, skin_c, ap_c) from dumpsys thermalservice's HAL temperature section."""
    out = adb("shell", "dumpsys", "thermalservice", serial=serial)
    status = int(m.group(1)) if (m := re.search(r"Thermal Status: (\d+)", out)) else -1
    section = out.split("Current temperatures from HAL")[-1]
    skin = ap = float("nan")
    if m := re.search(r"mValue=([\d.]+), mType=3", section):
        skin = float(m.group(1))
    if m := re.search(r"mValue=([\d.]+), mType=0", section):
        ap = float(m.group(1))
    return status, skin, ap


# Adreno busy % and clock (kgsl sysfs) and the fuel gauge as `dumpsys battery` reports it, in one
# shell round trip (the power_supply sysfs nodes are not readable from adb shell on the Samsung
# phones). At a capped frame rate the governor holds busy near a target and lowers the clock
# instead, so the clock, not busy %, shows how much work a frame takes.
POWER_SHELL = ("echo busy=$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null); "
               "echo mhz=$(cat /sys/class/kgsl/kgsl-3d0/clock_mhz 2>/dev/null); dumpsys battery")


def gpu_power(serial: str | None) -> tuple[float, float, float, float]:
    """(gpu_busy_pct, gpu_mhz, current_ma, voltage_mv, charge_counter_uah), nan when unavailable.
    Current is Samsung's `current now` (negative = discharging; microamps on the S25U); the charge
    counter's drop over a run is the charge it drew."""
    nan = float("nan")
    try:
        out = shell(POWER_SHELL, serial=serial)
    except (RuntimeError, subprocess.TimeoutExpired):
        return nan, nan, nan, nan, nan

    def field(pattern: str) -> float:
        return float(m.group(1)) if (m := re.search(pattern, out, re.MULTILINE)) else nan

    return (field(r"^busy=(\d+)"), field(r"^mhz=(\d+)"), field(r"current now: (-?\d+)"),
            field(r"^\s*voltage: (\d+)"), field(r"Charge counter: (\d+)"))


def apk_build(serial: str | None) -> str | None:
    """Git hash the installed APK was built from (versionName "<x.y.z>-<hash>[-dirty]"), or None for
    builds that predate the hash in the versionName."""
    out = adb("shell", "dumpsys", "package", PACKAGE, serial=serial)
    m = re.search(r"versionName=\d+(?:\.\d+)*-([0-9a-f]{7,}(?:-dirty)?)\s", out)
    return m.group(1) if m else None


def device_state(serial: str | None) -> str:
    battery = adb("shell", "dumpsys", "battery", serial=serial)
    level = m.group(1) if (m := re.search(r"level: (\d+)", battery)) else "?"
    powered = "yes" if re.search(r"(AC|USB|Wireless) powered: true", battery) else "no"
    size = adb("shell", "wm", "size", serial=serial).strip().replace("\n", " ")
    status, skin, ap = thermal(serial)
    return (f"battery {level}%, powered {powered}, {size}, "
            f"thermal status {status}, skin {skin:.1f} C, ap {ap:.1f} C")


def wait_cool(serial: str | None, skin_max: float, timeout_s: int) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        status, skin, _ = thermal(serial)
        if status == 0 and (skin != skin or skin <= skin_max):  # NaN skin: trust the status
            return
        print(f"  cooling: status {status}, skin {skin:.1f} C (target <= {skin_max:.1f})")
        time.sleep(30)
    print("  WARNING: cool-down timed out; starting anyway", file=sys.stderr)


def parse_frame_csv(text: str) -> tuple[dict[str, str], "object"]:
    """Returns (metadata from '# key=value' comment lines, numpy record array of the rows)."""
    import io

    import numpy as np

    meta: dict[str, str] = {}
    for line in text.splitlines():
        if not line.startswith("#"):
            continue
        for part in line[1:].split(", "):
            if "=" in part:
                key, _, value = part.partition("=")
                meta[key.strip()] = value.strip()
    # genfromtxt(names=True) would take the FIRST line as names even when commented -
    # drop the comment lines ourselves.
    data = "\n".join(line for line in text.splitlines() if not line.startswith("#"))
    rows = np.genfromtxt(io.StringIO(data), delimiter=",", names=True)
    return meta, rows


def window_summary(rows, t_lo: float, t_hi: float) -> dict[str, float] | None:
    import numpy as np

    sel = rows[(rows["t_s"] >= t_lo) & (rows["t_s"] < t_hi)]
    if sel.size < 2:
        return None
    span = float(sel["t_s"][-1] - sel["t_s"][0])
    return {
        "fps": (sel.size - 1) / span if span > 0 else float("nan"),
        "gpu": float(sel["gpu_ms"].mean()),
        "gpu_p50": float(np.percentile(sel["gpu_ms"], 50)),
        "gpu_p95": float(np.percentile(sel["gpu_ms"], 95)),
        "cull": float(sel["cull_ms"].mean()),
        "pre": float(sel["preprocess_ms"].mean()),
        "sort": float(sel["sort_ms"].mean()),
        "draw": float(sel["draw_ms"].mean()),
        "visible": float(sel["visible"].mean()),
        # GPU time per wall-clock second from our own timestamps: ~1 = GPU-bound, lower = capped
        "gpu_util": float(sel["gpu_ms"].mean()) * ((sel.size - 1) / span if span > 0 else 0.0) / 1000.0,
        "status": int(sel["thermal_status"].max()),
        "headroom": float(sel["thermal_headroom"][-1]),
    }


def write_aggregate(path: Path, meta: dict[str, str], rows, args, scene: str, build: str) -> None:
    """5-second buckets in the protocol column layout (docs/benchmarks/README.md); the
    importance_threshold column carries the contribution cull threshold (pixels, ADR-0008)."""
    import numpy as np

    res = meta.get("resolution", "0x0")
    width, height = res.split("x") if "x" in res else ("0", "0")
    lines = ["device,build_hash,scene,splats_total,splats_visible,sh_degree,internal_w,internal_h,"
             "key_bits,t_min_s,fps,ms_frame,ms_cull_preprocess,ms_sort,ms_draw,thermal_status,"
             "thermal_headroom,res_scale,importance_threshold"]
    def threshold(sel) -> str:  # per bucket: the controller moves it (mean over the bucket's frames)
        if "min_contribution" in (rows.dtype.names or ()):
            return f"{sel['min_contribution'].mean():.3g}"
        return meta.get("min_contribution", "")

    buckets = np.floor(rows["t_s"] / 5.0).astype(int)
    for bucket in np.unique(buckets):
        sel = rows[buckets == bucket]
        span = float(sel["t_s"][-1] - sel["t_s"][0])
        fps = (sel.size - 1) / span if span > 0 and sel.size > 1 else float("nan")
        lines.append(
            f"{args.device},{build},{scene},{meta.get('splats', '')},{sel['visible'].mean():.0f},"
            f"{meta.get('sh', '')},{width},{height},{meta.get('key_bits', '')},"
            f"{bucket * 5 / 60.0:.3f},{fps:.2f},{sel['gpu_ms'].mean():.3f},"
            f"{(sel['cull_ms'] + sel['preprocess_ms']).mean():.3f},{sel['sort_ms'].mean():.3f},"
            f"{sel['draw_ms'].mean():.3f},{int(sel['thermal_status'].max())},"
            f"{sel['thermal_headroom'][-1]:.3f},{args.res_scale},{threshold(sel)}")
    path.write_text("\n".join(lines) + "\n")


def bench_scene(scene: str, args, build: str) -> bool:
    serial = args.serial
    stem = Path(scene).stem
    suffix = "-16bit" if args.key_bits == 16 else ""
    if args.controller:
        suffix += "-ctl"
    elif args.min_contribution > 0.0:
        suffix += f"-c{args.min_contribution:g}"
    if args.width and args.height:
        suffix += f"-{args.width}x{args.height}"
    if args.target_fps > 0.0:
        suffix += f"-cap{args.target_fps:g}"
        if args.vote_fps:
            suffix += "-vote"
    if args.draw_mode != "vertex6":  # files without a suffix are the 6-vertex quads (all runs before S18)
        suffix += f"-{args.draw_mode}"
    if args.draw_probe != "none":
        suffix += f"-probe-{args.draw_probe}"
    if args.coverage_strength > 0.0:  # coverage fade (ADR-0013); needs <stem>.pscov in files/scenes
        suffix += f"-cov{args.coverage_strength:g}"
    if args.pose == "home":
        suffix += "-home"
    tag = f"{args.device}-{build}-{stem}{suffix}"
    device_csv = f"files/bench/bench-{stem}-{args.key_bits}bit.csv"

    listing = shell(f"run-as {PACKAGE} ls files/scenes", serial=serial).split()
    if scene not in listing:
        print(f"ERROR: {scene} not in files/scenes (found: {', '.join(listing) or 'nothing'})",
              file=sys.stderr)
        return False

    adb("shell", "am", "force-stop", PACKAGE, serial=serial)  # stop first: an idle app slows cooling
    if args.wait_cool:
        wait_cool(serial, args.cool_skin, args.cool_timeout)
    print(f"[{scene}] {device_state(serial)}")

    adb("shell", "input", "keyevent", "KEYCODE_WAKEUP", serial=serial)  # no surface = no bench
    adb("shell", "wm", "dismiss-keyguard", serial=serial)
    shell(f"run-as {PACKAGE} sh -c 'rm -f {device_csv} {device_csv}.tmp'", serial=serial)
    extras = ["--es", "bench_scene", scene, "--ei", "bench_seconds", str(args.seconds),
              "--ei", "bench_key_bits", str(args.key_bits),
              "--ef", "bench_min_contribution", str(args.min_contribution),
              "--ef", "bench_target_fps", str(args.target_fps),
              "--ei", "bench_draw_mode",
              str(DRAW_MODES.index(args.draw_mode) + len(DRAW_MODES) * DRAW_PROBES.index(args.draw_probe)),
              "--ez", "bench_controller", "true" if args.controller else "false",
              "--ez", "bench_vote_fps", "true" if args.vote_fps else "false",
              "--ef", "coverage_strength", str(args.coverage_strength),
              "--es", "bench_pose", args.pose]
    if args.width and args.height:  # e.g. a web viewer's canvas size, for like-for-like pixels
        extras += ["--ei", "bench_width", str(args.width), "--ei", "bench_height", str(args.height)]
    adb("shell", "am", "start", "-n", ACTIVITY, *extras, serial=serial)
    print(f"[{scene}] running {args.seconds} s (key_bits {args.key_bits}, "
          + ("quality controller" if args.controller else f"min_contribution {args.min_contribution:g}")
          + (f", capped at {args.target_fps:g} fps)" if args.target_fps > 0.0 else ")"))

    thermal_rows = ["t_s,thermal_status,skin_c,ap_c,gpu_busy_pct,gpu_mhz,current_ma,voltage_mv,charge_uah"]
    start = time.monotonic()
    deadline = start + args.seconds + 120  # loading + stop/write slack
    done = False
    while time.monotonic() < deadline:
        status, skin, ap = thermal(serial)
        busy, mhz, current, voltage, charge = gpu_power(serial)
        t_s = time.monotonic() - start
        thermal_rows.append(f"{t_s:.1f},{status},{skin:.2f},{ap:.2f},{busy:.0f},{mhz:.0f},{current:.0f},"
                            f"{voltage:.0f},{charge:.0f}")
        if int(t_s) % 60 < args.thermal_interval:
            print(f"  t={t_s / 60.0:4.1f} min  thermal status {status}, skin {skin:.1f} C, gpu busy {busy:.0f} %")
        probe = shell(f"run-as {PACKAGE} sh -c 'test -f {device_csv} && echo yes || echo no'",
                      serial=serial)
        if probe.strip() == "yes":
            done = True
            break
        time.sleep(args.thermal_interval)
    if not done:
        print(f"ERROR: bench CSV never appeared at {device_csv}", file=sys.stderr)
        return False

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"thermal-{tag}.csv").write_text("\n".join(thermal_rows) + "\n")
    text = pull_app_file(device_csv, serial=serial).decode()
    # What was drawn (the run ends on the scene), then stop the viewer: it keeps drawing the scene
    # every vsync and the next run's cool-down would never finish.
    (out_dir / f"frame-{tag}.png").write_bytes(adb("exec-out", "screencap", "-p", serial=serial, binary=True))
    adb("shell", "am", "force-stop", PACKAGE, serial=serial)
    raw_path = out_dir / f"frame-{tag}-raw.csv"
    raw_path.write_text(text)

    meta, rows = parse_frame_csv(text)
    lines = text.splitlines(keepends=True)
    comments = [line for line in lines if line.startswith("#")]  # comment lines lead the file
    header = "".join(comments) + lines[len(comments)]
    body = lines[len(comments) + 1:]
    (out_dir / f"frame-{tag}-cold.csv").write_text(header + "".join(body[:args.cold_frames]))
    write_aggregate(out_dir / f"frame-{tag}-5s.csv", meta, rows, args, scene, build)

    print(f"[{scene}] {rows.size} frames, {meta.get('resolution', '?')} internal, "
          f"present_mode={meta.get('present_mode', '?')}, target_fps={meta.get('target_fps', '0')}")
    if "reason" in (rows.dtype.names or ()) and meta.get("controller") == "on":
        import numpy as np

        reasons =["none", "frame_miss", "thermal_high", "thermal_low", "ladder_exhausted"]
        for row in rows[rows["reason"] > 0]:
            print(f"  controller t={row['t_s']:6.1f} s {reasons[int(row['reason'])]:16s} after c "
                  f"{row['min_contribution']:g}, forecast {row['thermal_forecast']:.3f}, {row['gpu_mhz']:.0f} MHz")
        dt = np.diff(rows["t_s"], append=rows["t_s"][-1])
        for c in np.unique(rows["min_contribution"]):
            print(f"  time at c {c:g}: {dt[rows['min_contribution'] == c].sum():.0f} s")
    for name, lo, hi in [("cold (0-30 s)", 0.0, 30.0),
                         ("sustained (min 8-10)", 480.0, 600.0)]:
        if (s := window_summary(rows, lo, hi)) is None:
            print(f"  {name}: not covered by this run")
            continue
        print(f"  {name}: {s['fps']:6.1f} fps | gpu {s['gpu']:.2f} (p50 {s['gpu_p50']:.2f}, "
              f"p95 {s['gpu_p95']:.2f}) ms | cull {s['cull']:.2f} pre {s['pre']:.2f} "
              f"sort {s['sort']:.2f} draw {s['draw']:.2f} | gpu util {s['gpu_util']:.2f} | "
              f"visible {s['visible']:.0f} | thermal {s['status']}, headroom {s['headroom']:.2f}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenes", nargs="+", required=True,
                        help="scene filenames inside the app's files/scenes/")
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--key-bits", type=int, default=32, choices=[16, 32])
    parser.add_argument("--min-contribution", type=float, default=0.0,
                        help="contribution cull threshold in pixels of full coverage (0 = off)")
    parser.add_argument("--target-fps", type=float, default=0.0,
                        help="frame cap (0 = uncapped); equal-work runs against a paced viewer")
    parser.add_argument("--controller", action="store_true",
                        help="run the thermal-adaptive quality controller (ADR-0009): it moves the "
                             "contribution threshold along its ladder; --min-contribution is ignored")
    parser.add_argument("--coverage-strength", type=float, default=0.0,
                        help="coverage fade strength 0..1 (ADR-0013); the scene needs its "
                             "<stem>.pscov sidecar in files/scenes (--push it)")
    parser.add_argument("--pose", default="orbit", choices=["orbit", "home"],
                        help="orbit: full turn about +y around the framing box every 600 frames (every run "
                             "before S26); home: +-30 deg swing about the scene's start view, phase 2pi/600 "
                             "per frame (bench_web.py --pose home gives SuperSplat the same path)")
    parser.add_argument("--vote-fps", action="store_true",
                        help="ask the compositor for --target-fps, so the panel runs at that rate as in "
                             "interactive use (without it the panel stays at 120 Hz at every cap)")
    parser.add_argument("--draw-mode", default="triangle", choices=DRAW_MODES,
                        help="how the draw emits each splat's quad (same image; A/B of per-splat vertex work)")
    parser.add_argument("--draw-probe", default="none", choices=DRAW_PROBES,
                        help="timing-only variant of the triangle draw (wrong image by design): seq = no "
                             "order[] gather, tiny = ~0-area primitives, clip = primitives rejected by clipping, "
                             "noblend = blending off, nofrag = every fragment discarded first; copy = records copied into "
                             "draw order after the sort (right image, no gather; the copy is timed in the sort)")
    parser.add_argument("--serial", default=None, help="adb serial (else ANDROID_SERIAL/default)")
    parser.add_argument("--device", default="s24u", help="device tag used in output filenames")
    parser.add_argument("--out-dir", default=str(REPO / "bench-out"),
                        help="output directory (curated files are copied to docs/benchmarks)")
    parser.add_argument("--apk", default=None, help="install this APK first")
    parser.add_argument("--push", nargs="*", default=[],
                        help="local scene files to stage into files/scenes first")
    parser.add_argument("--cold-frames", type=int, default=600,
                        help="frames kept in the committed cold extract")
    parser.add_argument("--thermal-interval", type=float, default=5.0)
    parser.add_argument("--wait-cool", action="store_true",
                        help="before each scene, wait for thermal status 0 and skin <= --cool-skin")
    parser.add_argument("--cool-skin", type=float, default=36.0)
    parser.add_argument("--cool-timeout", type=int, default=900)
    parser.add_argument("--width", type=int, default=0, help="override the protocol internal width")
    parser.add_argument("--height", type=int, default=0, help="override the protocol internal height")
    parser.add_argument("--res-scale", default="0.6",
                        help="recorded in the aggregate CSV (protocol internal scale)")
    args = parser.parse_args()

    if args.apk:
        print(f"installing {args.apk}")
        adb("install", "-r", args.apk, serial=args.serial, timeout=300)
    build = apk_build(args.serial)
    if build is None:  # an APK from before the hash went into versionName: label from the checkout
        build = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True,
                               text=True, cwd=REPO).stdout.strip() or "unknown"
        print(f"WARNING: installed APK has no git hash in its versionName; labelling files {build} "
              "(the checkout), which may not be the build that runs", file=sys.stderr)
    print(f"build {build} (installed APK)")
    for local in args.push:
        name = Path(local).name
        print(f"pushing {name}")
        adb("push", local, f"/data/local/tmp/{name}", serial=args.serial, timeout=600)
        shell(f"run-as {PACKAGE} sh -c 'mkdir -p files/scenes && "
              f"cp /data/local/tmp/{name} files/scenes/'", serial=args.serial)
        adb("shell", "rm", f"/data/local/tmp/{name}", serial=args.serial)

    ok = True
    for scene in args.scenes:
        ok = bench_scene(scene, args, build) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
