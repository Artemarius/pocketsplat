#!/usr/bin/env python3
"""Build and package the Windows viewer as a release zip.

    python scripts/package_windows.py [--build-dir build-release] [--version 0.3.0] [--no-build]

Configures a separate build with the C++ runtime linked statically (POCKETSPLAT_STATIC_CRT, so no
Visual C++ redistributable is needed), builds pocketsplat-viewer, checks that the exe imports no
MSVC runtime DLL, and writes dist/pocketsplat-viewer-<version>-windows-x64.zip:

    PocketSplat-<version>/
        pocketsplat-viewer.exe
        samples/hornedlizard.spz      opened when the viewer starts without a scene (MIT, nianticlabs/spz)
        README.txt                    requirements, controls, attribution
        LICENSE                       PocketSplat (MIT)
        THIRD_PARTY_NOTICES.txt       licence texts of everything compiled into the exe

Standard library only.
"""
import argparse
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

from third_party_notices import notices

REPO = Path(__file__).resolve().parent.parent

FORBIDDEN_IMPORTS = ("vcruntime", "msvcp", "ucrtbase", "concrt")

README = """PocketSplat viewer {version} for Windows
==========================================

A viewer for 3D Gaussian Splatting scenes (.ply, .spz, .pspk), built on the same Vulkan renderer,
import pipeline and navigation as the PocketSplat Android app.
Source, method and measurements: https://github.com/Artemarius/pocketsplat

Requirements
------------
* Windows 10 or 11, 64-bit.
* A GPU whose driver supports Vulkan 1.3: NVIDIA, AMD and Intel GPUs from about 2016 on, with a
  current driver. If none is found, the viewer says which GPUs it saw and their Vulkan versions;
  updating the graphics driver usually fixes it.
* No installation. Unzip anywhere and run pocketsplat-viewer.exe.
* The exe is not code-signed, so Windows SmartScreen may warn on first start
  ("More info" -> "Run anyway").

Opening scenes
--------------
* Without arguments the viewer opens samples/hornedlizard.spz.
* Drop a .ply, .spz or .pspk file on the window, press O for a file dialog, or pass a path:
      pocketsplat-viewer.exe scene.ply
* .ply and .spz files are imported once (cleaned, SH truncated to degree 1) and cached under
  %LOCALAPPDATA%\\PocketSplat\\imports. Delete that folder to free the space.
* Scenes larger than the GPU's storage-buffer limit are budget-pruned to fit.

Controls
--------
  Left drag            orbit (free trackball; L switches to a level orbit)
  Right / middle drag  pan
  Mouse wheel          zoom
  Double-click         orbit around the point under the cursor
  H or Home            back to the home view (chosen from the scene itself)
  Tab                  hide or show the panel
  F11                  full screen
  O                    open a scene

The panel's "Exact rendering" box switches from the phone's settings (16-bit depth keys, splats
that cover under 0.1 px culled before the sort) to an exact 32-bit sort with no culling.

Options: --gpu NAME picks a GPU by part of its name (e.g. --gpu intel), --validation turns on the
Vulkan validation layers (needs the Vulkan SDK).

Sample scene
------------
samples/hornedlizard.spz is a sample asset of nianticlabs/spz (https://github.com/nianticlabs/spz),
distributed under that repository's MIT licence (Copyright (c) 2024 Niantic Labs; text in
THIRD_PARTY_NOTICES.txt).

Licence
-------
PocketSplat is MIT-licensed (LICENSE). Third-party components and their licences:
THIRD_PARTY_NOTICES.txt.
"""


def run(cmd):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, cwd=REPO)


def pe_imports(path):
    """Names of the DLLs a PE32+ image imports (import directory only)."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path} is not a PE image")
    coff = pe + 4
    sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise ValueError("expected a 64-bit (PE32+) image")
    import_rva = struct.unpack_from("<I", data, opt + 112 + 8)[0]  # data directory 1
    table = []
    sec = opt + opt_size
    for i in range(sections):
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        table.append((vaddr, max(vsize, rsize), rptr))

    def offset(rva):
        for vaddr, size, rptr in table:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + rptr
        raise ValueError(f"rva {rva:#x} outside every section")

    names = []
    desc = offset(import_rva)
    while True:
        fields = struct.unpack_from("<IIIII", data, desc)
        if not any(fields):
            break
        name_off = offset(fields[3])
        names.append(data[name_off:data.index(b"\0", name_off)].decode("ascii"))
        desc += 20
    return names


def app_version():
    """The release version, from the Android app's versionName so both downloads carry the same one."""
    gradle = (REPO / "android" / "app" / "build.gradle.kts").read_text(encoding="utf-8")
    m = re.search(r'versionName\s*=\s*"(\d+\.\d+\.\d+)', gradle)
    if not m:
        sys.exit("no versionName in android/app/build.gradle.kts: pass --version")
    return m.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build-release")
    parser.add_argument("--version", default=None, help="default: the app's versionName (android/app/build.gradle.kts)-<git short hash>")
    parser.add_argument("--out-dir", default="dist")
    parser.add_argument("--no-build", action="store_true", help="package an existing build")
    args = parser.parse_args()

    build = (REPO / args.build_dir).resolve()
    version = args.version or app_version() + "-" + subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, cwd=REPO, check=True).stdout.strip()
    if not args.no_build:
        run(["cmake", "-S", ".", "-B", build, "-DPOCKETSPLAT_BUILD_DESKTOP=ON", "-DPOCKETSPLAT_BUILD_TESTS=OFF",
             "-DPOCKETSPLAT_BUILD_TOOLS=OFF", "-DPOCKETSPLAT_STATIC_CRT=ON"])
        run(["cmake", "--build", build, "--config", "Release", "--target", "pocketsplat-viewer", "-j", "--",
             "/nodeReuse:false"])

    exe = build / "desktop" / "Release" / "pocketsplat-viewer.exe"
    imports = pe_imports(exe)
    bad = [n for n in imports if n.lower().startswith(FORBIDDEN_IMPORTS)]
    if bad:
        sys.exit(f"{exe} imports the MSVC runtime ({', '.join(bad)}): configure with POCKETSPLAT_STATIC_CRT=ON")
    print("imports:", ", ".join(sorted(imports)))

    deps = build / "_deps"
    sample = deps / "spz-src" / "samples" / "hornedlizard.spz"
    out_dir = REPO / args.out_dir
    out_dir.mkdir(exist_ok=True)
    zip_path = out_dir / f"pocketsplat-viewer-{version}-windows-x64.zip"
    root = f"PocketSplat-{version}"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(exe, f"{root}/pocketsplat-viewer.exe")
        z.write(sample, f"{root}/samples/hornedlizard.spz")
        z.write(REPO / "LICENSE", f"{root}/LICENSE")
        z.writestr(f"{root}/README.txt", README.format(version=version).replace("\n", "\r\n"))
        z.writestr(f"{root}/THIRD_PARTY_NOTICES.txt", notices("windows", deps).replace("\n", "\r\n"))
    print(f"wrote {zip_path} ({zip_path.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
