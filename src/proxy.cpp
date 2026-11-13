#include "catalyst/bridge.hpp"
#include <mutex>
#include <winver.h>
namespace {
HMODULE system_version() {
  static HMODULE library = [] {
    wchar_t path[MAX_PATH]{};
    if (!GetSystemDirectoryW(path, MAX_PATH))
      return HMODULE{};
    return LoadLibraryExW((std::wstring(path) + L"\\version.dll").c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_SYSTEM32);
  }();
  return library;
}
template <class Fn> Fn resolve(const char *name) {
  auto library = system_version();
  return library ? reinterpret_cast<Fn>(GetProcAddress(library, name)) : nullptr;
}
DWORD WINAPI bootstrap(void *module) {
  try {
    catalyst::start_runtime(static_cast<HMODULE>(module));
  } catch (const std::exception &e) {
    catalyst::log(e.what());
  } catch (...) {
    OutputDebugStringA("Catalyst MCP bootstrap failed\n");
  }
  return 0;
}
} // namespace
#define PROXY(ret, name, params, args, fallback)                                                             \
  extern "C" ret WINAPI name params {                                                                        \
    static auto fn = resolve<ret(WINAPI *) params>(#name);                                                   \
    if (fn)                                                                                                  \
      return fn args;                                                                                        \
    SetLastError(ERROR_PROC_NOT_FOUND);                                                                      \
    return fallback;                                                                                         \
  }
PROXY(BOOL, GetFileVersionInfoA, (LPCSTR p, DWORD h, DWORD l, LPVOID d), (p, h, l, d), FALSE)
PROXY(BOOL, GetFileVersionInfoW, (LPCWSTR p, DWORD h, DWORD l, LPVOID d), (p, h, l, d), FALSE)
PROXY(BOOL, GetFileVersionInfoExA, (DWORD f, LPCSTR p, DWORD h, DWORD l, LPVOID d), (f, p, h, l, d), FALSE)
PROXY(BOOL, GetFileVersionInfoExW, (DWORD f, LPCWSTR p, DWORD h, DWORD l, LPVOID d), (f, p, h, l, d), FALSE)
PROXY(DWORD, GetFileVersionInfoSizeA, (LPCSTR p, LPDWORD h), (p, h), 0)
PROXY(DWORD, GetFileVersionInfoSizeW, (LPCWSTR p, LPDWORD h), (p, h), 0)
PROXY(DWORD, GetFileVersionInfoSizeExA, (DWORD f, LPCSTR p, LPDWORD h), (f, p, h), 0)
PROXY(DWORD, GetFileVersionInfoSizeExW, (DWORD f, LPCWSTR p, LPDWORD h), (f, p, h), 0)
PROXY(BOOL, VerQueryValueA, (LPCVOID b, LPCSTR s, LPVOID *v, PUINT l), (b, s, v, l), FALSE)
PROXY(BOOL, VerQueryValueW, (LPCVOID b, LPCWSTR s, LPVOID *v, PUINT l), (b, s, v, l), FALSE)
PROXY(DWORD, VerLanguageNameA, (DWORD l, LPSTR b, DWORD n), (l, b, n), 0)
PROXY(DWORD, VerLanguageNameW, (DWORD l, LPWSTR b, DWORD n), (l, b, n), 0)
PROXY(DWORD, VerFindFileA, (DWORD f, LPCSTR n, LPCSTR w, LPCSTR a, LPSTR c, PUINT cl, LPSTR d, PUINT dl),
      (f, n, w, a, c, cl, d, dl), VFF_BUFFTOOSMALL)
PROXY(DWORD, VerFindFileW, (DWORD f, LPCWSTR n, LPCWSTR w, LPCWSTR a, LPWSTR c, PUINT cl, LPWSTR d, PUINT dl),
      (f, n, w, a, c, cl, d, dl), VFF_BUFFTOOSMALL)
PROXY(DWORD, VerInstallFileA,
      (DWORD f, LPCSTR s, LPCSTR d, LPCSTR sd, LPCSTR dd, LPCSTR cd, LPSTR t, PUINT tl),
      (f, s, d, sd, dd, cd, t, tl), VIF_CANNOTREADSRC)
PROXY(DWORD, VerInstallFileW,
      (DWORD f, LPCWSTR s, LPCWSTR d, LPCWSTR sd, LPCWSTR dd, LPCWSTR cd, LPWSTR t, PUINT tl),
      (f, s, d, sd, dd, cd, t, tl), VIF_CANNOTREADSRC)
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    auto thread = CreateThread(nullptr, 0, bootstrap, module, 0, nullptr);
    if (thread)
      CloseHandle(thread);
  }
  return TRUE;
}
