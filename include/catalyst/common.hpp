#pragma once
#include <array>
#include <functional>
#include <json.hpp>
#include <stdexcept>
#include <string>
namespace catalyst {
using Json = nlohmann::json;
using Invoke = std::function<Json(const std::string &, const Json &)>;
struct ToolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// Graph tools that catalyst_edit_graph accepts as operations, named without their "catalyst_" prefix.
inline constexpr std::array<const char *, 9> graph_operations = {
    "place_nodes",  "connect_nodes", "disconnect_nodes",     "set_node_properties",     "move_nodes",
    "delete_nodes", "set_curve",     "set_sprite_animation", "set_evolution_properties"};
Json tool_catalog();
// Checks arguments against one tool_catalog() entry: unknown and missing keys, types and numeric bounds.
void validate_arguments(const Json &tool, const Json &args);
Json dispatch_tool(const std::string &name, const Json &args, const Invoke &native);
// The built-in authoring guide: topic names, and one topic as MCP text content.
Json documentation_topics();
Json documentation(const Json &args);
Json mutate_graph(Json &document, const std::string &operation, const Json &args, const Invoke &templates);
std::string base64(const std::string &bytes);
std::string unbase64(const std::string &text);
struct ProjectFile {
  std::string textures, drawables;
  Json document;
  static ProjectFile decode(const std::string &bytes);
  std::string encode() const;
};
} // namespace catalyst
