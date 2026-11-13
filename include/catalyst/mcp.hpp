#pragma once
#include "common.hpp"
#include <memory>
namespace httplib {
class Server;
}
namespace catalyst {
class McpServer {
public:
  McpServer(Invoke native, std::string token, int port);
  ~McpServer();
  bool listen();
  void stop();
  Json rpc(const Json &request);

private:
  Invoke native_;
  std::string token_;
  int port_;
  std::unique_ptr<httplib::Server> server_;
};
} // namespace catalyst
