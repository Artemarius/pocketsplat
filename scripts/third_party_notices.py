#!/usr/bin/env python3
"""Third-party licence notices for the shipped binaries (Windows viewer zip, Android APK).

    python scripts/third_party_notices.py android [--deps build/_deps]
        rewrites android/app/src/main/assets/third_party_notices.txt (committed; the app's
        licences screen shows it)

package_windows.py imports notices("windows", deps) for the zip. The licence texts come from the
FetchContent checkouts of a configured build (<build>/_deps), which are pinned to tags in
cmake/deps.cmake. Re-run the android form whenever a pinned dependency or the app's Gradle
dependencies change. Standard library only.
"""
import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Native components compiled into both binaries: (name, use, FetchContent source dir, licence files).
NATIVE = [
    ("spz (nianticlabs)", "SPZ reading", "spz-src", ["LICENSE"]),
    ("zstd (facebook), used under BSD", "SPZ compression", "zstd-src", ["LICENSE"]),
    ("zlib", "SPZ gzip", "zlib-src", ["LICENSE"]),
    ("GLM", "math", "glm-src", ["copying.txt"]),
    ("volk", "Vulkan loader", "volk-src", ["LICENSE.md"]),
    ("Vulkan Memory Allocator (AMD)", "GPU memory", "vma-src", ["LICENSE.txt"]),
    ("vk-bootstrap", "Vulkan setup", "vk_bootstrap-src", ["LICENSE.txt"]),
    ("Vulkan-Headers (Khronos), used under MIT", "Vulkan headers", "vulkan_headers-src",
     ["LICENSE.md", "LICENSES/MIT.txt"]),
]
WINDOWS_ONLY = [
    ("Dear ImGui", "interface", "imgui-src", ["LICENSE.txt"]),
    ("GLFW", "window and input", "glfw-src", ["LICENSE.md"]),
]
# JVM libraries in the APK (releaseRuntimeClasspath), all Apache License 2.0; none ships a NOTICE file.
ANDROID_APACHE = [
    "AndroidX: Jetpack Compose (runtime, ui, foundation, animation, material, material3), Activity,"
    " Lifecycle, Core, Annotation, Collection, Arch Core, SavedState, Emoji2, Startup, Tracing,"
    " ProfileInstaller, Autofill, Concurrent, CustomView, Graphics, Interpolator,"
    " VersionedParcelable (The Android Open Source Project)",
    "Kotlin standard library (JetBrains)",
    "kotlinx.coroutines (JetBrains)",
    "JetBrains Java Annotations (JetBrains)",
    "Guava ListenableFuture (Google)",
]
APACHE_TEXT = ("vulkan_headers-src", "LICENSES/Apache-2.0.txt")

RULE = "=" * 78


def _section(title, text):
    return f"\n{RULE}\n{title}\n{RULE}\n\n{text.strip()}\n"


def _read(deps, src, name):
    path = deps / src / name
    if not path.is_file():
        sys.exit(f"missing {path}: configure a build first (cmake -B build) or pass --deps")
    return path.read_text(encoding="utf-8", errors="replace")


def notices(target, deps):
    deps = Path(deps)
    if target == "windows":
        parts = ["Third-party components compiled into pocketsplat-viewer.exe, with their licence texts.\n"
                 "Sources are fetched at pinned versions by the CMake build (THIRD_PARTY.md in the repository).\n"]
        components = NATIVE + WINDOWS_ONLY
    elif target == "android":
        parts = ["PocketSplat is MIT-licensed. This app contains the third-party components below; their\n"
                 "licence texts follow. The bundled sample scene (hornedlizard.spz) is a sample asset of\n"
                 "nianticlabs/spz under that repository's MIT licence (the spz section).\n",
                 _section("PocketSplat", (REPO / "LICENSE").read_text(encoding="utf-8"))]
        components = NATIVE
    else:
        raise ValueError(target)
    for name, use, src, files in components:
        parts.append(_section(f"{name} - {use}", "\n\n".join(_read(deps, src, f).strip() for f in files)))
    if target == "android":
        listing = "\n".join(f"* {c}" for c in ANDROID_APACHE)
        parts.append(_section("Apache License 2.0 - the following libraries",
                              listing + "\n\n" + _read(deps, *APACHE_TEXT)))
    return "".join(parts).replace("\r\n", "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", choices=["android"])
    parser.add_argument("--deps", default="build/_deps", help="FetchContent dir of a configured build")
    args = parser.parse_args()
    out = REPO / "android/app/src/main/assets/third_party_notices.txt"
    out.parent.mkdir(parents=True, exist_ok=True)
    text = notices(args.target, REPO / args.deps)
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out.relative_to(REPO)} ({len(text)} chars)")


if __name__ == "__main__":
    main()
