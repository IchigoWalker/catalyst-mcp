#include "catalyst/common.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
namespace catalyst {
namespace {
uint64_t next_id(const Json &values, const char *field) {
  uint64_t result = 1;
  for (const auto &value : values)
    result = std::max(result, value.at(field).get<uint64_t>() + 1);
  return result;
}
void valid_name(const std::string &name) {
  if (name.empty() || name.size() > 128 || name.find_first_of("/\\:\r\n\t") != std::string::npos)
    throw ToolError("Name must contain 1-128 characters and no path separators or control characters");
}
Json &view(Json &doc, const Json &args) {
  auto id = args.at("view_id").get<uint64_t>();
  for (auto &v : doc["views"])
    if (v["id"] == id)
      return v;
  throw ToolError("View was not found");
}
Json &node(Json &v, uint64_t id) {
  for (auto &n : v["nodes"])
    if (n["i"] == id)
      return n;
  throw ToolError("Node was not found");
}
void patch_values(Json &values, const Json &patch) {
  if (!patch.is_object())
    throw ToolError("Properties must be an object");
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    if (!values.contains(it.key()))
      throw ToolError("Unknown property: " + it.key());
    auto &old = values[it.key()];
    const auto &value = it.value();
    if (old.is_null())
      throw ToolError("Use connections or curve tools for property: " + it.key());
    if (old.is_object())
      patch_values(old, value);
    else {
      if (old.is_number() && !value.is_number())
        throw ToolError("Expected a number: " + it.key());
      if (old.is_number_integer() && !value.is_number_integer())
        throw ToolError("Expected an integer: " + it.key());
      if (!old.is_number() && old.type() != value.type())
        throw ToolError("Property type mismatch: " + it.key());
      if (value.is_number() && !std::isfinite(value.get<double>()))
        throw ToolError("Property must be finite");
      old = value;
    }
  }
}
const Json &port(const Json &ports, const std::string &name) {
  for (const auto &p : ports)
    if (p["name"] == name)
      return p;
  throw ToolError("Unknown port: " + name);
}
void connect(Json &v, const Json &c, const Invoke &templates) {
  auto from = c.at("f").get<uint64_t>(), to = c.at("t").get<uint64_t>();
  if (from == to)
    throw ToolError("Cannot connect a node to itself");
  auto &source = node(v, from);
  auto &destination = node(v, to);
  auto src = templates("template", {{"type", source["t"]}}),
       dst = templates("template", {{"type", destination["t"]}});
  const auto &output = port(src["outputs"], c.at("fp"));
  const auto &input = port(dst["inputs"], c.at("tp"));
  if (output["data_type"] != input["data_type"] ||
      (output["data_type"] == 10 && output["reference_type"] != input["reference_type"]))
    throw ToolError("Port data types are incompatible");
  if (!input.value("multiple", false))
    for (const auto &edge : v["connections"])
      if (edge["t"] == to && edge["tp"] == c["tp"])
        throw ToolError("Input already has a connection; disconnect it first");
  if (std::find(v["connections"].begin(), v["connections"].end(), c) != v["connections"].end())
    throw ToolError("Connection already exists");
  // Cycles can recurse indefinitely during graph evaluation.
  std::set<uint64_t> visited;
  std::vector<uint64_t> pending{to};
  while (!pending.empty()) {
    auto id = pending.back();
    pending.pop_back();
    if (id == from)
      throw ToolError("Connection would create a cycle");
    if (!visited.insert(id).second)
      continue;
    for (const auto &edge : v["connections"])
      if (edge["f"] == id)
        pending.push_back(edge["t"].get<uint64_t>());
  }
  v["connections"].push_back(c);
}
Json add_node(Json &v, const Json &spec, const Invoke &templates, uint64_t id) {
  const auto type = spec.at("type").get<std::string>();
  auto result = templates("template", {{"type", type}}).at("node");
  result["i"] = id;
  result["p"] = {{"x", spec.value("x", 0.0)}, {"y", spec.value("y", 0.0)}};
  if (spec.contains("name")) {
    auto name = spec.at("name").get<std::string>();
    valid_name(name);
    result["n"] = name;
  }
  if (spec.contains("properties"))
    patch_values(result["v"], spec["properties"]);
  v["nodes"].push_back(result);
  return result;
}
// Runs a list of graph operations against a copy of the document and commits it only if all succeed.
Json edit_graph(Json &doc, const Json &args, const Invoke &templates) {
  const auto &operations = args.at("operations");
  if (operations.empty() || operations.size() > 16384)
    throw ToolError("Provide between 1 and 16384 operations");
  view(doc, args);
  std::map<std::string, Json> schemas;
  for (const auto &tool : tool_catalog())
    schemas[tool["name"].get<std::string>()] = tool;
  // A batch asks for the same few templates thousands of times; each one costs a native node construction.
  std::map<std::string, Json> cache;
  const Invoke cached = [&](const std::string &name, const Json &a) {
    const auto type = a.at("type").get<std::string>();
    auto it = cache.find(type);
    if (it == cache.end())
      it = cache.emplace(type, templates(name, a)).first;
    return it->second;
  };
  std::map<std::string, uint64_t> refs;
  std::set<std::string> declared;
  const auto resolve = [&](Json &id) {
    if (!id.is_string())
      return;
    auto it = refs.find(id.get<std::string>());
    if (it == refs.end())
      throw ToolError("Unknown node reference: " + id.get<std::string>());
    id = it->second;
  };
  Json work = doc, placed = Json::array();
  for (size_t index = 0; index < operations.size(); ++index) {
    std::string op = "?";
    try {
      Json call = operations[index];
      if (!call.is_object() || !call.contains("op") || !call["op"].is_string())
        throw ToolError("Expected an object with a string \"op\"");
      op = call["op"].get<std::string>();
      call.erase("op");
      if (std::find(graph_operations.begin(), graph_operations.end(), op) == graph_operations.end())
        throw ToolError("Unknown operation");
      if (!call.contains("view_id"))
        call["view_id"] = args.at("view_id");
      for (const char *key : {"node_id", "from_node", "to_node"})
        if (call.contains(key))
          resolve(call[key]);
      if (call.contains("node_ids") && call["node_ids"].is_array())
        for (auto &id : call["node_ids"])
          resolve(id);
      if (call.contains("positions") && call["positions"].is_array())
        for (auto &position : call["positions"])
          if (position.is_object() && position.contains("node_id"))
            resolve(position["node_id"]);
      std::vector<std::string> names;
      if (op == "place_nodes" && call.contains("nodes") && call["nodes"].is_array())
        for (auto &spec : call["nodes"]) {
          std::string name;
          if (spec.is_object() && spec.contains("ref")) {
            if (!spec["ref"].is_string() || spec["ref"].get<std::string>().empty())
              throw ToolError("Node ref must be a non-empty string");
            name = spec["ref"].get<std::string>();
            if (!declared.insert(name).second)
              throw ToolError("Duplicate node ref: " + name);
            spec.erase("ref");
          }
          names.push_back(name);
        }
      validate_arguments(schemas.at("catalyst_" + op), call);
      const auto result = mutate_graph(work, "catalyst_" + op, call, cached);
      for (size_t i = 0; i < names.size(); ++i) {
        const auto id = result.at("nodes").at(i).at("i").get<uint64_t>();
        placed.push_back(id);
        if (!names[i].empty())
          refs[names[i]] = id;
      }
    } catch (const std::exception &e) {
      throw ToolError("operations[" + std::to_string(index) + "] (" + op + "): " + e.what());
    }
  }
  doc = std::move(work);
  const auto &v = view(doc, args);
  return {{"applied", operations.size()},
          {"placed", placed},
          {"refs", refs},
          {"nodes", v["nodes"].size()},
          {"connections", v["connections"].size()}};
}
} // namespace
Json mutate_graph(Json &doc, const std::string &operation, const Json &args, const Invoke &templates) {
  if (operation == "catalyst_edit_graph")
    return edit_graph(doc, args, templates);
  uint64_t next_node = 1;
  for (const auto &v : doc.at("views"))
    next_node = std::max(next_node, next_id(v.at("nodes"), "i"));
  if (operation == "catalyst_create_particle") {
    auto name = args.at("name").get<std::string>();
    valid_name(name);
    for (const auto &v : doc["views"])
      if (v["name"] == name)
        throw ToolError("An effect with that name already exists");
    uint64_t collection = args.at("collection_id");
    bool found = false;
    for (const auto &c : doc["collections"])
      if (c["id"] == collection)
        found = true;
    if (!found)
      throw ToolError("Collection was not found");
    Json v = {{"id", next_id(doc["views"], "id")},
              {"name", name},
              {"collectionId", collection},
              {"z", 1.0},
              {"px", 0.0},
              {"py", 0.0},
              {"cy", 0.0},
              {"cp", 0.25},
              {"cd", 5.0},
              {"tx", 0.0},
              {"ty", 0.0},
              {"tz", 0.0},
              {"nodes", Json::array()},
              {"connections", Json::array()}};
    Json ids = Json::object();
    const std::vector<std::string> types = {
        "ParticleEffectRule", "ParticleEventEmitter", "ParticleEmitterRule",   "ParticleRule",
        "ParticleDrawSprite", "SpriteShader",         "ParticleBehaviorColor", "ParticleBehaviorSize"};
    for (size_t i = 0; i < types.size(); ++i) {
      auto n = add_node(v, {{"type", types[i]}, {"x", double(i % 4) * 350.0}, {"y", double(i / 4) * 450.0}},
                        templates, next_node++);
      ids[types[i]] = n["i"];
      if (n["v"].contains("name"))
        v["nodes"].back()["v"]["name"] = name + (i ? "_" + std::to_string(i) : "");
    }
    auto edge = [&](const char *src, const char *dst, const char *input) {
      connect(v, {{"f", ids[src]}, {"t", ids[dst]}, {"fp", "output"}, {"tp", input}}, templates);
    };
    edge("ParticleEventEmitter", "ParticleEffectRule", "eventEmitter");
    edge("ParticleEmitterRule", "ParticleEventEmitter", "emitterRule");
    edge("ParticleRule", "ParticleEventEmitter", "particleRule");
    edge("ParticleDrawSprite", "ParticleRule", "draw");
    edge("SpriteShader", "ParticleDrawSprite", "shader");
    edge("ParticleBehaviorColor", "ParticleRule", "colourBehaviour");
    edge("ParticleBehaviorSize", "ParticleRule", "sizeBehaviour");
    doc["views"].push_back(v);
    doc["openTabs"].push_back(name);
    doc["activeTab"] = name;
    doc["previewTab"] = name;
    return {{"view_id", v["id"]}, {"nodes", ids}};
  }
  if (operation == "catalyst_place_nodes") {
    auto &v = view(doc, args);
    Json added = Json::array();
    if (args.at("nodes").empty() || args["nodes"].size() > 256)
      throw ToolError("Provide between 1 and 256 nodes");
    for (const auto &spec : args["nodes"])
      added.push_back(add_node(v, spec, templates, next_node++));
    return {{"nodes", added}};
  }
  if (operation == "catalyst_connect_nodes" || operation == "catalyst_disconnect_nodes") {
    auto &v = view(doc, args);
    Json c = {{"f", args.at("from_node")},
              {"t", args.at("to_node")},
              {"fp", args.at("from_port")},
              {"tp", args.at("to_port")}};
    if (operation == "catalyst_connect_nodes")
      connect(v, c, templates);
    else {
      auto &edges = v["connections"];
      auto it = std::find(edges.begin(), edges.end(), c);
      if (it == edges.end())
        throw ToolError("Connection was not found");
      edges.erase(it);
    }
    return {{"connection", c}};
  }
  if (operation == "catalyst_set_node_properties") {
    auto &n = node(view(doc, args), args.at("node_id"));
    patch_values(n["v"], args.at("properties"));
    return {{"node", n}};
  }
  if (operation == "catalyst_move_nodes") {
    auto &v = view(doc, args);
    for (const auto &p : args.at("positions"))
      node(v, p.at("node_id"))["p"] = {{"x", p.at("x")}, {"y", p.at("y")}};
    return {{"moved", args["positions"].size()}};
  }
  if (operation == "catalyst_delete_nodes") {
    auto &v = view(doc, args);
    std::set<uint64_t> ids;
    for (const auto &id : args.at("node_ids")) {
      node(v, id);
      ids.insert(id.get<uint64_t>());
    }
    auto &nodes = v["nodes"];
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&](const auto &n) { return ids.contains(n["i"].template get<uint64_t>()); }),
                nodes.end());
    auto &edges = v["connections"];
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [&](const auto &e) {
                                 return ids.contains(e["f"].template get<uint64_t>()) ||
                                        ids.contains(e["t"].template get<uint64_t>());
                               }),
                edges.end());
    return {{"deleted", ids.size()}};
  }
  if (operation == "catalyst_set_curve") {
    auto &n = node(view(doc, args), args.at("node_id"));
    const auto property = args.at("property").get<std::string>();
    if (!n.contains("k") || !n["k"].contains(property))
      throw ToolError("Unknown animated property");
    const auto &keys = args.at("keyframes");
    if (keys.empty() || keys.size() > 4096)
      throw ToolError("Provide 1-4096 keyframes");
    Json frames = Json::array();
    double last = -1;
    for (const auto &k : keys) {
      double t = k.at("time");
      if (!std::isfinite(t) || t < 0 || t > 1 || t <= last)
        throw ToolError("Keyframe times must increase strictly within [0,1]");
      last = t;
      auto values = k.at("value");
      if (!values.is_array() || values.empty() || values.size() > 4)
        throw ToolError("Keyframe value must have 1-4 components");
      Json f = {{"t", t}, {"i", k.value("interpolation", 0)}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 0.0}};
      int i = 0;
      for (const auto &val : values) {
        if (!val.is_number() || !std::isfinite(val.get<double>()))
          throw ToolError("Curve values must be finite numbers");
        f[std::array{"x", "y", "z", "w"}[i++]] = val;
      }
      frames.push_back(f);
    }
    n["k"][property] = {{"i", args.value("inverted", false)}, {"k", frames}};
    return {{"curve", n["k"][property]}};
  }
  if (operation == "catalyst_set_evolution_properties") {
    auto &n = node(view(doc, args), args.at("node_id"));
    if (n["t"] != "ParticleEvolution")
      throw ToolError("Choose a ParticleEvolution node");
    const auto &properties = args.at("properties");
    if (properties.size() > 256)
      throw ToolError("Provide at most 256 properties");
    Json entries = Json::array();
    std::set<uint32_t> seen;
    for (const auto &p : properties) {
      if (!p.is_object() || !p.contains("property"))
        throw ToolError("Each entry needs a property");
      // A keyframe property is identified by the hash of its RAGE name, such as "ptxu_Colour:m_rgbaMinKFP".
      const auto &id = p["property"];
      uint32_t pid = 0;
      if (id.is_string()) {
        for (unsigned char c : id.get<std::string>()) {
          pid += static_cast<unsigned char>(std::tolower(c));
          pid += pid << 10;
          pid ^= pid >> 6;
        }
        pid += pid << 3;
        pid ^= pid >> 11;
        pid += pid << 15;
      } else if (id.is_number_unsigned() && id.get<uint64_t>() <= 0xFFFFFFFFull)
        pid = id.get<uint32_t>();
      else
        throw ToolError("property must be a keyframe property name or its numeric id");
      if (!seen.insert(pid).second)
        throw ToolError("A property is listed twice");
      const auto blend = p.value("blend_mode", 0);
      if (blend < 0 || blend > 255)
        throw ToolError("blend_mode must be within 0..255");
      entries.push_back({{"pid", pid}, {"bm", blend}, {"lod", p.value("lod", false)}});
    }
    n["ek"] = entries;
    return {{"node_id", n["i"]}, {"properties", entries}};
  }
  if (operation == "catalyst_set_sprite_animation") {
    auto &n = node(view(doc, args), args.at("node_id"));
    if (n["t"] != "ParticleDrawSprite" && n["t"] != "ParticleDrawModel")
      throw ToolError("Choose a sprite or model draw node");
    int first = args.at("first_frame"), last = args.at("last_frame");
    double fps = args.at("fps");
    if (first < 0 || last < first || last > 65535 || !std::isfinite(fps) || fps < 0 || fps > 1000)
      throw ToolError("Invalid frame range or FPS");
    patch_values(n["v"], {{"hasAnimateTexture", true},
                          {"texFrameIdMin", first},
                          {"texFrameIdMax", first},
                          {"lastFrameId", last},
                          {"isHeldOnLastFrame", args.value("hold_last", false)},
                          {"isScaledOverParticleLife", args.value("match_life", false)},
                          {"doFrameBlending", args.value("blend_frames", true)}});
    n["k"]["animRate"] = {
        {"i", false},
        {"k", Json::array({{{"i", 0}, {"t", 0.0}, {"x", fps}, {"y", fps}, {"z", 0.0}, {"w", 0.0}}})}};
    return {{"node", n}};
  }
  throw ToolError("Unknown graph operation: " + operation);
}
} // namespace catalyst
