#include "platform.h"

#include <cstdio>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first: commdlg.h and shellapi.h need its types.
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace pocketsplat::platform {

std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string utf8_from_path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

#ifdef _WIN32

namespace {

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

}  // namespace

std::vector<std::filesystem::path> command_line(int, char**) {
    std::vector<std::filesystem::path> out;
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args == nullptr) return out;
    for (int i = 0; i < count; ++i) out.emplace_back(args[i]);
    LocalFree(args);
    return out;
}

void show_error(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
    MessageBoxW(nullptr, wide_from_utf8(message).c_str(), wide_from_utf8(title).c_str(),
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

std::optional<std::filesystem::path> open_scene_dialog(GLFWwindow* window) {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window != nullptr ? glfwGetWin32Window(window) : nullptr;
    // Each filter is "label\0pattern\0"; the list ends with an extra \0.
    static const wchar_t kFilter[] =
        L"Gaussian splat scenes (*.ply, *.spz, *.pspk)\0*.ply;*.spz;*.pspk\0All files (*.*)\0*.*\0";
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = L"Open a splat scene";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return std::filesystem::path(file);
}

std::filesystem::path executable_dir() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return std::filesystem::current_path();
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path import_cache_dir() {
    std::filesystem::path base;
    PWSTR local = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local) == S_OK && local != nullptr) {
        base = std::filesystem::path(local);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    if (local != nullptr) CoTaskMemFree(local);
    std::filesystem::path dir = base / "PocketSplat" / "imports";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

#else  // !_WIN32

std::vector<std::filesystem::path> command_line(int argc, char** argv) {
    std::vector<std::filesystem::path> out;
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

void show_error(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
}

std::optional<std::filesystem::path> open_scene_dialog(GLFWwindow*) {
    return std::nullopt;  // drag and drop or the command line instead
}

std::filesystem::path executable_dir() {
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : self.parent_path();
}

std::filesystem::path import_cache_dir() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "pocketsplat-imports";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

#endif

}  // namespace pocketsplat::platform
