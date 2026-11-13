#include "catalyst/mcp.hpp"
#include <Windows.h>
#include <algorithm>
#include <httplib.h>
#include <wincrypt.h>
namespace catalyst {
std::string base64(const std::string &bytes) {
  DWORD size = 0;
  if (!CryptBinaryToStringA(reinterpret_cast<const BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size()),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size))
    throw ToolError("Base64 encoding failed");
  std::string out(size, '\0');
  CryptBinaryToStringA(reinterpret_cast<const BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size()),
                       CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &size);
  out.resize(size);
  return out;
}
std::string unbase64(const std::string &text) {
  DWORD size = 0;
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &size,
                            nullptr, nullptr))
    throw ToolError("Invalid base64");
  std::string out(size, '\0');
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                            reinterpret_cast<BYTE *>(out.data()), &size, nullptr, nullptr))
    throw ToolError("Invalid base64");
  out.resize(size);
  return out;
}
namespace {
Json error(const Json &id, int code, const std::string &message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}
bool supported(const std::string &version) {
  return version == "2025-03-26" || version == "2025-06-18" || version == "2025-11-25";
}
bool equal_token(const std::string &a, const std::string &b) {
  size_t difference = a.size() ^ b.size();
  for (size_t i = 0; i < b.size(); ++i)
    difference |= static_cast<unsigned char>((i < a.size() ? a[i] : 0) ^ b[i]);
  return difference == 0;
}
} // namespace
McpServer::McpServer(Invoke native, std::string token, int port)
    : native_(std::move(native)), token_(std::move(token)), port_(port),
      server_(std::make_unique<httplib::Server>()) {
  server_->set_payload_max_length(32 * 1024 * 1024);
  server_->set_read_timeout(10, 0);
  server_->set_write_timeout(30, 0);
  server_->new_task_queue = [] { return new httplib::ThreadPool(4, 32); };
  server_->set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
    const auto origin = req.get_header_value("Origin");
    const auto local = "http://127.0.0.1:" + std::to_string(port_);
    if (!origin.empty() && origin != local && origin != "http://localhost:" + std::to_string(port_)) {
      res.status = 403;
      return httplib::Server::HandlerResponse::Handled;
    }
    const auto host = req.get_header_value("Host");
    if (host != "127.0.0.1:" + std::to_string(port_) && host != "localhost:" + std::to_string(port_)) {
      res.status = 403;
      return httplib::Server::HandlerResponse::Handled;
    }
    if (!equal_token(req.get_header_value("Authorization"), "Bearer " + token_)) {
      res.status = 401;
      res.set_header("WWW-Authenticate", "Bearer");
      return httplib::Server::HandlerResponse::Handled;
    }
    auto version = req.get_header_value("MCP-Protocol-Version");
    if (!version.empty() && !supported(version)) {
      res.status = 400;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  server_->Post("/mcp", [this](const httplib::Request &req, httplib::Response &res) {
    if (req.get_header_value("Content-Type").find("application/json") == std::string::npos) {
      res.status = 415;
      return;
    }
    const auto accept = req.get_header_value("Accept");
    if (accept.find("application/json") == std::string::npos ||
        accept.find("text/event-stream") == std::string::npos) {
      res.status = 406;
      return;
    }
    Json response;
    try {
      response = rpc(Json::parse(req.body));
    } catch (const Json::parse_error &) {
      response = error(nullptr, -32700, "Parse error");
    } catch (const std::exception &) {
      response = error(nullptr, -32603, "Internal error");
    }
    if (response.is_null()) {
      res.status = 202;
      return;
    }
    res.set_content(response.dump(), "application/json");
  });
  const auto no_stream = [](const httplib::Request &, httplib::Response &res) {
    res.status = 405;
    res.set_header("Allow", "POST");
  };
  server_->Get("/mcp", no_stream);
  server_->Delete("/mcp", no_stream);
}
McpServer::~McpServer() {
  stop();
}
bool McpServer::listen() {
  return server_->listen("127.0.0.1", port_);
}
void McpServer::stop() {
  server_->stop();
}
Json McpServer::rpc(const Json &request) {
  if (!request.is_object())
    return error(nullptr, -32600, "Expected one JSON-RPC object");
  if (request.value("jsonrpc", Json()) == "2.0" && request.contains("id") && !request.contains("method") &&
      (request.contains("result") != request.contains("error")))
    return nullptr;
  const auto id = request.value("id", Json(nullptr));
  if (request.value("jsonrpc", Json()) != "2.0" || !request.contains("method") ||
      !request["method"].is_string() ||
      (request.contains("id") && !id.is_null() && !id.is_string() && !id.is_number_integer()))
    return error(nullptr, -32600, "Invalid request");
  const auto method = request["method"].get<std::string>();
  if (!request.contains("id"))
    return nullptr;
  const auto params = request.value("params", Json::object());
  if (!params.is_object())
    return error(id, -32602, "Parameters must be an object");
  Json result;
  if (method == "initialize") {
    auto version = params.value("protocolVersion", Json("2025-11-25"));
    if (!version.is_string() || !supported(version.get<std::string>()))
      version = "2025-11-25";
    result = {{"protocolVersion", version},
              {"capabilities", {{"tools", {{"listChanged", false}}}}},
              {"serverInfo", {{"name", "catalyst-mcp"}, {"version", "0.1.0"}}},
              {"instructions",
               "Use catalyst_status first. Mutations run on Catalyst's editor thread. Inspect node "
               "templates and ports before connecting. Every graph tool call reloads the project, so "
               "send more than a few edits as one catalyst_edit_graph call. Save your project before "
               "replacing it. catalyst_get_docs returns the authoring guide; Catalyst itself is documented "
               "at https://docs.fivem.net/docs/catalyst/."}};
  } else if (method == "ping")
    result = Json::object();
  else if (method == "tools/list")
    result = {{"tools", tool_catalog()}};
  else if (method == "tools/call") {
    if (!params.contains("name") || !params["name"].is_string() ||
        !params.value("arguments", Json::object()).is_object())
      return error(id, -32602, "Expected a tool name and object arguments");
    bool known = false;
    for (const auto &tool : tool_catalog())
      if (tool["name"] == params["name"])
        known = true;
    if (!known)
      return error(id, -32602, "Unknown tool");
    try {
      auto value = dispatch_tool(params["name"], params.value("arguments", Json::object()), native_);
      if (value.is_object() && value.contains("_mcp_content"))
        result = {{"content", value["_mcp_content"]}, {"isError", false}};
      else
        result = {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})},
                  {"structuredContent", value.is_object() ? value : Json{{"value", value}}},
                  {"isError", false}};
    } catch (const std::exception &e) {
      result = {{"content", Json::array({{{"type", "text"}, {"text", e.what()}}})}, {"isError", true}};
    }
  } else
    return error(id, -32601, "Method not found");
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
} // namespace catalyst
