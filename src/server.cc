#include <string>
#include <string_view>

#include "boost/asio/io_context.hpp"

#include "fmt/format.h"

#if MOTIS_ENABLE_IPC
#include <cctype>
#include <algorithm>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "boost/beast/http.hpp"
#include "boost/json.hpp"

#include "motis/ipc/nng_server.h"
#endif  // MOTIS_ENABLE_IPC

#include "net/lb.h"
#include "net/run.h"
#include "net/stop_handler.h"
#include "net/web_server/web_server.h"

#include "utl/enumerate.h"
#include "utl/init_from.h"
#include "utl/logging.h"
#include "utl/parser/arg_parser.h"

#include "ctx/ctx.h"

#include "motis/config.h"
#include "motis/ctx_data.h"
#include "motis/ctx_exec.h"
#include "motis/data.h"
#include "motis/motis_instance.h"

namespace fs = std::filesystem;

namespace motis {

#if MOTIS_ENABLE_IPC
namespace {

boost::beast::http::verb to_http_verb(std::string_view method) {
  using boost::beast::http::verb;
  auto normalized = std::string(method);
  std::transform(
      normalized.begin(), normalized.end(), normalized.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (normalized.empty() || normalized == "POST") {
    return verb::post;
  } else if (normalized == "GET") {
    return verb::get;
  } else if (normalized == "PUT") {
    return verb::put;
  } else if (normalized == "DELETE") {
    return verb::delete_;
  } else if (normalized == "PATCH") {
    return verb::patch;
  }
  throw std::runtime_error(fmt::format("unsupported IPC method '{}'", method));
}

std::string extract_body(net::web_server::http_res_t&& res) {
  return std::visit(
      [](auto&& reply) -> std::string {
        using res_t = std::decay_t<decltype(reply)>;
        if constexpr (std::is_same_v<res_t, net::web_server::string_res_t>) {
          return std::move(reply.body());
        } else if constexpr (std::is_same_v<res_t,
                                            net::web_server::empty_res_t>) {
          return {};
        } else {
          throw std::runtime_error(
              "IPC request returned unsupported payload type");
        }
      },
      std::move(res));
}

std::string handle_ipc_request(net::query_router<ctx_exec>& router,
                               std::string const& req_json) {
  namespace json = boost::json;
  namespace http = boost::beast::http;

  auto parsed = json::parse(req_json);
  if (!parsed.is_object()) {
    throw std::runtime_error("IPC payload must be a JSON object");
  }

  auto const& obj = parsed.as_object();
  auto const path_it = obj.if_contains("path");
  if (path_it == nullptr || !path_it->is_string()) {
    throw std::runtime_error("IPC payload requires string field 'path'");
  }
  auto json_path = path_it->as_string();
  auto target = std::string{json_path.data(), json_path.size()};
  if (!target.starts_with('/')) {
    throw std::runtime_error("IPC path must start with '/'");
  }

  auto method = std::string{"POST"};
  if (auto const method_it = obj.if_contains("method"); method_it != nullptr) {
    if (!method_it->is_string()) {
      throw std::runtime_error("IPC field 'method' must be a string");
    }
    auto json_method = method_it->as_string();
    method.assign(json_method.data(), json_method.size());
  }

  auto const verb = to_http_verb(method);
  std::string body;
  if (auto const body_it = obj.if_contains("body"); body_it != nullptr) {
    body = json::serialize(*body_it);
  } else if (verb == http::verb::post || verb == http::verb::put ||
             verb == http::verb::patch) {
    body = "{}";
  }

  auto req = net::web_server::http_req_t{};
  req.version(11);
  req.keep_alive(false);
  req.target(target);
  req.method(verb);
  if (!body.empty()) {
    req.body() = std::move(body);
    req.set(http::field::content_type, "application/json");
  }
  req.prepare_payload();

  auto promise = std::make_shared<std::promise<std::string>>();
  auto future = promise->get_future();

  router(
      std::move(req),
      [promise](net::web_server::http_res_t&& res) {
        try {
          promise->set_value(extract_body(std::move(res)));
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
      },
      false);

  return future.get();
}

}  // namespace
#endif  // MOTIS_ENABLE_IPC

int server(data d,
           config const& c,
           server_settings const& settings,
           std::string_view const motis_version) {
  auto scheduler = ctx::scheduler<ctx_data>{};
  auto m = motis_instance{ctx_exec{scheduler.runner_.ios(), scheduler}, d, c,
                          motis_version};

  auto s = net::web_server{scheduler.runner_.ios()};
  s.set_timeout(std::chrono::minutes{5});
  s.on_http_request(m.qr_);

#if MOTIS_ENABLE_IPC
  std::unique_ptr<ipc::nng_server> ipc_server;
  if (settings.ipc_enable) {
    auto handler = [&qr = m.qr_](std::string const& req_json) {
      return handle_ipc_request(qr, req_json);
    };
    ipc_server = std::make_unique<ipc::nng_server>(
        ipc::nng_server_config{settings.ipc_address}, std::move(handler));
    ipc_server->start();
    utl::log_info("motis.server", "IPC listening on {}", settings.ipc_address);
  }
#endif

  auto ec = boost::system::error_code{};
  auto const server_config = c.server_.value_or(config::server{});
  s.init(server_config.host_, server_config.port_, ec);
  if (ec) {
    std::cerr << "error: " << ec << "\n";
    return 1;
  }

  auto const stop = net::stop_handler(scheduler.runner_.ios(), [&]() {
    utl::log_info("motis.server", "shutdown");
#if MOTIS_ENABLE_IPC
    if (ipc_server) {
      ipc_server->stop();
    }
#endif
    s.stop();
    m.stop();
    scheduler.runner_.stop();
  });

  utl::log_info(
      "motis.server",
      "n_threads={}, listening on {}:{}\nlocal link: http://localhost:{}",
      c.n_threads(), server_config.host_, server_config.port_,
      server_config.port_);

  s.run();
  m.run(d, c);
  scheduler.runner_.run(c.n_threads());
#if MOTIS_ENABLE_IPC
  if (ipc_server) {
    ipc_server->stop();
    ipc_server.reset();
  }
#endif
  m.join();

  return 0;
}

unsigned get_api_version(boost::urls::url_view const& url) {
  if (url.encoded_path().length() > 7) {
    return utl::parse<unsigned>(
        std::string_view{url.encoded_path().substr(6, 2)});
  }
  return 0U;
}

}  // namespace motis
