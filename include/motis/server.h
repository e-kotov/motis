#pragma once

#include <string>
#include <string_view>

#include "boost/url/url_view.hpp"

namespace motis {

struct data;
struct config;

inline std::string default_ipc_address() {
#if defined(_WIN32)
  return "ipc://motis-ipc";
#else
  return "ipc:///tmp/motis-ipc.sock";
#endif
}

struct server_settings {
  bool ipc_enable = false;
  std::string ipc_address = default_ipc_address();
};

int server(data d,
           config const& c,
           server_settings const& settings,
           std::string_view);

unsigned get_api_version(boost::urls::url_view const&);

}  // namespace motis
