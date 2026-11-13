#include "catalyst/bridge.hpp"
#include "catalyst/mcp.hpp"
#include <bcrypt.h>
#include <fstream>
#include <mutex>
namespace catalyst {
std::filesystem::path module_directory;
void log(const std::string &message) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  OutputDebugStringA(("[Catalyst MCP] " + message + "\n").c_str());
  if (!module_directory.empty()) {
    std::ofstream out(module_directory / "catalyst-mcp.log", std::ios::app);
    out << message << '\n';
  }
}
void start_runtime(HMODULE module) {
  wchar_t disabled[8]{};
  if (GetEnvironmentVariableW(L"CATALYST_MCP_DISABLE", disabled, 8) && std::wstring(disabled) == L"1")
    return;
  // Keep the module mapped while hooks and HTTP workers can call its code.
  HMODULE pinned{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(&start_runtime), &pinned);
  wchar_t path[32768]{};
  GetModuleFileNameW(module, path, 32768);
  module_directory = std::filesystem::path(path).parent_path();
  Json config;
  const auto file = module_directory / "catalyst-mcp.json";
  if (std::filesystem::exists(file)) {
    std::ifstream in(file);
    in >> config;
  } else {
    unsigned char random[32]{};
    if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
      throw ToolError("Could not generate the local API token");
    std::string token;
    const char *hex = "0123456789abcdef";
    for (auto b : random) {
      token += hex[b >> 4];
      token += hex[b & 15];
    }
    config = {{"port", 13338}, {"token", token}};
    std::ofstream out(file);
    out << config.dump(2) << '\n';
    if (!out)
      throw ToolError("Could not write catalyst-mcp.json");
  }
  auto token = config.at("token").get<std::string>();
  const int port = config.value("port", 13338);
  if (token.size() < 32 || port < 1024 || port > 65535)
    throw ToolError("Invalid MCP configuration");
  if (!install_bridge()) {
    // Without the native bindings the license flag cannot be read, so acceptance cannot be confirmed.
    log("Native signature/layout checks failed; the MCP server will not start. See "
        "catalyst-mcp-patterns.json");
    return;
  }
  log("Pattern-resolved native bridge installed");
  // Serve nothing until the user has accepted Catalyst's license agreement in the editor.
  bool announced = false;
  for (auto state = license_state(); state != LicenseState::Accepted; state = license_state()) {
    if (state == LicenseState::Pending && !announced) {
      log("Waiting for the Catalyst license agreement to be accepted; the MCP server is not running");
      announced = true;
    }
    Sleep(200);
  }
  McpServer server(invoke_native, token, port);
  log("Listening on http://127.0.0.1:" + std::to_string(port) + "/mcp");
  if (!server.listen())
    log("HTTP listener failed (port may already be in use)");
}
} // namespace catalyst
