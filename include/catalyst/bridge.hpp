#pragma once
#include "common.hpp"
#include <Windows.h>
#include <filesystem>
namespace catalyst {
void start_runtime(HMODULE module);
void log(const std::string &message);
bool install_bridge();
// Catalyst's license agreement as the running editor reports it. Unknown until the editor's first update.
enum class LicenseState { Unknown, Pending, Accepted };
LicenseState license_state();
Json invoke_native(const std::string &operation, const Json &args);
Json capture_window(HWND window, const Json &args);
extern std::filesystem::path module_directory;
} // namespace catalyst
