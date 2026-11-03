#pragma once

// Small OS layer for the desktop viewer: the few things GLFW does not cover. Windows
// implementations in platform.cpp; other platforms get plain fallbacks (the viewer ships for
// Windows only, but the dev host builds wherever GLFW and Vulkan do).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace pocketsplat::platform {

// The command line as paths/strings that survive non-ASCII characters (on Windows argv is in the
// ANSI code page; this reads the wide command line instead). Element 0 is the program.
std::vector<std::filesystem::path> command_line(int argc, char** argv);

// UTF-8 text (GLFW drop paths, ImGui) <-> path.
std::filesystem::path path_from_utf8(const std::string& text);
std::string utf8_from_path(const std::filesystem::path& path);

// Modal error box (the viewer has no console). Falls back to stderr.
void show_error(const std::string& title, const std::string& message);

// Native "open file" dialog for splat scenes; nullopt when cancelled.
std::optional<std::filesystem::path> open_scene_dialog(GLFWwindow* window);

// Directory of the running executable (bundled samples live next to it).
std::filesystem::path executable_dir();

// Per-user cache for imported scenes (%LOCALAPPDATA%\PocketSplat\imports on Windows); created.
std::filesystem::path import_cache_dir();

}  // namespace pocketsplat::platform
