#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>

#include "conf/configuration.h"

#include "utl/init_from.h"
#include "utl/parallel_for.h"
#include "utl/parser/cstr.h"

#include "motis/config.h"
#include "motis/data.h"
#include "motis/motis_instance.h"

#include "./flags.h"

namespace fs = std::filesystem;
namespace po = boost::program_options;
namespace json = boost::json;

struct thousands_sep : std::numpunct<char> {
  char do_thousands_sep() const override { return ','; }
  std::string do_grouping() const override { return "\3"; }
};

struct stats {
  static constexpr auto kMaxMs = 600'000ULL;  // 10 minutes

  stats() = default;
  stats(std::string name, std::uint64_t)
      : name_{std::move(name)}, histogram_(kMaxMs + 1, 0U) {}

  void add(uint64_t, std::uint64_t value) {
    auto const bucket = value < kMaxMs ? value : kMaxMs;
    ++histogram_[bucket];
    sum_ += value;
    ++count_;
    if (value < min_) {
      min_ = value;
    }
    if (value > max_) {
      max_ = value;
    }
  }

  std::uint64_t quantile(double q) const {
    auto const target = static_cast<std::uint64_t>(
        std::round(q * static_cast<double>(count_ - 1)));
    std::uint64_t cumulative = 0;
    auto const search_limit = std::min<std::uint64_t>(max_, kMaxMs);
    for (auto i = 0ULL; i <= search_limit; ++i) {
      cumulative += histogram_[i];
      if (cumulative > target) {
        return i;
      }
    }
    return kMaxMs;
  }

  std::string name_;
  std::vector<std::uint64_t> histogram_;
  std::uint64_t sum_{}, count_{};
  std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t max_{};
};

void print_stats(stats const& s) {
  if (s.count_ == 0) {
    return;
  }
  auto const avg = s.sum_ / static_cast<double>(s.count_);
  std::cout << s.name_ << "\n      average: " << std::right << std::setw(15)
            << std::setprecision(2) << std::fixed << avg
            << "\n          max: " << std::right << std::setw(12) << s.max_
            << "\n  99 quantile: " << std::right << std::setw(12)
            << s.quantile(0.99) << "\n  90 quantile: " << std::right
            << std::setw(12) << s.quantile(0.9)
            << "\n  80 quantile: " << std::right << std::setw(12)
            << s.quantile(0.8) << "\n  50 quantile: " << std::right
            << std::setw(12) << s.quantile(0.5)
            << "\n          min: " << std::right << std::setw(12) << s.min_
            << "\n"
            << std::endl;
}

namespace motis {

int batch(int ac, char** av) {
  auto data_path = fs::path{"data"};
  auto queries_path = fs::path{"queries.txt"};
  auto responses_path = fs::path{"responses.txt"};
  auto mt = true;

  auto desc = po::options_description{"Options"};
  desc.add_options()  //
      ("help", "Prints this help message")  //
      ("multithreading,mt", po::value(&mt)->default_value(mt))  //
      ("queries,q", po::value(&queries_path)->default_value(queries_path),
       "queries file")  //
      ("responses,r", po::value(&responses_path)->default_value(responses_path),
       "response file");
  add_data_path_opt(desc, data_path);

  auto vm = parse_opt(ac, av, desc);
  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }

  auto queries = std::vector<std::string_view>{};
  auto f = cista::mmap{queries_path.generic_string().c_str(),
                       cista::mmap::protection::READ};
  utl::for_each_token(utl::cstr{f.view()}, '\n',
                      [&](utl::cstr s) { queries.push_back(s.view()); });

  auto const c = config::read(data_path / "config.yml");
  utl::verify(c.timetable_.has_value(), "timetable required");

  auto d = data{data_path, c};
  utl::verify(d.tt_, "timetable required");

  auto response_time = stats{"response_time", 0U};

  struct state {};

  auto out = std::ofstream{responses_path};
  auto m = motis_instance{net::default_exec{}, d, c, ""};
  auto const compute_response = [&](state&, std::size_t const id) {
    UTL_START_TIMING(request);
    auto response = std::string{};
    try {
      m.qr_(
          {boost::beast::http::verb::get,
           boost::beast::string_view{queries.at(id)}, 11},
          [&](net::web_server::http_res_t const& res) {
            std::visit(
                [&](auto&& r) {
                  using ResponseType = std::decay_t<decltype(r)>;
                  if constexpr (std::is_same_v<ResponseType,
                                               net::web_server::string_res_t>) {
                    response = r.body();
                    if (response.empty()) {
                      std::cout << "empty response for " << id << ": "
                                << queries.at(id) << " [status=" << r.result()
                                << "]\n";
                    }
                  } else {
                    throw utl::fail("not a valid response type: {}",
                                    cista::type_str<ResponseType>());
                  }
                },
                res);
          },
          false);
    } catch (std::exception const& e) {
      std::cerr << "ERROR IN QUERY " << id << ": " << e.what() << "\n";
    }
    return std::pair{UTL_GET_TIMING_MS(request), std::move(response)};
  };

  auto const pt = utl::activate_progress_tracker("batch");
  pt->in_high(queries.size());
  if (mt) {
    utl::parallel_ordered_collect_threadlocal<state>(
        queries.size(), compute_response,
        [&](std::size_t const id,
            std::pair<std::uint64_t, std::string> const& s) {
          response_time.add(id, s.first);
          out << s.second << "\n";
        },
        pt->update_fn());
  } else {
    auto s = state{};
    for (auto i = 0U; i != queries.size(); ++i) {
      compute_response(s, i);
      pt->increment();
    }
  }

  std::cout << "\nresponse_time\n=============\n" << std::endl;
  std::cout.imbue(std::locale(std::locale::classic(), new thousands_sep));
  print_stats(response_time);

  return 0U;
}

}  // namespace motis
