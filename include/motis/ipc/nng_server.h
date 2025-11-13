#pragma once

#if MOTIS_ENABLE_IPC

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <nng/nng.h>

namespace motis::ipc {

struct nng_server_config {
  std::string address;
};

class nng_server {
public:
  using request_handler_t = std::function<std::string(std::string const&)>;

  nng_server(nng_server_config cfg, request_handler_t handler);
  ~nng_server();

  // non-copyable
  nng_server(nng_server const&) = delete;
  nng_server& operator=(nng_server const&) = delete;

  void start();
  void stop();

private:
  void run();
  void close_socket();

  nng_server_config cfg_;
  request_handler_t handler_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  nng_socket socket_{};
};

}  // namespace motis::ipc

#endif  // MOTIS_ENABLE_IPC
