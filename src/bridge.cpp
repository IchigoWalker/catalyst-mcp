#include "catalyst/bridge.hpp"
#include "catalyst/native_bindings.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
namespace catalyst {
namespace {
constexpr char reference_hash[] = "76b89d30a8874ca3c82543a9c6f0bf347ab56e1aa34606cf910bce4359afa1f7";
NativeBindings bindings;
std::atomic<void *> app{};
std::atomic<DWORD> editor_thread{};
std::atomic<unsigned> ui_notices{};
// Viewport modes in the order of Catalyst's own numbering; any other stored string means the editor view.
constexpr std::array<std::string_view, 4> viewport_modes = {"preview", "editor", "wireframe", "simulation"};
std::atomic<unsigned> viewport_mode{1};
bool installed{};
std::string actual_hash;
using Tick = void (*)(void *, unsigned char);
Tick original_tick{};
using Present = uintptr_t (*)();
using Destroy = void (*)(void *);
Present original_present{};
Destroy original_destroy{};
std::atomic<uint64_t> presented_frames{};
std::mutex frame_mutex;
std::condition_variable frame_changed;
uintptr_t present_hook() {
  auto result = original_present();
  ++presented_frames;
  frame_changed.notify_all();
  return result;
}
using FileDialog = void *(*)(void *, HWND, char, const wchar_t *, const wchar_t *);
using ShowError = void (*)(void *, const void *, const void *);
FileDialog original_dialog{};
ShowError original_error{};
thread_local const std::string *selected_file = nullptr;
thread_local std::string *intercepted_error = nullptr;
void set_string(void *target, const std::string &text);
std::string read_string(void *value);
void *dialog_hook(void *result, HWND owner, char save, const wchar_t *filter, const wchar_t *extension) {
  if (!selected_file)
    return original_dialog(result, owner, save, filter, extension);
  std::memset(result, 0, 32);
  *reinterpret_cast<size_t *>(static_cast<char *>(result) + 24) = 15;
  set_string(result, *selected_file);
  return result;
}
void error_hook(void *self, const void *title, const void *message) {
  if (intercepted_error) {
    *intercepted_error = read_string(const_cast<void *>(message));
    return;
  }
  original_error(self, title, message);
}
template <class T> T function(NativeSymbol symbol) {
  return reinterpret_cast<T>(bindings.address(symbol));
}
template <class T> T &field(void *object, size_t offset) {
  return *reinterpret_cast<T *>(static_cast<char *>(object) + offset);
}
void *project() {
  auto a = app.load();
  if (!a)
    throw ToolError("Catalyst editor is not ready");
  return field<void *>(a, 4024);
}
std::string sha256(const std::filesystem::path &path) {
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    return {};
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  char buffer[65536];
  bool ok = bool(input);
  while (input) {
    input.read(buffer, sizeof buffer);
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(input.gcount()), 0) < 0)
      ok = false;
  }
  unsigned char digest[32]{};
  if (BCryptFinishHash(hash, digest, sizeof digest, 0) < 0)
    ok = false;
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  std::string result;
  const char *hex = "0123456789abcdef";
  for (auto b : digest) {
    result += hex[b >> 4];
    result += hex[b & 15];
  }
  return ok ? result : "";
}
struct Job {
  std::function<Json()> action;
  std::promise<Json> promise;
  std::atomic<int> state{0};
};
std::mutex queue_mutex;
std::deque<std::shared_ptr<Job>> queue;
thread_local bool draining = false;
void destroy_hook(void *self) {
  app = nullptr;
  editor_thread = 0;
  {
    std::lock_guard lock(queue_mutex);
    for (auto &job : queue) {
      int expected = 0;
      if (job->state.compare_exchange_strong(expected, 3))
        job->promise.set_exception(std::make_exception_ptr(ToolError("Catalyst is closing")));
    }
    queue.clear();
  }
  original_destroy(self);
}
void tick_hook(void *self, unsigned char force) {
  // Publish the dialog flags before the App pointer: license_state() trusts them once the pointer is set.
  ui_notices = (field<uint8_t>(self, 1472) ? 1u : 0u) | (field<uint8_t>(self, 1475) ? 2u : 0u) |
               (field<uint8_t>(self, 1473) ? 4u : 0u) | (field<uint8_t>(self, 1474) ? 8u : 0u) |
               (field<uint8_t>(self, 1412) ? 16u : 0u);
  {
    // Read the mode string without throwing: this runs inside Catalyst's own update.
    auto text = static_cast<char *>(self) + 1048;
    const auto size = field<size_t>(text, 16), capacity = field<size_t>(text, 24);
    unsigned index = 1;
    if (size <= capacity && size < 16) {
      const std::string_view value(capacity < 16 ? text : field<char *>(text, 0), size);
      const auto mode = std::find(viewport_modes.begin(), viewport_modes.end(), value);
      if (mode != viewport_modes.end())
        index = static_cast<unsigned>(mode - viewport_modes.begin());
    }
    viewport_mode = index;
  }
  editor_thread = GetCurrentThreadId();
  app = self;
  if (!draining) {
    draining = true;
    std::shared_ptr<Job> job;
    {
      std::lock_guard lock(queue_mutex);
      if (!queue.empty()) {
        job = queue.front();
        queue.pop_front();
      }
    }
    if (job) {
      int expected = 0;
      if (job->state.compare_exchange_strong(expected, 1)) {
        try {
          job->promise.set_value(job->action());
        } catch (...) {
          job->promise.set_exception(std::current_exception());
        }
        job->state = 2;
      }
    }
    draining = false;
  }
  original_tick(self, force);
}
std::string serialize() {
  if (!field<void *>(project(), 536) || !field<void *>(project(), 552))
    throw ToolError("No project is open. Create or open a project first.");
  std::ostringstream output(std::ios::binary);
  function<void (*)(void *, std::ostream *, bool)>(NativeSymbol::SerializeProject)(project(), &output, false);
  if (!output)
    throw ToolError("Catalyst serialization failed");
  return output.str();
}
std::filesystem::path path_from_utf8(const std::string &text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::string path_utf8(const std::filesystem::path &path) {
  auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}
std::filesystem::path absolute_path(const Json &args, const char *key, const char *extension = nullptr) {
  auto p = path_from_utf8(args.at(key).get<std::string>());
  if (!p.is_absolute())
    throw ToolError(std::string(key) + " must be an absolute path");
  auto suffix = p.extension().string();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension && suffix != extension)
    throw ToolError(std::string(key) + " must have extension " + extension);
  return p;
}
std::string read_string(void *value) {
  auto size = field<size_t>(value, 16);
  auto capacity = field<size_t>(value, 24);
  if (size > 32 * 1024 * 1024 || size > capacity)
    throw ToolError("Invalid native string");
  return std::string(capacity < 16 ? static_cast<char *>(value) : field<char *>(value, 0), size);
}
Json node_types() {
  function<void *(*)()>(NativeSymbol::RegistryInstance)();
  Json types = Json::array();
  auto head = *reinterpret_cast<void **>(bindings.address(NativeSymbol::RegistryHead));
  if (!head)
    throw ToolError("Node registry is not initialized");
  auto node = field<void *>(head, 0);
  for (size_t count = 0; node != head && count < 4096; node = field<void *>(node, 0), ++count)
    types.push_back(read_string(static_cast<char *>(node) + 16));
  std::sort(types.begin(), types.end());
  return types;
}
// Catalyst uses RapidJSON's 64-bit, 48-bit-pointer GenericValue layout.
Json read_value(void *value, unsigned depth = 0) {
  if (depth > 64)
    throw ToolError("Native JSON nesting limit exceeded");
  const auto tag = field<uint16_t>(value, 14);
  const auto raw = field<uint64_t>(value, 0);
  auto pointer = reinterpret_cast<char *>(field<uint64_t>(value, 8) & 0x0000ffffffffffffull);
  const auto size = field<uint32_t>(value, 0);
  if (size > 1000000 && ((tag & 7) == 3 || (tag & 7) == 4 || ((tag & 7) == 5 && !(tag & 0x1000))))
    throw ToolError("Native JSON value is too large");
  switch (tag & 7) {
  case 0:
    return nullptr;
  case 1:
    return false;
  case 2:
    return true;
  case 3: {
    Json out = Json::object();
    for (uint32_t i = 0; i < size; ++i) {
      auto key = read_value(pointer + 32ull * i, depth + 1);
      out[key.get<std::string>()] = read_value(pointer + 32ull * i + 16, depth + 1);
    }
    return out;
  }
  case 4: {
    Json out = Json::array();
    for (uint32_t i = 0; i < size; ++i)
      out.push_back(read_value(pointer + 16ull * i, depth + 1));
    return out;
  }
  case 5:
    if (tag & 0x1000)
      return std::string(static_cast<char *>(value), 13 - field<uint8_t>(value, 13));
    else
      return std::string(pointer, size);
  case 6:
    if (tag & 0x200)
      return field<double>(value, 0);
    else if (tag & 0x80)
      return raw;
    else
      return static_cast<int64_t>(raw);
  default:
    throw ToolError("Invalid native JSON tag");
  }
}
Json node_ports(void *node, size_t offset) {
  Json result = Json::array();
  auto begin = field<void **>(node, offset), end = field<void **>(node, offset + 8);
  if (end - begin > 2048)
    throw ToolError("Invalid port table");
  for (auto it = begin; it != end; ++it) {
    Json p = {{"name", read_string(static_cast<char *>(*it) + 16)},
              {"data_type", field<int>(*it, 80)},
              {"reference_type", field<uint32_t>(*it, 84)}};
    if (offset == 96)
      p["multiple"] = !field<uint8_t>(*it, 192);
    result.push_back(p);
  }
  return result;
}
Json node_template(const std::string &type) {
  function<void *(*)()>(NativeSymbol::RegistryInstance)();
  auto head = *reinterpret_cast<void **>(bindings.address(NativeSymbol::RegistryHead));
  if (!head)
    throw ToolError("Node registry is not initialized");
  for (auto entry = field<void *>(head, 0); entry != head; entry = field<void *>(entry, 0)) {
    if (read_string(static_cast<char *>(entry) + 16) != type)
      continue;
    auto factory = field<void *>(entry, 200);
    if (!factory)
      throw ToolError("Factory is not callable");
    auto factory_vtable = field<uintptr_t *>(factory, 0);
    void *node{};
    reinterpret_cast<void (*)(void *, void **)>(factory_vtable[2])(factory, &node);
    if (!node)
      throw ToolError("Factory returned no node");
    auto vtable = field<uintptr_t *>(node, 0);
    alignas(16) std::array<char, 96> text{};
    try {
      const auto serializer = type == "ParticleBiasLink"    ? NativeSymbol::SerializeBiasLink
                              : type == "ParticleEvolution" ? NativeSymbol::SerializeEvolution
                                                            : NativeSymbol::SerializeNode;
      if (vtable[15] != bindings.address(serializer))
        throw ToolError("Node serialization vtable layout is incompatible with the resolved profile");
      reinterpret_cast<void (*)(void *, void *)>(vtable[15])(node, text.data());
      Json result = {{"node", read_value(text.data())},
                     {"inputs", node_ports(node, 96)},
                     {"outputs", node_ports(node, 120)}};
      function<void (*)(void *)>(NativeSymbol::JsonDestroy)(text.data());
      reinterpret_cast<void (*)(void *, unsigned)>(vtable[1])(node, 1);
      return result;
    } catch (...) {
      function<void (*)(void *)>(NativeSymbol::JsonDestroy)(text.data());
      reinterpret_cast<void (*)(void *, unsigned)>(vtable[1])(node, 1);
      throw;
    }
  }
  throw ToolError("Unknown node type: " + type);
}
void set_string(void *target, const std::string &text) {
  if (text.size() > 1024)
    throw ToolError("Native string is too long");
  function<void (*)(void *)>(NativeSymbol::StringDestroy)(target);
  std::memset(target, 0, 32);
  field<size_t>(target, 16) = text.size();
  if (text.size() < 16) {
    field<size_t>(target, 24) = 15;
    std::memcpy(target, text.data(), text.size());
  } else {
    auto memory = function<void *(*)(size_t)>(NativeSymbol::OperatorNew)(text.size() + 1);
    field<void *>(target, 0) = memory;
    field<size_t>(target, 24) = text.size();
    std::memcpy(memory, text.c_str(), text.size() + 1);
  }
}
void invalidate_views(const Json &doc) {
  static uint64_t generation = 0x100000000ull;
  for (const auto &view : doc.at("views")) {
    auto native = function<void *(*)(void *, uint64_t)>(NativeSymbol::FindView)(project(), view.at("id"));
    if (native)
      field<uint64_t>(native, 384) = ++generation;
  }
  auto self = app.load();
  field<uint64_t>(self, 4520) = ~uint64_t{};
  field<void *>(self, 4536) = nullptr;
}
void apply_document(ProjectFile &file) {
  auto self = app.load();
  auto original_path = read_string(static_cast<char *>(self) + 920);
  auto directory = module_directory / "mcp-cache";
  std::filesystem::create_directories(directory);
  const auto path = directory / "working.ctlst";
  const auto backup = serialize();
  {
    std::ofstream out(directory / "recovery.ctlst", std::ios::binary | std::ios::trunc);
    out.write(backup.data(), backup.size());
    if (!out)
      throw ToolError("Could not save recovery project");
  }
  const auto bytes = file.encode();
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    if (!out)
      throw ToolError("Could not write working project");
  }
  std::string filename = path_utf8(path);
  function<void (*)(void *, const std::string *)>(NativeSymbol::OpenProject)(self, &filename);
  invalidate_views(file.document);
  set_string(static_cast<char *>(self) + 920, original_path);
  function<void (*)(void *, unsigned char)>(NativeSymbol::RefreshApp)(self, 1);
}
Json execute(const std::string &operation, const Json &args) {
  auto self = app.load();
  if (operation == "catalyst_list_node_types")
    return {{"types", node_types()}};
  if (operation == "catalyst_get_node_template")
    return node_template(args.at("type"));
  if (operation == "catalyst_create_particle" || operation == "catalyst_place_nodes" ||
      operation == "catalyst_connect_nodes" || operation == "catalyst_disconnect_nodes" ||
      operation == "catalyst_set_node_properties" || operation == "catalyst_move_nodes" ||
      operation == "catalyst_delete_nodes" || operation == "catalyst_set_curve" ||
      operation == "catalyst_set_sprite_animation" || operation == "catalyst_edit_graph" ||
      operation == "catalyst_set_evolution_properties") {
    auto file = ProjectFile::decode(serialize());
    auto result = mutate_graph(file.document, operation, args, [](const std::string &, const Json &a) {
      return node_template(a.at("type"));
    });
    apply_document(file);
    if (operation == "catalyst_set_evolution_properties") {
      // Catalyst names the node's new curves while loading, so report the node as it is afterwards.
      const auto document = ProjectFile::decode(serialize()).document;
      for (const auto &view : document.at("views"))
        if (view.at("id") == args.at("view_id"))
          for (const auto &node : view.at("nodes"))
            if (node.at("i") == args.at("node_id"))
              result["node"] = node;
    }
    return result;
  }
  if (operation == "catalyst_get_project")
    return ProjectFile::decode(serialize()).document;
  if (operation == "catalyst_new_project") {
    function<void (*)(void *)>(NativeSymbol::NewProject)(self);
    return {{"created", true}};
  }
  if (operation == "catalyst_create_collection") {
    serialize();
    std::string name = args.value("name", std::string("new_collection.ypt"));
    if (name.empty() || name.size() > 128 || name.find_first_of("/\\:\r\n\t") != std::string::npos)
      throw ToolError("Invalid collection name");
    auto id =
        function<uint64_t (*)(void *, const std::string *)>(NativeSymbol::CreateCollection)(project(), &name);
    field<uint64_t>(self, 400) = id;
    return {{"collection_id", id}};
  }
  if (operation == "catalyst_save_project") {
    auto path = absolute_path(args, "path", ".ctlst");
    if (!std::filesystem::is_directory(path.parent_path()))
      throw ToolError("Parent directory does not exist");
    auto bytes = serialize();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    out.close();
    if (!out)
      throw ToolError("Could not save project");
    set_string(static_cast<char *>(self) + 920, path_utf8(path));
    // Use the native save command to update dirty-state counters and the window title.
    function<void (*)(void *)>(NativeSymbol::SaveProject)(self);
    return {{"path", path_utf8(path)}, {"bytes", bytes.size()}};
  }
  if (operation == "catalyst_open_project") {
    auto path = absolute_path(args, "path", ".ctlst");
    if (!std::filesystem::is_regular_file(path))
      throw ToolError("Project file does not exist");
    std::string filename = path_utf8(path);
    function<void (*)(void *, const std::string *)>(NativeSymbol::OpenProject)(self, &filename);
    invalidate_views(ProjectFile::decode(serialize()).document);
    return {{"path", path_utf8(path)}};
  }
  if (operation == "catalyst_add_texture" || operation == "catalyst_add_drawable") {
    auto before = ProjectFile::decode(serialize()).document;
    auto collection = args.at("collection_id").get<uint64_t>();
    bool found = false;
    for (const auto &c : before["collections"])
      if (c["id"] == collection)
        found = true;
    if (!found)
      throw ToolError("Collection was not found");
    bool texture = operation == "catalyst_add_texture";
    auto path = absolute_path(args, "path", texture ? ".dds" : ".ydr");
    if (!std::filesystem::is_regular_file(path))
      throw ToolError("Asset file does not exist");
    auto filename = path_utf8(path);
    std::string error;
    auto original_collection = field<uint64_t>(self, 400);
    field<uint64_t>(self, 400) = collection;
    selected_file = &filename;
    intercepted_error = &error;
    uintptr_t listener[2] = {0, reinterpret_cast<uintptr_t>(self)};
    try {
      function<void (*)(void *)>(texture ? NativeSymbol::ImportTexture
                                         : NativeSymbol::ImportDrawable)(listener);
    } catch (...) {
      selected_file = nullptr;
      intercepted_error = nullptr;
      field<uint64_t>(self, 400) = original_collection;
      throw;
    }
    selected_file = nullptr;
    intercepted_error = nullptr;
    field<uint64_t>(self, 400) = original_collection;
    if (!error.empty())
      throw ToolError(error);
    return {{"imported", path_utf8(path)}, {"collection_id", collection}};
  }
  if (operation == "catalyst_import_ypt") {
    serialize();
    auto path = absolute_path(args, "path", ".ypt");
    if (!std::filesystem::is_regular_file(path))
      throw ToolError("YPT file does not exist");
    std::string filename = path_utf8(path);
    if (!function<bool (*)(void *, const std::string *)>(NativeSymbol::ImportYpt)(project(), &filename))
      throw ToolError("Catalyst could not import the YPT");
    auto file = ProjectFile::decode(serialize());
    apply_document(file);
    return {{"imported", path_utf8(path)}, {"collections", file.document["collections"]}};
  }
  if (operation == "catalyst_export_ypt") {
    auto doc = ProjectFile::decode(serialize()).document;
    auto directory = absolute_path(args, "directory");
    if (!std::filesystem::is_directory(directory))
      throw ToolError("Export directory does not exist");
    bool legacy = args.value("legacy", true), enhanced = args.value("enhanced", false);
    if (!legacy && !enhanced)
      throw ToolError("Choose Legacy, Enhanced or both");
    Json files = Json::array();
    for (const auto &c : doc["collections"])
      if (c.value("forExport", true)) {
        auto collection = c.at("id").get<uint64_t>();
        auto stem = path_utf8(path_from_utf8(c.at("name").get<std::string>()).stem());
        if (stem.empty() || stem.find_first_of("/\\:") != std::string::npos)
          throw ToolError("Invalid collection export name");
        for (int target = 0; target < 2; ++target)
          if (target ? enhanced : legacy) {
            auto path = directory / (stem + (target ? "_enhanced.ypt" : "_legacy.ypt"));
            std::string filename = path_utf8(path);
            if (!function<bool (*)(void *, const std::string *, unsigned, uint64_t)>(NativeSymbol::ExportYpt)(
                    project(), &filename, target, collection))
              throw ToolError("Catalyst export failed: " + filename);
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) < 16)
              throw ToolError("Export output is missing or empty");
            files.push_back(path_utf8(path));
          }
      }
    if (files.empty())
      throw ToolError("No collections are checked for export");
    return {{"files", files}};
  }
  if (operation == "catalyst_set_viewport_mode") {
    const auto mode = args.at("mode").get<std::string>();
    const auto known = std::find(viewport_modes.begin(), viewport_modes.end(), mode);
    if (known == viewport_modes.end())
      throw ToolError("mode must be preview, editor, wireframe or simulation");
    // The same three steps as the editor's own dropdown: store the mode, refresh its UI binding, apply it.
    set_string(static_cast<char *>(self) + 1048, mode);
    const std::string variable = "previewViewMode";
    function<void (*)(void *, const std::string *)>(NativeSymbol::DirtyVariable)(
        static_cast<char *>(self) + 4016, &variable);
    auto element = function<void *(*)(void *)>(NativeSymbol::PreviewElement)(self);
    if (element)
      function<void (*)(void *, int)>(NativeSymbol::SetPreviewViewMode)(
          element, static_cast<int>(known - viewport_modes.begin()));
    viewport_mode = static_cast<unsigned>(known - viewport_modes.begin());
    return {{"mode", mode}, {"applied", element != nullptr}};
  }
  if (operation == "catalyst_render_particle") {
    if (ui_notices.load())
      throw ToolError("A Catalyst startup or error dialog covers the preview. Finish or dismiss it in the "
                      "editor before capturing an image.");
    auto manager = project();
    auto id = args.at("view_id").get<uint64_t>();
    auto v = function<void *(*)(void *, uint64_t)>(NativeSymbol::FindView)(manager, id);
    if (!v)
      throw ToolError("View was not found");
    auto doc = ProjectFile::decode(serialize()).document;
    std::string name;
    for (const auto &view : doc["views"])
      if (view["id"] == id)
        name = view["name"];
    set_string(static_cast<char *>(self) + 2032, name);
    set_string(static_cast<char *>(self) + 3056, name);
    set_string(static_cast<char *>(self) + 2064, "effect");
    if (field<uint8_t>(v, 0)) {
      function<void (*)(void *, void *, void *)>(NativeSymbol::BuildTimeline)(
          static_cast<char *>(v) + 120, static_cast<char *>(v) + 72, static_cast<char *>(v) + 96);
      field<uint8_t>(v, 0) = 0;
    }
    auto duration = args.value("duration", 1.0);
    if (!std::isfinite(duration) || duration <= 0 || duration > 300)
      throw ToolError("Duration must be within (0,300]");
    // Keep the App model and view in the same mode. Refresh copies App::timelineSimMode
    // back into the view; setting only the view silently clamps seconds to normalized time.
    function<void (*)(void *, unsigned char)>(NativeSymbol::SetTimelineMode)(self, args.contains("duration"));
    field<uint8_t>(v, 468) = 0;
    // Timeline position remains normalized to the effect's duration even in simulation
    // mode. +488 is the computed maximum ratio, +492 the complete simulation duration,
    // and +496 the effect duration. Do not overwrite +488 with seconds.
    auto effect_duration = std::max(field<float>(v, 496), 0.01f);
    auto position = args.at("position").get<double>();
    auto normalized = args.contains("duration") ? position * duration / effect_duration : position;
    field<float>(v, 464) = static_cast<float>(normalized);
    function<void (*)(void *, unsigned char)>(NativeSymbol::RefreshApp)(self, 1);
    return {{"view_id", id},
            {"position", args.at("position")},
            {"seconds", normalized * effect_duration},
            {"effect_duration", effect_duration},
            {"_capture_frame", presented_frames.load() + 2},
            {"_capture_hwnd", reinterpret_cast<uintptr_t>(function<HWND (*)()>(NativeSymbol::GetWindow)())}};
  }
  if (operation == "catalyst_import_clip_regions" || operation == "catalyst_export_clip_regions" ||
      operation == "catalyst_create_clip_regions") {
    auto file = ProjectFile::decode(serialize());
    if (operation == "catalyst_export_clip_regions") {
      auto path = absolute_path(args, "path", ".dat");
      std::ofstream out(path, std::ios::binary);
      out << file.document.at("clipRegions").get<std::string>();
      if (!out)
        throw ToolError("Could not write clip regions");
      return {{"path", path_utf8(path)}};
    }
    if (operation == "catalyst_import_clip_regions") {
      auto path = absolute_path(args, "path", ".dat");
      std::ifstream input(path, std::ios::binary);
      if (!input)
        throw ToolError("Could not open clip regions");
      std::ostringstream text;
      text << input.rdbuf();
      file.document["clipRegions"] = text.str();
    } else {
      std::string texture = args.at("texture");
      bool owned = false;
      for (const auto &owner : file.document.at("textureOwners"))
        if (owner.at("h") == texture)
          owned = true;
      if (!owned)
        throw ToolError("Import or copy the texture into this project before assigning clip regions");
      auto columns = args.at("columns").get<int>(), rows = args.at("rows").get<int>();
      if (texture.empty() || texture.find_first_of("\r\n\t ") != std::string::npos || columns < 1 ||
          rows < 1 || columns > 256 || rows > 256 || columns * rows > 4096)
        throw ToolError("Invalid texture name or sprite grid");
      std::istringstream current(file.document.at("clipRegions").get<std::string>());
      int count = 0, total = 0;
      current >> count >> total;
      if (!current || count < 0 || total < 0)
        throw ToolError("Invalid existing clip regions");
      struct Grid {
        std::string name;
        int cols, rows;
        std::vector<std::array<float, 4>> uv;
      };
      std::vector<Grid> grids;
      for (int i = 0; i < count; ++i) {
        Grid g;
        current >> g.name >> g.cols >> g.rows;
        if (!current || g.cols < 1 || g.rows < 1 || g.cols * g.rows > 4096)
          throw ToolError("Invalid existing grid");
        g.uv.resize(g.cols * g.rows);
        for (auto &uv : g.uv)
          for (auto &f : uv)
            current >> f;
        if (!current)
          throw ToolError("Truncated existing grid");
        if (g.name != texture)
          grids.push_back(g);
      }
      // Native ApplyClipGrid initializes every frame to (uMin,uMax,vMin,vMax)=(0,1,0,1).
      // Tile selection is encoded by the separate columns/rows fields, not by these crop bounds.
      Grid g{texture, columns, rows, {}};
      g.uv.resize(static_cast<size_t>(columns) * rows, {0.0f, 1.0f, 0.0f, 1.0f});
      if (args.contains("frames")) {
        std::set<size_t> seen;
        for (const auto &frame : args.at("frames")) {
          auto index = frame.at("index").get<size_t>();
          if (index >= g.uv.size() || !seen.insert(index).second)
            throw ToolError("Invalid or duplicate clip frame index");
          std::array<float, 4> uv{frame.at("u_min"), frame.at("u_max"), frame.at("v_min"), frame.at("v_max")};
          for (auto f : uv)
            if (!std::isfinite(f) || f < 0.0f || f > 1.0f)
              throw ToolError("Clip bounds must be finite and within [0,1]");
          if (uv[0] >= uv[1] || uv[2] >= uv[3])
            throw ToolError("Clip minima must be below maxima");
          g.uv[index] = uv;
        }
      }
      grids.push_back(g);
      size_t frames = 0;
      for (const auto &grid : grids)
        frames += grid.uv.size();
      std::ostringstream text;
      text << grids.size() << '\n' << frames << '\n';
      for (const auto &grid : grids) {
        text << grid.name << '\n' << grid.cols << ' ' << grid.rows << '\n';
        for (const auto &uv : grid.uv)
          text << uv[0] << ' ' << uv[1] << ' ' << uv[2] << ' ' << uv[3] << '\n';
      }
      file.document["clipRegions"] = text.str();
    }
    // Validate with Catalyst's own parser before changing the live project.
    alignas(16) std::array<char, 80> regions{};
    int zero = 0;
    char temporary[16]{};
    field<uintptr_t>(regions.data(), 0) = bindings.address(NativeSymbol::ClipVtable);
    function<void (*)(void *, void *, void *)>(NativeSymbol::InitializeClipMap)(regions.data() + 8, &zero,
                                                                                temporary);
    auto text = file.document.at("clipRegions").get<std::string>();
    bool valid = function<bool (*)(void *, const std::string *)>(NativeSymbol::ParseClipRegions)(
        regions.data(), &text);
    function<void (*)(void *, unsigned)>(NativeSymbol::DestroyClipRegions)(regions.data(), 0);
    if (!valid)
      throw ToolError("Catalyst rejected the clip region data");
    apply_document(file);
    return {{"updated", true}};
  }
  throw ToolError("Native operation is not implemented: " + operation);
}
} // namespace
bool install_bridge() {
  wchar_t path[32768]{};
  GetModuleFileNameW(nullptr, path, 32768);
  actual_hash = sha256(path);
  const auto module = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
  const auto publish = [] {
    std::ofstream report(module_directory / "catalyst-mcp-patterns.json");
    if (report)
      report << bindings.report.dump(2) << '\n';
  };
  try {
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
        dos->e_lfanew > 1024 * 1024)
      throw ToolError("Invalid main executable header");
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage > 1024 * 1024 * 1024)
      throw ToolError("Invalid main executable size");
    PatternImage mapped({module, nt->OptionalHeader.SizeOfImage});
    bindings = resolve_native_bindings(mapped);
  } catch (const std::exception &error) {
    bindings.report = {{"mode", "patterns"}, {"compatible", false}, {"errors", Json::array({error.what()})}};
  }
  bindings.report["exe_sha256"] = actual_hash;
  bindings.report["reference_build"] = actual_hash == reference_hash;
  publish();
  if (!bindings.compatible) {
    log("Native pattern resolution rejected this build: " + bindings.report.at("errors").dump());
    return false;
  }
  log("Resolved " + std::to_string(native_signatures().size()) + " native signatures and " +
      std::to_string(native_layout_guards().size()) + " layout guards in " +
      std::to_string(bindings.report.at("resolution_ms").get<double>()) + " ms");
  if (MH_Initialize() != MH_OK) {
    bindings.report["hook_error"] = "MinHook initialization failed";
    publish();
    log("MinHook initialization failed");
    return false;
  }
  const auto hook = [](NativeSymbol symbol, auto callback, auto original) {
    return MH_CreateHook(reinterpret_cast<void *>(bindings.address(symbol)),
                         reinterpret_cast<void *>(callback), reinterpret_cast<void **>(original)) == MH_OK;
  };
  // Resolve and validate everything before patching code; enable the hooks together.
  if (!hook(NativeSymbol::Tick, &tick_hook, &original_tick) ||
      !hook(NativeSymbol::FileDialog, &dialog_hook, &original_dialog) ||
      !hook(NativeSymbol::ShowError, &error_hook, &original_error) ||
      !hook(NativeSymbol::Present, &present_hook, &original_present) ||
      !hook(NativeSymbol::Destroy, &destroy_hook, &original_destroy) ||
      MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    bindings.report["hook_error"] = "Could not install pattern-resolved native adapters; hooks rolled back";
    bindings.report["hooks_installed"] = false;
    publish();
    log(bindings.report["hook_error"].get<std::string>());
    return false;
  }
  installed = true;
  bindings.report["hooks_installed"] = true;
  publish();
  return true;
}
LicenseState license_state() {
  if (!app.load())
    return LicenseState::Unknown;
  return ui_notices.load() & 2 ? LicenseState::Pending : LicenseState::Accepted;
}
Json invoke_native(const std::string &operation, const Json &args) {
  // The listener starts only after acceptance; this also covers the dialog reappearing while it runs.
  if (ui_notices.load() & 2)
    throw ToolError("Accept the Catalyst license agreement in the editor before using the MCP tools");
  if (operation == "catalyst_status") {
    Json notices = Json::array();
    auto mask = ui_notices.load();
    if (mask & 1)
      notices.push_back("telemetry_choice");
    if (mask & 4)
      notices.push_back("locate_game");
    if (mask & 8)
      notices.push_back("update");
    if (mask & 16)
      notices.push_back("editor_error");
    return {{"bridge_installed", installed},
            {"editor_ready", app.load() != nullptr},
            {"preview_ready", app.load() != nullptr && notices.empty()},
            {"ui_notices", notices},
            {"viewport_mode", viewport_modes[viewport_mode.load()]},
            {"exe_sha256", actual_hash},
            {"reference_sha256", reference_hash},
            {"reference_build", actual_hash == reference_hash},
            {"binding_mode", "patterns"},
            {"pattern_resolution", bindings.report},
            {"editor_thread", editor_thread.load()}};
  }
  if (!installed)
    throw ToolError("Catalyst signatures or native layout did not pass validation. Inspect catalyst_status "
                    "or catalyst-mcp-patterns.json.");
  if (!app.load())
    throw ToolError("Catalyst editor is initializing or a startup dialog is open");
  auto job = std::make_shared<Job>();
  job->action = [operation, args] { return execute(operation, args); };
  auto future = job->promise.get_future();
  {
    std::lock_guard lock(queue_mutex);
    if (queue.size() >= 32)
      throw ToolError("Editor queue is full");
    queue.push_back(job);
  }
  PostThreadMessageW(editor_thread.load(), WM_NULL, 0, 0);
  if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
    int expected = 0;
    if (job->state.compare_exchange_strong(expected, 3))
      throw ToolError("Editor did not service the request; queued operation was cancelled");
    throw ToolError("Native operation is still executing; inspect editor state before retrying");
  }
  auto result = future.get();
  auto window = result.is_object() && result.contains("_capture_hwnd")
                    ? reinterpret_cast<HWND>(result.at("_capture_hwnd").get<uintptr_t>())
                    : nullptr;
  auto capture_frame = result.is_object() ? result.value("_capture_frame", uint64_t{}) : 0;
  if (result.is_object()) {
    result.erase("_capture_hwnd");
    result.erase("_capture_frame");
  }
  if (operation == "catalyst_render_particle" && args.value("image", true)) {
    // Capture on an HTTP worker so the editor remains free to present the sought frame.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (presented_frames.load() < capture_frame) {
      if (!app.load())
        throw ToolError("Catalyst closed before rendering completed");
      if (std::chrono::steady_clock::now() > deadline)
        throw ToolError("Catalyst did not present the requested preview");
      PostThreadMessageW(editor_thread.load(), WM_NULL, 0, 0);
      std::unique_lock lock(frame_mutex);
      frame_changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    auto captured = capture_window(window, Json::object());
    return {
        {"_mcp_content",
         Json::array({{{"type", "text"}, {"text", result.dump()}},
                      {{"type", "image"}, {"mimeType", "image/png"}, {"data", captured.at("png_base64")}}})}};
  }
  return result;
}
} // namespace catalyst
