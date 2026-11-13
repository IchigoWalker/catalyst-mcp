#include "catalyst/common.hpp"
#include <cstring>
namespace catalyst {
ProjectFile ProjectFile::decode(const std::string &bytes) {
  if (bytes.size() < 22 || bytes.compare(0, 6, "CTLSTJ") != 0)
    throw ToolError("Expected a CTLSTJ project");
  size_t offset = 6;
  auto blob = [&]() {
    if (bytes.size() - offset < 8)
      throw ToolError("Truncated project length");
    uint64_t size;
    std::memcpy(&size, bytes.data() + offset, 8);
    offset += 8;
    if (size > bytes.size() - offset)
      throw ToolError("Truncated project assets");
    auto result = bytes.substr(offset, static_cast<size_t>(size));
    offset += static_cast<size_t>(size);
    return result;
  };
  ProjectFile p;
  p.textures = blob();
  p.drawables = blob();
  p.document = Json::parse(bytes.substr(offset));
  return p;
}
std::string ProjectFile::encode() const {
  std::string out = "CTLSTJ";
  for (auto data : {&textures, &drawables}) {
    uint64_t n = data->size();
    out.append(reinterpret_cast<const char *>(&n), 8);
    out += *data;
  }
  out += document.dump();
  return out;
}
Json tool_catalog() {
  Json tools = Json::array();
  auto add = [&](const char *name, const char *description, Json properties = Json::object(),
                 Json required = Json::array()) {
    tools.push_back({{"name", name},
                     {"description", description},
                     {"inputSchema",
                      {{"type", "object"},
                       {"properties", properties},
                       {"required", required},
                       {"additionalProperties", false}}}});
  };
  add("catalyst_status", "Inspect native bridge readiness and the supported executable fingerprint.");
  add("catalyst_get_docs",
      "Read the built-in authoring guide: workflow, graph structure, property semantics, preview limits and "
      "export. Omit topic for the overview. Catalyst itself is documented at "
      "https://docs.fivem.net/docs/catalyst/.",
      {{"topic", {{"type", "string"}, {"enum", documentation_topics()}}}});
  add("catalyst_get_project", "Read the current project graph, properties, collections and clip regions.");
  add("catalyst_list_node_types", "List the actual node factories registered by this Catalyst build.");
  add("catalyst_get_node_template", "Get Catalyst's default serialized node and its property schema.",
      {{"type", {{"type", "string"}}}}, {"type"});
  add("catalyst_new_project", "Create a new empty project. Replaces the current project.");
  add("catalyst_create_collection", "Create a collection using Catalyst's native command.",
      {{"name", {{"type", "string"}}}});
  add("catalyst_save_project", "Save the current project to an absolute .ctlst path.",
      {{"path", {{"type", "string"}}}}, {"path"});
  add("catalyst_open_project", "Open an existing .ctlst project, replacing the current project.",
      {{"path", {{"type", "string"}}}}, {"path"});
  const Json str = {{"type", "string"}}, integer = {{"type", "integer"}, {"minimum", 0}},
             number = {{"type", "number"}}, boolean = {{"type", "boolean"}}, object = {{"type", "object"}},
             array = {{"type", "array"}};
  add("catalyst_create_particle",
      "Create a connected effect, event emitter, emitter, particle, sprite draw, shader, color and size "
      "graph. Assign a texture to the shader to make it visible.",
      {{"name", str}, {"collection_id", integer}}, {"name", "collection_id"});
  add("catalyst_place_nodes",
      "Place native default nodes. Each node specifies type, optional name, x, y and properties. Inspect "
      "templates first.",
      {{"view_id", integer}, {"nodes", array}}, {"view_id", "nodes"});
  const Json connection = {{"view_id", integer},
                           {"from_node", integer},
                           {"from_port", str},
                           {"to_node", integer},
                           {"to_port", str}};
  add("catalyst_connect_nodes",
      "Connect compatible named ports. Supports multiple behavior inputs; rejects cycles.", connection,
      {"view_id", "from_node", "from_port", "to_node", "to_port"});
  add("catalyst_disconnect_nodes", "Remove one exact connection.", connection,
      {"view_id", "from_node", "from_port", "to_node", "to_port"});
  add("catalyst_set_node_properties",
      "Merge typed properties into a node's native v object. Use templates for property names and units.",
      {{"view_id", integer}, {"node_id", integer}, {"properties", object}},
      {"view_id", "node_id", "properties"});
  add("catalyst_move_nodes", "Move nodes with positions [{node_id,x,y}].",
      {{"view_id", integer}, {"positions", array}}, {"view_id", "positions"});
  add("catalyst_delete_nodes", "Remove nodes and their incident connections.",
      {{"view_id", integer}, {"node_ids", array}}, {"view_id", "node_ids"});
  add("catalyst_set_curve",
      "Replace an animated property curve. Keyframes: [{time:0..1,value:[x,y,z,w],interpolation:0}].",
      {{"view_id", integer},
       {"node_id", integer},
       {"property", str},
       {"keyframes", array},
       {"inverted", boolean}},
      {"view_id", "node_id", "property", "keyframes"});
  Json operation_names = Json::array();
  for (const char *operation : graph_operations)
    operation_names.push_back(operation);
  add("catalyst_edit_graph",
      "Apply many graph edits as one transaction with a single project reload; nothing changes if any "
      "operation fails. Each operation is {op, ...arguments}, where the arguments are those of the "
      "catalyst_<op> tool and view_id defaults to this call's. A place_nodes node may carry a string ref; "
      "later operations accept that ref wherever a node id is expected. Returns new node ids in placement "
      "order and the id behind every ref.",
      {{"view_id", integer},
       {"operations",
        {{"type", "array"},
         {"items",
          {{"type", "object"},
           {"properties", {{"op", {{"type", "string"}, {"enum", operation_names}}}}},
           {"required", Json::array({"op"})}}}}}},
      {"view_id", "operations"});
  add("catalyst_set_evolution_properties",
      "Choose which animated properties a ParticleEvolution node overrides. Each entry is {property, "
      "blend_mode, lod}; property is a keyframe property name such as ptxEffectRule:m_colourTintMinKFP or "
      "ptxu_Colour:m_rgbaMinKFP, or its numeric id. Catalyst gives the node one curve per property; the "
      "returned node lists their names for catalyst_set_curve.",
      {{"view_id", integer}, {"node_id", integer}, {"properties", array}},
      {"view_id", "node_id", "properties"});
  add("catalyst_set_sprite_animation",
      "Configure flipbook frame range, FPS, blending, hold and lifetime matching.",
      {{"view_id", integer},
       {"node_id", integer},
       {"first_frame", integer},
       {"last_frame", integer},
       {"fps", number},
       {"hold_last", boolean},
       {"match_life", boolean},
       {"blend_frames", boolean}},
      {"view_id", "node_id", "first_frame", "last_frame", "fps"});
  add("catalyst_add_texture", "Import a .dds file into a collection.",
      {{"path", str}, {"collection_id", integer}}, {"path", "collection_id"});
  add("catalyst_add_drawable", "Import a .ydr file into a collection.",
      {{"path", str}, {"collection_id", integer}}, {"path", "collection_id"});
  add("catalyst_import_ypt", "Import a Legacy or Enhanced YPT into the current project.", {{"path", str}},
      {"path"});
  add("catalyst_export_ypt", "Export checked collections into an existing directory and wait for completion.",
      {{"directory", str}, {"legacy", boolean}, {"enhanced", boolean}}, {"directory"});
  add("catalyst_set_viewport_mode",
      "Switch the preview viewport, like the dropdown in its corner: preview shows the effect as the game "
      "draws it; editor adds the grid and domain outlines; wireframe shows only the outlines; simulation "
      "shows outlines and particles as dots. catalyst_status reports the current mode.",
      {{"mode", {{"type", "string"}, {"enum", {"preview", "editor", "wireframe", "simulation"}}}}}, {"mode"});
  add("catalyst_render_particle",
      "Seek a graph preview to a normalized playback position and return a PNG image. Duration is seconds "
      "for one normalized interval.",
      {{"view_id", integer},
       {"position", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
       {"duration", number},
       {"image", boolean}},
      {"view_id", "position"});
  add("catalyst_import_clip_regions", "Import ptxclipregions.dat into the project.", {{"path", str}},
      {"path"});
  add("catalyst_export_clip_regions", "Export the project's clip regions as ptxclipregions.dat.",
      {{"path", str}}, {"path"});
  add("catalyst_create_clip_regions",
      "Create an atlas grid with native default crop bounds. Optional frames: "
      "[{index,u_min,u_max,v_min,v_max}] override per-frame bounds within the tile.",
      {{"texture", str}, {"columns", integer}, {"rows", integer}, {"frames", array}},
      {"texture", "columns", "rows"});
  return tools;
}
void validate_arguments(const Json &tool, const Json &args) {
  const auto &properties = tool.at("inputSchema").at("properties");
  for (auto it = args.begin(); it != args.end(); ++it)
    if (!properties.contains(it.key()))
      throw ToolError("Unknown argument: " + it.key());
  for (const auto &key : tool.at("inputSchema").at("required"))
    if (!args.contains(key.get<std::string>()))
      throw ToolError("Missing argument: " + key.get<std::string>());
  for (auto it = args.begin(); it != args.end(); ++it) {
    const auto &schema = properties.at(it.key());
    auto type = schema.value("type", std::string());
    const auto &value = it.value();
    if ((type == "string" && !value.is_string()) || (type == "integer" && !value.is_number_integer()) ||
        (type == "number" && !value.is_number()) || (type == "boolean" && !value.is_boolean()) ||
        (type == "object" && !value.is_object()) || (type == "array" && !value.is_array()))
      throw ToolError("Invalid argument type: " + it.key());
    if (schema.contains("minimum") && value.get<double>() < schema["minimum"].get<double>())
      throw ToolError("Argument below minimum: " + it.key());
    if (schema.contains("maximum") && value.get<double>() > schema["maximum"].get<double>())
      throw ToolError("Argument above maximum: " + it.key());
  }
}
Json dispatch_tool(const std::string &name, const Json &args, const Invoke &native) {
  for (const auto &t : tool_catalog())
    if (t["name"] == name) {
      validate_arguments(t, args);
      if (name == "catalyst_get_docs")
        return documentation(args);
      return native(name, args);
    }
  throw ToolError("Unknown tool: " + name);
}
} // namespace catalyst
