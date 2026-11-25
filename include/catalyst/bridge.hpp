#pragma once
#include "common.hpp"
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <vector>
namespace catalyst {
void start_runtime(HMODULE module);
void log(const std::string &message);
bool install_bridge();
// Catalyst's license agreement as the running editor reports it. Unknown until the editor's first update.
enum class LicenseState { Unknown, Pending, Accepted };
LicenseState license_state();
Json invoke_native(const std::string &operation, const Json &args);
struct Frame {
  int width{}, height{};
  std::vector<uint8_t> bgra;
};
struct Rect {
  int x{}, y{}, width{}, height{};
};
Frame capture_window(HWND window);
// Finds the 3D preview in a captured Catalyst window.
bool detect_viewport(const Frame &frame, Rect &area);
Frame crop(const Frame &frame, const Rect &area);
Frame compose_row(const std::vector<Frame> &frames, int gap);
// Scales the frame down to at most max_width pixels wide before encoding.
std::string encode_png(const Frame &frame, int max_width, int &out_width, int &out_height);
extern std::filesystem::path module_directory;
} // namespace catalyst
