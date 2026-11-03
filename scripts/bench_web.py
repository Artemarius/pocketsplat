#!/usr/bin/env python3
"""Web-viewer baseline driver: third-party browser renderers on the same phone and scenes.

Measures pinned builds of the PlayCanvas SuperSplat viewer (WebGPU by default, WebGL2 with
`--viewer supersplat-webgl`, the experimental sort-free renderer with `supersplat-stochastic`) and
Spark (three.js/WebGL2, `scripts/web/spark.html`) in Chrome on an adb-attached phone, so their
numbers sit next to PocketSplat's `scripts/bench_android.py` runs on identical splats.

  fetch   download the pinned npm tarballs (integrity-checked) into .cache/web-viewers/
  run     serve viewer + scene over `adb reverse`, open it in Chrome, drive the camera, record
          per-second fps / frame-time percentiles from requestAnimationFrame via the Chrome
          DevTools protocol, plus the canvas size, the graphics API actually requested and
          `dumpsys thermalservice` samples

Camera: Spark orbits with PocketSplat's bench camera (framing computed from the PLY here). The
SuperSplat viewer keeps its own default framing (85 deg FOV, distance from the full bounding box)
unless `--pose ours` puts it on PocketSplat's bench orbit too (same target, distance, pitch, start
yaw and 60 deg vertical FOV, mapped into the viewer's frame); `--camera orbit` (default) turns its
camera about the focus point by 2pi/600 per rendered frame from inside the page (its CameraManager
pose), the same per-frame step as PocketSplat's bench and the Spark fixture. `--hold-shot out.png`
holds that orbit at its start pose and saves the page instead of measuring - the pose check
against `pocketsplat-desktop --offscreen --yaw-deg 34.3775 --screenshot`. Its renderer draws
on demand, so this keeps it rendering - and sorting - every frame it can; the synthetic one-finger
drags of the Phase 0 baseline (`--camera drag`, files without the `-orbit` label) paced it at the
input rate instead (~72 fps on the S25U whatever the scene). The `raf_fps` column is the page's
requestAnimationFrame rate, the ceiling Chrome grants it, and `gpu_busy_pct` the Adreno kgsl busy
percentage sampled each second (~100 = GPU-bound, lower = paced), next to the GPU clock and the
battery current / voltage / charge counter (as bench_android.py): untouched pages get 60 Hz on the S24U
even at a forced 120 Hz, so the orbit also sends drags by default, for pacing only (`--no-touch`
off; files labelled `-orbit-touch`).
Feed every viewer the same file: export the cleaned, SH-truncated, budget-pruned cloud with
`splatconv clean ... out.ply` so all renderers draw identical splats.

Output (--out-dir): web-<device>-<viewer>-<scene>.csv, one row per second after load, with a
`#` metadata header. Protocol as docs/benchmarks/README.md (brightness fixed, charger off for
sustained runs over Wi-Fi adb). Stdlib + numpy only (CLAUDE.md §4).

Usage:
  python scripts/bench_web.py fetch
  python scripts/bench_web.py run --viewer supersplat --scene path/to/truck_1m.sh0.ply --seconds 90
  python scripts/bench_web.py run --viewer spark --scene path/to/truck_1m.sh0.ply --seconds 600
"""
import argparse
import base64
import csv
import functools
import hashlib
import http.client
import http.server
import io
import json
import os
import re
import socket
import statistics
import struct
import subprocess
import sys
import tarfile
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
CACHE = REPO / ".cache" / "web-viewers"
WEB = Path(__file__).resolve().parent / "web"

# Pinned third-party builds (all MIT). Integrity = npm's dist.integrity (sha512, base64).
PACKAGES = {
    "supersplat-viewer": ("https://registry.npmjs.org/@playcanvas/supersplat-viewer/-/supersplat-viewer-1.41.0.tgz",
                          "sha512-sAisV60LYHXnQ8EZbC0Wx63ul8URgXJ342/f/B3b1Aa19hCRJ1BnC8T5QOLmSfni9Lq+62a/n8owFxRC9pKYkg==",
                          "1.41.0", ["package/public/"]),
    "spark": ("https://registry.npmjs.org/@sparkjsdev/spark/-/spark-2.4.0.tgz",
              "sha512-aUjgBfhwDRcF3nUwiAD6Nh7IwvxEmW9liz2qrAXIm2BlPYBmn81QbXIpGFLBSr/o0TKGvEXbNIsKujgPGV/7QQ==",
              "2.4.0", ["package/dist/spark.module.js", "package/dist/assets/"]),
    "three": ("https://registry.npmjs.org/three/-/three-0.180.0.tgz",
              "sha512-o+qycAMZrh+TsE01GqWUxUIKR1AL0S8pq7zDkYOQw8GqfX8b8VoCKYUoHbhiX5j+7hr8XsuHDVU6+gkQJQKg9w==",
              "0.180.0", ["package/build/three.module.js", "package/build/three.core.js", "package/examples/jsm/"]),
}

VIEWERS = {
    # name: (package, url query extras)
    "supersplat": ("supersplat-viewer", ""),
    "supersplat-webgl": ("supersplat-viewer", "&webgl"),
    "supersplat-stochastic": ("supersplat-viewer", "&stochastic=mobile"),
    # Diagnostic: the viewer's FrameGate off (framesInFlight 0), i.e. GPU-bound throughput rather
    # than its latency-limited default pacing.
    "supersplat-nogate": ("supersplat-viewer", "&nogate"),
    "spark": ("spark", ""),
}

# Installed before any page script: records the graphics contexts requested and every
# requestAnimationFrame timestamp (drained by the sampler once per second).
PAGE_HOOK = r"""
(() => {
  const getContext = HTMLCanvasElement.prototype.getContext;
  window.__ctx = [];
  HTMLCanvasElement.prototype.getContext = function (type, ...rest) {
    window.__ctx.push(type);
    return getContext.call(this, type, ...rest);
  };
  window.__frames = [];
  const tick = (t) => { window.__frames.push(t); requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
})();
"""

# SuperSplat continuous orbit (--camera orbit): once per rendered frame (postrender), set the
# CameraManager pose to the next yaw step about the focus point it had at install. Its update() is
# stubbed so only the orbit moves the camera (drags sent with --touch, to keep Chrome at the panel
# rate, cannot); the viewer's update handler applies the pose and its camera-change detection
# requests the next frame through its frame gate, as during a real drag. The step runs on
# 'frameupdate', which the engine fires before 'update' - stepping in 'update' after the viewer's
# own handler applied the pose one tick late and rendered every other tick. The new position comes
# from the viewer's calcFocusPoint for the new angles, so no Euler convention is assumed here;
# window.__orbit.drift() reports how far the focus moved (should stay ~0).
# __POSE__ is null (viewer default framing) or PocketSplat's bench orbit in the viewer's world
# (pose_ours); window.__orbit.hold stops the orbit for --hold-shot.
ORBIT_JS = r"""
window.__viewer.then(v => {
  if (window.__orbit) return window.__orbit.info;
  const cm = v.__viewer && v.__viewer.cameraManager;
  if (!cm || !cm.update) return 'no cameraManager';
  const cam = cm.camera;
  const pose = __POSE__;
  const focus = cam.position.clone();
  cam.calcFocusPoint(focus);
  const arm = cam.position.clone();
  const eye = cam.position.clone();
  const pitch = cam.angles.x, yaw0 = cam.angles.y, roll = cam.angles.z;
  const stepDeg = 360 / 600;
  const place = (k) => {
    if (pose && pose.home) {  // swing about the start view: arm turned about home.up (Rodrigues)
      const h = pose.home, a = pose.amp * Math.PI / 180 * Math.sin(2 * Math.PI * (k % 600) / 600);
      const ax = h.up, c = Math.cos(a), s = Math.sin(a);
      const v = [h.eye[0] - h.focus[0], h.eye[1] - h.focus[1], h.eye[2] - h.focus[2]];
      const d = ax[0] * v[0] + ax[1] * v[1] + ax[2] * v[2];
      const x = [ax[1] * v[2] - ax[2] * v[1], ax[2] * v[0] - ax[0] * v[2], ax[0] * v[1] - ax[1] * v[0]];
      eye.set(focus.x + v[0] * c + x[0] * s + ax[0] * d * (1 - c),
              focus.y + v[1] * c + x[1] * s + ax[1] * d * (1 - c),
              focus.z + v[2] * c + x[2] * s + ax[2] * d * (1 - c));
      cam.look(eye, focus);
      cam.fov = pose.fov;
      return;
    }
    if (pose) {
      const yaw = pose.yaw0 + k * 2 * Math.PI / 600, cp = Math.cos(pose.pitch);
      eye.set(focus.x - pose.distance * cp * Math.sin(yaw), focus.y + pose.distance * Math.sin(pose.pitch),
              focus.z - pose.distance * cp * Math.cos(yaw));
      cam.look(eye, focus);
      cam.fov = pose.fov;
      return;
    }
    cam.angles.set(pitch, yaw0 + k * stepDeg, roll);
    cam.position.set(0, 0, 0);
    cam.calcFocusPoint(arm);
    cam.position.set(focus.x - arm.x, focus.y - arm.y, focus.z - arm.z);
  };
  if (pose) {
    focus.set(pose.focus[0], pose.focus[1], pose.focus[2]);
    place(0);
  }
  cm.update = () => {};
  let rendered = true, steps = 0;
  v.app.on('postrender', () => { rendered = true; });
  v.app.on('frameupdate', () => {
    if (!rendered || window.__orbit.hold) return;
    rendered = false;
    steps++;
    place(steps);
  });
  window.__orbit = {
    hold: false,
    info: {focus: [focus.x, focus.y, focus.z], distance: cam.distance, pitch: cam.angles.x, fov: cam.fov},
    drift: () => { const f = cam.position.clone(); cam.calcFocusPoint(f);
                   return [f.distance(focus) / cam.distance, steps]; },
  };
  return window.__orbit.info;
})
"""


def pose_home(bench_csv: Path) -> dict:
    """The start-view swing (bench_android.py --pose home) in SuperSplat's world, from the app's CSV.

    The app records the HomeView it computed (splatcore::estimate_home_view, RUB world) in its bench
    CSV header; RUB (x, y, z) is (-x, y, -z) in the viewer's world (pose_ours). Same swing as the
    app: the eye turned about home.up through home.focus by 30 deg * sin(2pi k / 600) at rendered
    frame k, 60 deg vertical FOV. cam.look() keeps the viewer's +y up, the app looks with home.up:
    the two images agree in roll only where home.up is close to +y."""
    header = " ".join(line for line in bench_csv.read_text(encoding="utf-8").splitlines() if line.startswith("#"))
    home = {}
    for key in ("focus", "eye", "up"):
        m = re.search(rf"home_{key}=([-0-9.e]+);([-0-9.e]+);([-0-9.e]+)", header)
        if not m:
            sys.exit(f"{bench_csv}: no home_{key} in the header (a bench_android.py --pose home CSV?)")
        x, y, z = (float(g) for g in m.groups())
        home[key] = [-x, y, -z]
    return {"home": home, "focus": home["focus"], "amp": 30.0, "fov": 60.0}


def pose_ours(facts: dict) -> dict:
    """PocketSplat's bench orbit (android_host.cpp, desktop main.cpp) in SuperSplat's world.

    Ours: RUB world = PLY (x, -y, -z); eye = target + d (cos p sin yaw, sin p, cos p cos yaw),
    d = 1.2 x robust diagonal, p = 0.3 rad, yaw from 0.6 rad, 60 deg vertical FOV. SuperSplat turns
    the PLY entity 180 deg about z: world = PLY (-x, -y, z), i.e. our RUB world turned 180 deg about
    y, so the target becomes (-cx, -cy, cz) and the horizontal arm flips sign (ORBIT_JS)."""
    c = facts["center"]
    return {"focus": [-c[0], -c[1], c[2]], "distance": 1.2 * facts["diag"], "pitch": 0.3, "yaw0": 0.6,
            "fov": 60.0}

SUPERSPLAT_SETTINGS = {
    "version": 2, "tonemapping": "none", "highPrecisionRendering": False,
    "background": {"color": [0, 0, 0]},
    "postEffectSettings": {
        "sharpness": {"enabled": False, "amount": 0},
        "bloom": {"enabled": False, "intensity": 0.1, "blurLevel": 2},
        "grading": {"enabled": False, "brightness": 1, "contrast": 1, "saturation": 1, "tint": [1, 1, 1]},
        "vignette": {"enabled": False, "intensity": 0.5, "inner": 0.3, "outer": 0.75, "curvature": 1},
        "fringing": {"enabled": False, "intensity": 0.5},
    },
    "animTracks": [], "cameras": [], "annotations": [], "startMode": "default",
}


# --------------------------------------------------------------------------------------------
# fetch


def package_dir(name: str) -> Path:
    return CACHE / f"{name}-{PACKAGES[name][2]}"


def fetch() -> None:
    for name, (url, integrity, version, keep) in PACKAGES.items():
        target = package_dir(name)
        if target.exists():
            print(f"{name} {version}: cached")
            continue
        data = urllib.request.urlopen(url, timeout=120).read()
        digest = "sha512-" + base64.b64encode(hashlib.sha512(data).digest()).decode()
        if digest != integrity:
            sys.exit(f"{name}: integrity mismatch ({digest})")
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            for member in tar.getmembers():
                if not member.isfile() or not any(member.name.startswith(k) for k in keep):
                    continue
                rel = Path(member.name).relative_to("package")
                out = target / rel
                if ".." in rel.parts:
                    continue
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(tar.extractfile(member).read())
        print(f"{name} {version}: fetched")


# --------------------------------------------------------------------------------------------
# scene facts


def ply_facts(path: Path) -> dict:
    """Splat count, SH degree and PocketSplat's robust framing (5th-95th percentile box)."""
    with open(path, "rb") as f:
        header = b""
        while not header.endswith(b"end_header\n"):
            line = f.readline()
            if not line:
                raise ValueError(f"{path.name}: no PLY header")
            header += line
        offset = f.tell()
    text = header.decode("ascii", errors="replace")
    if "binary_little_endian" not in text:
        raise ValueError(f"{path.name}: only binary little-endian PLY is supported")
    count = int(re.search(r"element vertex (\d+)", text).group(1))
    types = {"float": "<f4", "double": "<f8", "uchar": "u1", "char": "i1", "int": "<i4", "uint": "<u4",
             "short": "<i2", "ushort": "<u2"}
    fields = [(m.group(2), types[m.group(1)]) for m in re.finditer(r"property (\w+) (\w+)", text)]
    data = np.memmap(path, dtype=np.dtype(fields), mode="r", offset=offset, shape=(count,))
    step = max(1, count // 2_000_000)  # percentiles from <= 2M samples
    xyz = np.stack([np.asarray(data[k][::step], dtype=np.float64) for k in ("x", "y", "z")], axis=1)
    lo, hi = np.percentile(xyz, 5, axis=0), np.percentile(xyz, 95, axis=0)
    rest = sum(1 for name, _ in fields if name.startswith("f_rest_"))
    sh = {0: 0, 9: 1, 24: 2, 45: 3}.get(rest, -1)
    return {"splats": count, "sh": sh, "center": ((lo + hi) / 2).tolist(),
            "diag": float(np.linalg.norm(hi - lo))}


# --------------------------------------------------------------------------------------------
# local web server


class Site:
    """URL layout: /supersplat/ (viewer, patched index), /spark/ (fixture + vendor), /scenes/."""

    def __init__(self, scene: Path):
        self.scene = scene
        viewer = package_dir("supersplat-viewer") / "public"
        index = (viewer / "index.html").read_text(encoding="utf-8")
        # Keep the viewer handle reachable for the readiness check (state.loaded).
        patched = index.replace(
            "createViewer({ container: document.body, settings, ...options });",
            "window.__viewer = createViewer({ container: document.body, settings, ...options,"
            " ...(new URLSearchParams(location.search).has('nogate') ? { framesInFlight: 0 } : {}) });")
        if patched == index:
            raise RuntimeError("supersplat-viewer index.html changed shape; update the readiness patch")
        # Expose the internal Viewer (its CameraManager) on the handle for the --camera orbit driver.
        script = (viewer / "index.js").read_text(encoding="utf-8")
        patched_script = script.replace("    const handle = {\n        app,\n",
                                        "    const handle = {\n        __viewer: viewer,\n        app,\n")
        if patched_script == script:
            raise RuntimeError("supersplat-viewer index.js changed shape; update the orbit-driver patch")
        self.files = {
            "/supersplat/index.html": patched.encode("utf-8"),
            "/supersplat/index.js": patched_script.encode("utf-8"),
            "/supersplat/settings.json": json.dumps(SUPERSPLAT_SETTINGS).encode("utf-8"),
            "/spark/index.html": (WEB / "spark.html").read_bytes(),
        }
        self.dirs = {
            "/supersplat/": viewer,
            "/spark/vendor/spark/": package_dir("spark") / "dist",
            "/spark/vendor/three/": package_dir("three") / "build",
            "/spark/vendor/three-addons/": package_dir("three") / "examples" / "jsm",
        }

    def resolve(self, url_path: str):
        path = urllib.parse.unquote(url_path.split("?", 1)[0])
        if path in self.files:
            return self.files[path]
        if path == "/scenes/" + self.scene.name:
            return self.scene
        for prefix, root in self.dirs.items():
            if path.startswith(prefix):
                candidate = (root / path[len(prefix):]).resolve()
                if root.resolve() in candidate.parents and candidate.is_file():
                    return candidate
        return None


def make_handler(site: Site):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):  # quiet
            pass

        def do_GET(self):
            target = site.resolve(self.path)
            if target is None:
                self.send_error(404)
                return
            data = target if isinstance(target, bytes) else None
            size = len(data) if data is not None else target.stat().st_size
            start, end = 0, size - 1
            ranged = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
            if ranged:
                start = int(ranged.group(1))
                end = int(ranged.group(2)) if ranged.group(2) else size - 1
            self.send_response(206 if ranged else 200)
            ctype = {".js": "text/javascript", ".html": "text/html", ".css": "text/css", ".json": "application/json",
                     ".wasm": "application/wasm"}.get(Path(self.path.split("?", 1)[0]).suffix,
                                                      "application/octet-stream")
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Access-Control-Allow-Origin", "*")
            if ranged:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            try:
                if data is not None:
                    self.wfile.write(data[start:end + 1])
                    return
                with open(target, "rb") as f:
                    f.seek(start)
                    remaining = end - start + 1
                    while remaining > 0:
                        chunk = f.read(min(1 << 20, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass

    return Handler


# --------------------------------------------------------------------------------------------
# adb + Chrome DevTools protocol


def adb(serial, *args: str, timeout: int = 60) -> str:
    cmd = ["adb"] + (["-s", serial] if serial else []) + list(args)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


# Adreno busy % and clock (kgsl sysfs) and the fuel gauge as `dumpsys battery` reports it, in one
# shell round trip (the power_supply sysfs nodes are not readable from adb shell on the Samsung
# phones). At a capped frame rate the governor holds busy near a target and lowers the clock
# instead, so the clock, not busy %, shows how much work a frame takes.
POWER_SHELL = ("echo busy=$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null); "
               "echo mhz=$(cat /sys/class/kgsl/kgsl-3d0/clock_mhz 2>/dev/null); dumpsys battery")


def gpu_power(serial) -> tuple[float, float, float, float]:
    """(gpu_busy_pct, gpu_mhz, current_ma, voltage_mv, charge_counter_uah), nan when unavailable.
    Current is Samsung's `current now` (negative = discharging; microamps on the S25U); the charge
    counter's drop over a run is the charge it drew."""
    nan = float("nan")
    try:
        out = adb(serial, "shell", POWER_SHELL, timeout=10)
    except (RuntimeError, subprocess.TimeoutExpired):
        return nan, nan, nan, nan, nan

    def field(pattern: str) -> float:
        return float(m.group(1)) if (m := re.search(pattern, out, re.MULTILINE)) else nan

    return (field(r"^busy=(\d+)"), field(r"^mhz=(\d+)"), field(r"current now: (-?\d+)"),
            field(r"^\s*voltage: (\d+)"), field(r"Charge counter: (\d+)"))


def thermal(serial) -> tuple[int, float, float]:
    out = adb(serial, "shell", "dumpsys", "thermalservice")
    status = int(m.group(1)) if (m := re.search(r"Thermal Status: (\d+)", out)) else -1
    section = out.split("Current temperatures from HAL")[-1]
    skin = float(m.group(1)) if (m := re.search(r"mValue=([\d.]+), mType=3", section)) else float("nan")
    ap = float(m.group(1)) if (m := re.search(r"mValue=([\d.]+), mType=0", section)) else float("nan")
    return status, skin, ap


class WebSocket:
    """Minimal RFC 6455 client: text frames out (masked), text frames in, enough for CDP."""

    def __init__(self, url: str):
        parts = urllib.parse.urlparse(url)
        self.sock = socket.create_connection((parts.hostname, parts.port or 80), timeout=60)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {parts.path} HTTP/1.1\r\nHost: {parts.hostname}:{parts.port}\r\n"
                           f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("websocket handshake failed")
            response += chunk
        head, self.buffer = response.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError(head.decode(errors="replace"))

    def send(self, text: str) -> None:
        payload = text.encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            head = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            head = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            head = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = (np.frombuffer(payload, np.uint8) ^ np.resize(np.frombuffer(mask, np.uint8), n)).tobytes()
        self.sock.sendall(head + mask + masked)

    def _read(self, n: int) -> bytes:
        while len(self.buffer) < n:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise ConnectionError("websocket closed")
            self.buffer += chunk
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def recv(self) -> str:
        message = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            opcode = b0 & 0x0F
            if opcode == 8:
                raise ConnectionError("websocket closed by peer")
            if opcode in (9, 10):  # ping/pong: ignore (Chrome does not require pongs here)
                continue
            message += data
            if b0 & 0x80:
                return message.decode(errors="replace")


class Cdp:
    def __init__(self, ws_url: str):
        self.ws = WebSocket(ws_url)
        self.next_id = 0
        self.crashed = False

    def call(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        self.ws.send(json.dumps({"id": self.next_id, "method": method, "params": params or {}}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("method") == "Inspector.targetCrashed":
                self.crashed = True
            if msg.get("id") == self.next_id:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})

    def eval(self, expression: str):
        result = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True,
                                                "awaitPromise": True})
        return result.get("result", {}).get("value")


CDP_PORT = 9333  # host port for the phone's DevTools socket (9222 = a local Chrome, --desktop)


def devtools(path: str, method: str = "GET"):
    request = urllib.request.Request(f"http://127.0.0.1:{CDP_PORT}{path}", method=method)
    body = urllib.request.urlopen(request, timeout=10).read()
    try:
        return json.loads(body or b"null")
    except json.JSONDecodeError:  # /json/close answers in plain text
        return body.decode(errors="replace")


# --------------------------------------------------------------------------------------------
# run


def drag(cdp: Cdp, width: float, height: float, steps: int = 30) -> None:
    """One left-to-right one-finger drag across the middle of the page (~ one orbit segment)."""
    y = height * 0.5
    x0, x1 = width * 0.2, width * 0.8
    point = lambda x: [{"x": x, "y": y, "id": 1}]
    cdp.call("Input.dispatchTouchEvent", {"type": "touchStart", "touchPoints": point(x0)})
    for i in range(1, steps + 1):
        cdp.call("Input.dispatchTouchEvent", {"type": "touchMove", "touchPoints": point(x0 + (x1 - x0) * i / steps)})
        time.sleep(0.02)
    cdp.call("Input.dispatchTouchEvent", {"type": "touchEnd", "touchPoints": []})


def run(args) -> int:
    global CDP_PORT
    CDP_PORT = 9222 if args.desktop else args.cdp_port
    scene = Path(args.scene).resolve()
    facts = ply_facts(scene)
    package, extra = VIEWERS[args.viewer]
    if not package_dir(package).exists() or not package_dir("three").exists():
        sys.exit("run `python scripts/bench_web.py fetch` first")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(Site(scene)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    serial = args.serial or os.environ.get("ANDROID_SERIAL")
    if not args.desktop:
        if args.wait_cool:
            deadline = time.monotonic() + args.cool_timeout
            while (skin := thermal(serial)[1]) > args.cool_skin and time.monotonic() < deadline:
                print(f"cooling: skin {skin:.1f} C > {args.cool_skin} C", flush=True)
                time.sleep(30)
        adb(serial, "reverse", f"tcp:{args.port}", f"tcp:{args.port}")
        adb(serial, "forward", f"tcp:{CDP_PORT}", "localabstract:chrome_devtools_remote")

    scene_url = f"http://localhost:{args.port}/scenes/{urllib.parse.quote(scene.name)}"
    if package == "spark":
        c, d = facts["center"], facts["diag"]
        url = (f"http://localhost:{args.port}/spark/index.html?scene={urllib.parse.quote(scene_url)}"
               f"&cx={c[0]:.6g}&cy={c[1]:.6g}&cz={c[2]:.6g}&diag={d:.6g}")
    else:
        budget = max(1.0, facts["splats"] / 1e6 * 1.05)  # never let the viewer drop splats
        url = (f"http://localhost:{args.port}/supersplat/index.html?content={urllib.parse.quote(scene_url)}"
               f"&noui&noanim&budget={budget:.2f}{extra}")
    if args.pixel_ratio:
        url += f"&pr={args.pixel_ratio}"

    # Open ours (an intent brings Chrome to the foreground), then close every other tab so
    # nothing else renders during the measurement.
    # The phone's DevTools socket only exists while Chrome runs, so list targets after launching.
    def pages():
        try:
            return [t for t in devtools("/json/list") or [] if t.get("type") == "page"]
        except (OSError, http.client.HTTPException):
            return []

    if args.desktop:
        devtools("/json/new?" + urllib.parse.quote(url, safe=""), method="PUT")
    else:
        adb(serial, "shell", "am", "start", "-a", "android.intent.action.VIEW", "-d", f"'{url}'",
            "com.android.chrome")
    deadline = time.monotonic() + 30
    page = None
    while page is None and time.monotonic() < deadline:
        time.sleep(1)
        page = next((t for t in pages() if t.get("url", "").startswith(f"http://localhost:{args.port}/")), None)
    if page is None:
        sys.exit("Chrome tab with the viewer did not appear")
    for target in pages():
        if target["id"] != page["id"]:
            devtools(f"/json/close/{target['id']}")
    cdp = Cdp(page["webSocketDebuggerUrl"])
    cdp.call("Page.enable")
    cdp.call("Inspector.enable")
    cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": PAGE_HOOK})
    if args.desktop:
        cdp.call("Emulation.setTouchEmulationEnabled", {"enabled": True, "maxTouchPoints": 1})
        if args.viewport:  # a phone-shaped page (portrait: the viewer's FOV is then vertical, as ours)
            w, h = (int(v) for v in args.viewport.lower().split("x"))
            cdp.call("Emulation.setDeviceMetricsOverride", {"width": w, "height": h, "deviceScaleFactor": 1,
                                                            "mobile": True})
    cdp.call("Page.reload", {"ignoreCache": True})

    # Spark: the fixture flags readiness. SuperSplat: the viewer renders on demand once loading
    # reaches 100 % and only sets state.loaded after that first real frame, so a drag kicks it;
    # its engine's postrender events (fired inside app.render(), i.e. frames actually rendered)
    # replace the rAF count, which keeps ticking while an on-demand renderer idles. Rows before
    # 2026-10-28 counted 'frameend', which this engine fires on every tick, rendered or not - they
    # are tick rates (an upper bound on the render rate).
    ready_js = ("window.__bench && (window.__bench.error ? 'error:' + window.__bench.error : window.__bench.loaded)"
                if package == "spark" else
                "window.__viewer ? window.__viewer.then(v => { if (!window.__rendered) { window.__rendered = [];"
                " v.app.on('postrender', () => window.__rendered.push(performance.now())); }"
                " return v.state.loaded ? true : v.state.progress; }) : 0")
    t_nav = time.monotonic()
    loaded = False
    page_w, page_h = 0, 0
    while time.monotonic() - t_nav < args.load_timeout:
        time.sleep(1)
        try:
            state = cdp.eval(ready_js)
            if state == 100:
                if not page_w:
                    page_w, page_h = cdp.eval("[innerWidth, innerHeight]")
                drag(cdp, page_w, page_h, steps=5)
        except (ConnectionError, RuntimeError) as err:
            print(f"page lost while loading: {err}")
            break
        if isinstance(state, str) and state.startswith("error:"):
            print(state)
            break
        if state is True:
            loaded = True
            break
        if cdp.crashed:
            print("renderer process crashed while loading")
            break
    load_s = time.monotonic() - t_nav
    chrome = devtools("/json/version") or {}
    model = "desktop" if args.desktop else adb(serial, "shell", "getprop", "ro.product.model").strip()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    orbit = package != "spark" and args.camera == "orbit"
    # Pacing drags: default for the SuperSplat orbit (an untouched page measures Chrome's 60 Hz cap),
    # opt-in for Spark (its earlier rows ran untouched).
    touch = args.touch or (orbit and not args.no_touch)
    ours = orbit and args.pose == "ours"
    home = orbit and args.pose == "home"
    if home and not args.home_from:
        sys.exit("--pose home needs --home-from <bench_android.py --pose home CSV>")
    label = (args.viewer + ("-orbit" if orbit else "") + ("-ourpose" if ours else "") + ("-home" if home else "")
             + ("-touch" if touch else ""))
    out = out_dir / f"web-{args.device}-{label}-{scene.stem}.csv"
    meta = [f"# pocketsplat bench_web.py, viewer={args.viewer} ({package} {PACKAGES[package][2]}), "
            f"browser={chrome.get('Browser', '?')}, model={model}",
            f"# scene={scene.name}, splats={facts['splats']}, sh={facts['sh']}, url_extras={extra or '-'}"]
    if not loaded:
        meta.append(f"# LOAD FAILED after {load_s:.0f} s")
        out.write_text("\n".join(meta) + "\n", encoding="utf-8")
        print(f"{args.viewer}: load failed after {load_s:.0f} s -> {out}")
        server.shutdown()
        return 1

    cdp.eval("navigator.wakeLock && navigator.wakeLock.request('screen').then(() => true, () => false)")
    page_w, page_h = cdp.eval("[innerWidth, innerHeight]")
    canvas = cdp.eval("(() => { const c = [...document.querySelectorAll('canvas')]"
                      ".sort((a, b) => b.width * b.height - a.width * a.height)[0];"
                      " return c ? [c.width, c.height] : [0, 0]; })()")
    contexts = cdp.eval("window.__ctx || []")
    backend = "webgpu" if "webgpu" in contexts else ("webgl2" if "webgl2" in contexts else ",".join(contexts))
    # Spark: splats loaded; SuperSplat: the engine's per-frame gsplat count after budget/LOD (before
    # its per-splat contribution culling) - shows whether the viewer decimated the scene.
    splats_drawn = (cdp.eval("window.__bench ? window.__bench.splats : null") if package == "spark" else
                    cdp.eval("window.__viewer.then(v => v.app.stats.frame.gsplats)"))
    meta.append(f"# load_s={load_s:.1f}, backend={backend}, canvas={canvas[0]}x{canvas[1]}, "
                f"page={page_w}x{page_h}, splats_reported={splats_drawn}")
    if orbit:
        pose = pose_ours(facts) if ours else pose_home(Path(args.home_from)) if home else None
        orbit_info = cdp.eval(ORBIT_JS.replace("__POSE__", json.dumps(pose) if pose else "null"))
        if not isinstance(orbit_info, dict):
            print(f"orbit driver not installed: {orbit_info}")
            server.shutdown()
            return 1
        if home:
            meta.append(f"# camera=home-swing-30deg/600f (bench_android.py --pose home) from {Path(args.home_from).name}: "
                        f"viewer-world home {json.dumps(pose['home'])}, fov 60 deg"
                        + (", touch drags for pacing only (the camera ignores input)" if touch else ""))
        else:
            meta.append(f"# camera=orbit 2pi/600 per rendered frame about focus "
                        f"{[round(c, 4) for c in orbit_info['focus']]}, distance {orbit_info['distance']:.4g}, "
                        f"pitch {orbit_info['pitch']:.1f} deg, fov {orbit_info['fov']:.0f} deg "
                        + ("(PocketSplat bench pose: 1.2 x robust diagonal, start yaw 0.6 rad)" if ours
                           else "(viewer default framing)")
                        + (", touch drags for pacing only (the camera ignores input)" if touch else ""))
    elif package != "spark":
        meta.append("# camera=drag (synthetic one-finger drags, input-paced)")
    print("\n".join(meta))

    if args.hold_shot:
        if not orbit:
            sys.exit("--hold-shot needs the SuperSplat orbit (--camera orbit)")
        cdp.eval("window.__orbit.hold = true")
        time.sleep(3)  # the start pose is applied and rendered; the on-demand renderer then idles
        shot = cdp.call("Page.captureScreenshot", {"format": "png"})
        Path(args.hold_shot).write_bytes(base64.b64decode(shot["data"]))
        print(f"start-pose screenshot ({canvas[0]}x{canvas[1]} canvas) -> {args.hold_shot}")
        cdp.call("Page.navigate", {"url": "about:blank"})
        server.shutdown()
        return 0

    frames_var = "window.__frames" if package == "spark" else "window.__rendered"
    counted = "requestAnimationFrame" if package == "spark" else "engine postrender events"
    meta.append(f"# frames counted from {counted}")
    rows = []
    cdp.eval(f"{frames_var} = []; window.__frames = []")
    t0 = time.monotonic()
    last_thermal = (-1, float("nan"), float("nan"))
    next_thermal = 0.0
    while time.monotonic() - t0 < args.seconds and not cdp.crashed:
        window_start = time.monotonic()
        if (package != "spark" and not orbit) or touch:  # orbit / Spark ignore touch: drags only pace them
            drag(cdp, page_w, page_h)
        else:
            time.sleep(1.0)
        try:
            frames = cdp.eval(f"(() => {{ const f = {frames_var}; {frames_var} = []; return f; }})()") or []
            # page requestAnimationFrame rate: the ceiling Chrome grants the page (vsync, or its
            # adaptive pacing) - shows whether a viewer is render-bound or paced
            raf = (frames if package == "spark" else
                   cdp.eval("(() => { const f = window.__frames; window.__frames = []; return f; })()") or [])
        except (ConnectionError, RuntimeError):
            print("page lost during measurement")
            break
        t = time.monotonic() - t0
        busy, mhz, current, voltage, charge = (float("nan"),) * 5 if args.desktop else gpu_power(serial)
        if t >= next_thermal and not args.desktop:
            last_thermal = thermal(serial)
            next_thermal = t + args.thermal_interval
        deltas = np.diff(np.asarray(frames, dtype=np.float64))
        if len(deltas) == 0:
            continue
        fps = len(deltas) / (deltas.sum() / 1000.0)
        raf_deltas = np.diff(np.asarray(raf, dtype=np.float64))
        raf_fps = len(raf_deltas) / (raf_deltas.sum() / 1000.0) if raf_deltas.sum() > 0 else float("nan")
        rows.append([args.device, label, PACKAGES[package][2], backend, scene.name, facts["splats"],
                     facts["sh"], canvas[0], canvas[1], f"{t:.1f}", f"{fps:.2f}",
                     f"{np.percentile(deltas, 50):.2f}", f"{np.percentile(deltas, 95):.2f}",
                     last_thermal[0], f"{last_thermal[1]:.1f}", f"{last_thermal[2]:.1f}", f"{raf_fps:.2f}",
                     f"{busy:.0f}", f"{mhz:.0f}", f"{current:.0f}", f"{voltage:.0f}", f"{charge:.0f}"])
        print(f"t={t:6.1f}s fps={fps:6.1f} raf={raf_fps:6.1f} gpu={busy:3.0f}% p95={np.percentile(deltas, 95):6.2f} ms"
              f" thermal={last_thermal[0]} ({time.monotonic() - window_start:.2f} s window)", flush=True)

    if orbit and not cdp.crashed:
        drift, steps = cdp.eval("window.__orbit.drift()")
        meta.append(f"# orbit steps={steps}, focus drift={drift:.2e} x distance")
        print(meta[-1])
    with open(out, "w", newline="", encoding="utf-8") as f:
        f.write("\n".join(meta) + "\n")
        writer = csv.writer(f)
        writer.writerow(["device", "viewer", "viewer_version", "backend", "scene", "splats_total", "sh_degree",
                         "canvas_w", "canvas_h", "t_s", "fps", "ms_p50", "ms_p95", "thermal_status", "skin_c",
                         "ap_c", "raf_fps", "gpu_busy_pct", "gpu_mhz", "current_ma", "voltage_mv", "charge_uah"])
        writer.writerows(rows)
    cold = [float(r[10]) for r in rows if float(r[9]) <= 30.0]
    sustained = [float(r[10]) for r in rows if 480.0 <= float(r[9]) <= 600.0]
    print(f"{args.viewer} on {scene.name}: cold (0-30 s) median {statistics.median(cold) if cold else float('nan'):.1f} fps"
          + (f", sustained (min 8-10) median {statistics.median(sustained):.1f} fps" if sustained else "")
          + f" -> {out}")
    if not args.desktop:  # after measuring, so it cannot perturb the numbers: proves what was drawn
        shot = subprocess.run(["adb"] + (["-s", serial] if serial else []) + ["exec-out", "screencap", "-p"],
                              capture_output=True, timeout=60).stdout
        out.with_suffix(".png").write_bytes(shot)
    cdp.call("Page.navigate", {"url": "about:blank"})  # stop rendering; keep Chrome's last tab
    server.shutdown()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("fetch", help="download the pinned viewer builds")
    r = sub.add_parser("run", help="measure one viewer on one scene")
    r.add_argument("--viewer", required=True, choices=sorted(VIEWERS))
    r.add_argument("--scene", required=True, help="binary little-endian 3DGS PLY")
    r.add_argument("--seconds", type=int, default=90)
    r.add_argument("--device", default="s24u", help="device tag used in output filenames")
    r.add_argument("--serial", default=None)
    r.add_argument("--port", type=int, default=8123, help="local web server (adb reverse) port")
    r.add_argument("--cdp-port", type=int, default=9333,
                   help="host port for the phone's DevTools socket (distinct per phone when running two)")
    r.add_argument("--pixel-ratio", default=None, help="Spark only: renderer pixel ratio")
    r.add_argument("--load-timeout", type=int, default=600)
    r.add_argument("--thermal-interval", type=float, default=5.0)
    r.add_argument("--out-dir", default=str(REPO / "bench-out"))
    r.add_argument("--camera", choices=["orbit", "drag"], default="orbit",
                   help="SuperSplat: per-frame in-page orbit (default) or the old synthetic drags")
    r.add_argument("--touch", action="store_true",
                   help="send synthetic drags only to raise Chrome's page frame rate (it paces untouched"
                        " pages at 60 Hz even at a forced 120 Hz); Spark's fixture and the SuperSplat orbit"
                        " driver ignore them for the camera; default on for the SuperSplat orbit")
    r.add_argument("--no-touch", action="store_true", help="SuperSplat orbit without pacing drags (60 Hz cap)")
    r.add_argument("--pose", choices=["viewer", "ours", "home"], default="viewer",
                   help="SuperSplat orbit: the viewer's default framing (default), PocketSplat's bench pose, "
                        "or PocketSplat's start-view swing (bench_android.py --pose home; needs --home-from)")
    r.add_argument("--home-from", default=None,
                   help="--pose home: the app's bench CSV, whose header carries the start view it computed")
    r.add_argument("--hold-shot", default=None,
                   help="SuperSplat orbit: hold the start pose, save a page screenshot (PNG) and exit")
    r.add_argument("--viewport", default=None, help="--desktop only: emulated page size WxH at pixel ratio 1")
    r.add_argument("--wait-cool", action="store_true", help="wait until skin <= --cool-skin first")
    r.add_argument("--cool-skin", type=float, default=36.0)
    r.add_argument("--cool-timeout", type=int, default=900)
    r.add_argument("--desktop", action="store_true",
                   help="harness self-test against a local Chrome started with --remote-debugging-port=9222")
    args = parser.parse_args()
    if args.command == "fetch":
        fetch()
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
