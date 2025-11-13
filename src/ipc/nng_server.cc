#include "motis/ipc/nng_server.h"

#if MOTIS_ENABLE_IPC

#include <exception>
#include <utility>

#include <nng/protocol/reqrep0/rep.h>

#include "fmt/format.h"

#include "utl/logging.h"

namespace motis::ipc {

nng_server::nng_server(nng_server_config cfg, request_handler_t handler)
    : cfg_{std::move(cfg)}, handler_{std::move(handler)} {}

nng_server::~nng_server() { stop(); }

void nng_server::start() {
  if (thread_.joinable()) {
    return;
  }
  stop_.store(false);
  thread_ = std::thread{&nng_server::run, this};
}

void nng_server::stop() {
  stop_.store(true);
  close_socket();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void nng_server::close_socket() {
  if (socket_.id != 0U) {
    nng_close(socket_);
    socket_ = nng_socket{};
  }
}

void nng_server::run() {
  int rv = nng_rep0_open(&socket_);
  if (rv != 0) {
    utl::log_error("motis.ipc", "nng_rep0_open failed: {}", nng_strerror(rv));
    socket_ = nng_socket{};
    return;
  }

  rv = nng_listen(socket_, cfg_.address.c_str(), nullptr, 0);
  if (rv != 0) {
    utl::log_error("motis.ipc", "nng_listen failed on {}: {}", cfg_.address,
                   nng_strerror(rv));
    close_socket();
    return;
  }

  utl::log_info("motis.ipc", "IPC server listening on {}", cfg_.address);

  while (!stop_.load()) {
    nng_msg* msg = nullptr;
    rv = nng_recvmsg(socket_, &msg, 0);
    if (rv != 0) {
      if (rv == NNG_ECLOSED || stop_.load()) {
        break;
      }
      utl::log_error("motis.ipc", "nng_recvmsg error: {}", nng_strerror(rv));
      continue;
    }

    auto const* data = static_cast<char const*>(nng_msg_body(msg));
    auto const len = nng_msg_len(msg);
    std::string req_json{data, len};

    std::string res_json;
    try {
      res_json = handler_(req_json);
    } catch (std::exception const& e) {
      res_json = fmt::format(R"({{"error":"{}"}})", e.what());
    } catch (...) {
      res_json = R"({"error":"unknown ipc handler error"})";
    }

    nng_msg_clear(msg);
    if (auto const append_rv =
            nng_msg_append(msg, res_json.data(), res_json.size());
        append_rv != 0) {
      utl::log_error("motis.ipc", "nng_msg_append error: {}",
                     nng_strerror(append_rv));
      nng_msg_free(msg);
      continue;
    }

    rv = nng_sendmsg(socket_, msg, 0);
    if (rv != 0) {
      utl::log_error("motis.ipc", "nng_sendmsg error: {}", nng_strerror(rv));
      nng_msg_free(msg);
    }
  }

  close_socket();
}

}  // namespace motis::ipc

#endif  // MOTIS_ENABLE_IPC
