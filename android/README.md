# PocketSplat Android host

Gradle project wrapping `splatcore` + `splatrender` (built from the repository root via
`externalNativeBuild`) behind a JNI bridge, with a Compose shell around a SurfaceView-hosted
Vulkan swapchain. Requires the SDK's CMake 4.1.2 and a Vulkan SDK on the host for
`glslangValidator` (shaders are compiled to SPIR-V at build time, as on desktop).

```
cd android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Release build

```
cd android
./gradlew assembleRelease     # app/build/outputs/apk/release/app-release.apk (R8-shrunk, ~23 MB)
```

The APK is signed when a release key is configured, and is left unsigned
(`app-release-unsigned.apk`) otherwise. Keys never go into git. One-time setup:

```
keytool -genkeypair -keystore ~/keys/pocketsplat-release.jks -alias pocketsplat \
        -keyalg RSA -keysize 4096 -validity 10000
```

then either `android/keystore.properties` (gitignored, like `*.jks` / `*.keystore`):

```
storeFile=C:/Users/<you>/keys/pocketsplat-release.jks
storePassword=...
keyAlias=pocketsplat
keyPassword=...
```

or the environment variables `POCKETSPLAT_KEYSTORE`, `POCKETSPLAT_KEYSTORE_PASSWORD`,
`POCKETSPLAT_KEY_ALIAS` and `POCKETSPLAT_KEY_PASSWORD` (they override the file). Check with
`apksigner verify --print-certs app/build/outputs/apk/release/app-release.apk`. A release-signed
APK cannot be installed over a debug build (different signer): uninstall first, which deletes
the app's imported scenes.

Every build bundles two assets:

* `samples/hornedlizard.spz` (nianticlabs/spz sample, MIT), imported and opened on the first
  interactive launch. Scripted launches (bench or `open_scene` extras) skip it. The Gradle task
  takes `data/raw/hornedlizard.spz` or the root build's spz checkout when the hash matches, and
  otherwise downloads the file from the spz tag that `cmake/deps.cmake` pins.
* `third_party_notices.txt`, shown under "About & licences". It is generated and committed, so
  re-run `python scripts/third_party_notices.py android` (needs a configured root build for
  `build/_deps`) after changing a pinned dependency or a Gradle dependency.

## Getting scenes onto the device

Two ways in:

**In-app (SAF)** — "Open PLY/SPZ file…" on the scene-list screen opens the system file picker
(Downloads, Drive, anywhere SAF reaches; no storage permission needed). The pick is copied into
the app cache, header-probed (count/SH shown before anything heavy runs), then imported on a
background thread: the full `docs/cleaning.md` pipeline (SOR floater removal is the optional
"deep clean" toggle — it is the slow op), SH truncation to ≤ 1, a splat budget
(500K/1M/2M/all, default 1M), Morton reorder, pack. The resulting `.pspk` lands in
`files/scenes/` — so the import runs once and re-opening is instant. Inputs above 8M splats are
refused (a phone can't hold their float expansion; convert with `splatconv` on a desktop).
Tapping a `.ply`/`.spz` already in the scene list routes through the same import flow.

**adb push** (debug builds) into the app's private `files/scenes/` via `run-as`:

```
adb push data/derived/museum1.pspk /data/local/tmp/
adb shell run-as com.pocketsplat.app sh -c 'mkdir -p files/scenes && cp /data/local/tmp/museum1.pspk files/scenes/'
adb shell rm /data/local/tmp/museum1.pspk
```

`.pspk` opens directly (fast parse; must be pack v2+ from `splatconv pack` — v1 caches are
rejected). Import details go to logcat (per-op cleaning stats).

## Benchmark mode

The viewer overlay has a "Benchmark 10 min" button; `scripts/bench_android.py` drives the same
thing over adb. Either way the run is: deterministic auto-orbit (2π/600 per frame, the desktop
`--bench` rate), vsync off (MAILBOX), internal resolution pinned to 0.6× of the panel's native
mode (the protocol resolution, independent of the current display-mode setting), touch input
ignored, per-frame GPU/pass times + 1 Hz thermal status/headroom recorded natively and written
to `files/bench/bench-<scene>-<bits>bit.csv` when the run ends (atomically; `.tmp` until then).

Intent extras for scripting: `--es bench_scene <name in files/scenes>`,
`--ei bench_seconds N` (default 600), `--ei bench_key_bits 16|32` (default 32),
`--ei bench_width W --ei bench_height H` (override the protocol resolution).

```
python scripts/bench_android.py --scenes museum1.pspk truck_1m.pspk
```

pulls the CSVs, samples `dumpsys thermalservice` host-side, and emits the protocol outputs
(see `docs/benchmarks/README.md`). Sustained runs follow the protocol: battery power, Wi-Fi adb.

## Structure

- `app/src/main/cpp/` — `AndroidHost` (context/swapchain/frame loop over the shared renderer)
  and the JNI bridge. All engine calls run on the app's single render thread.
- `app/src/main/java/` — `RenderEngine` (HandlerThread + Choreographer pacing, camera state),
  `SplatSurfaceView` (touch orbit/pinch, internal-resolution via `setFixedSize`), Compose shell.
- Logs: `adb logcat -s PocketSplat:V`
