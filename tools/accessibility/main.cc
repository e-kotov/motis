// motis-accessibility: offline many-origin accessibility batch on top of stock
// MOTIS/Nigiri. The transit kernel is the unmodified nigiri one_to_all RAPTOR;
// access/egress connectors are either read from a file (e.g. straight-line
// connectors computed by the Rust CLI) or computed once per point with the
// same motis::ep::routing::get_offsets call the stock one-to-many endpoint uses
// per request. Results are streamed as a compact binary travel-time matrix
// (see tools/accessibility/README.md for the format and semantics).

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include "fmt/core.h"

#include "utl/init_from.h"

#include "geo/latlng.h"

#include "osr/location.h"
#include "osr/routing/profile.h"

#include "nigiri/location_routes.h"
#include "nigiri/routing/limits.h"
#include "nigiri/routing/one_to_all.h"
#include "nigiri/routing/query.h"
#include "nigiri/timetable.h"
#include "nigiri/types.h"

#include "motis-api/motis-api.h"
#include "motis/config.h"
#include "motis/data.h"
#include "motis/endpoints/one_to_many.h"
#include "motis/endpoints/routing.h"
#include "motis/gbfs/routing_data.h"
#include "motis/osr/parameters.h"
#include "motis/place.h"
#include "motis/rental_options.h"
#include "motis/timetable/modes_to_clasz_mask.h"

namespace fs = std::filesystem;
namespace n = nigiri;

namespace {

constexpr auto kFormatVersion = std::uint32_t{1U};
constexpr auto kUnreachable = std::uint8_t{255U};
constexpr auto kUnreachable16 = std::uint16_t{0xFFFFU};
constexpr auto kMaxCap = 1440U;  // u16 path; one day
constexpr auto kDirectWalk = std::uint8_t{254U};
constexpr auto kNoStop = std::uint32_t{0xFFFFFFFFU};

using clock_t_ = std::chrono::steady_clock;

double ms_since(clock_t_::time_point const t) {
  return std::chrono::duration<double, std::milli>(clock_t_::now() - t)
      .count();
}

long peak_rss_kb() {
  auto u = rusage{};
  getrusage(RUSAGE_SELF, &u);
  return u.ru_maxrss;
}

// ---------------------------------------------------------------- args
struct args {
  explicit args(int ac, char** av) {
    for (auto i = 2; i < ac; ++i) {
      auto const k = std::string_view{av[i]};
      if (!k.starts_with("--")) {
        throw std::runtime_error(fmt::format("unexpected argument {}", k));
      }
      if (i + 1 < ac && !std::string_view{av[i + 1]}.starts_with("--")) {
        kv_[std::string{k.substr(2)}] = av[i + 1];
        ++i;
      } else {
        kv_[std::string{k.substr(2)}] = "1";
      }
    }
  }
  bool has(std::string const& k) const {
    if (kv_.contains(k)) {
      used_[k] = true;
      return true;
    }
    return false;
  }
  std::string str(std::string const& k) const {
    auto const it = kv_.find(k);
    if (it == end(kv_)) {
      throw std::runtime_error(fmt::format("missing --{}", k));
    }
    used_[k] = true;
    return it->second;
  }
  std::string str(std::string const& k, std::string const& def) const {
    return has(k) ? str(k) : def;
  }
  double num(std::string const& k) const { return std::stod(str(k)); }
  double num(std::string const& k, double const def) const {
    return has(k) ? num(k) : def;
  }
  std::int64_t integer(std::string const& k) const {
    return std::stoll(str(k));
  }
  std::int64_t integer(std::string const& k, std::int64_t const def) const {
    return has(k) ? integer(k) : def;
  }
  void check_all_used() const {
    for (auto const& [k, v] : kv_) {
      if (!used_.contains(k)) {
        throw std::runtime_error(fmt::format("unknown option --{}", k));
      }
    }
  }
  std::map<std::string, std::string> kv_;
  mutable std::map<std::string, bool> used_;
};

// ---------------------------------------------------------------- csv
std::vector<std::string_view> split(std::string_view s, char const sep) {
  auto out = std::vector<std::string_view>{};
  auto start = std::size_t{0U};
  while (true) {
    auto const p = s.find(sep, start);
    if (p == std::string_view::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, p - start));
    start = p + 1U;
  }
  return out;
}

template <typename T>
T parse_num(std::string_view s) {
  auto v = T{};
  if constexpr (std::is_floating_point_v<T>) {
    v = static_cast<T>(std::stod(std::string{s}));
  } else {
    auto const [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
      throw std::runtime_error(fmt::format("bad number '{}'", s));
    }
  }
  return v;
}

struct csv_table {
  std::vector<std::string> header_;
  std::vector<std::vector<std::string>> rows_;
  std::size_t col(std::string_view name) const {
    auto const it = std::find(begin(header_), end(header_), name);
    if (it == end(header_)) {
      throw std::runtime_error(fmt::format("column {} missing", name));
    }
    return static_cast<std::size_t>(it - begin(header_));
  }
};

csv_table read_csv(fs::path const& p) {
  auto in = std::ifstream{p};
  if (!in) {
    throw std::runtime_error(fmt::format("cannot open {}", p.string()));
  }
  auto t = csv_table{};
  auto line = std::string{};
  auto first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    auto parts = split(line, ',');
    auto row = std::vector<std::string>(parts.begin(), parts.end());
    if (first) {
      t.header_ = std::move(row);
      first = false;
    } else {
      t.rows_.push_back(std::move(row));
    }
  }
  return t;
}

// Points: CSV with lat,lon columns; row order defines the point index.
std::vector<geo::latlng> read_points(fs::path const& p) {
  auto const t = read_csv(p);
  auto const lat = t.col("lat");
  auto const lon = t.col("lon");
  auto out = std::vector<geo::latlng>{};
  out.reserve(t.rows_.size());
  for (auto const& r : t.rows_) {
    out.emplace_back(parse_num<double>(r.at(lat)), parse_num<double>(r.at(lon)));
  }
  return out;
}

// Connector CSV: point,stop,minutes (point index, nigiri location index).
struct connector {
  std::uint32_t point_;
  std::uint32_t stop_;
  std::uint16_t minutes_;
};

std::vector<connector> read_connectors(fs::path const& p) {
  auto in = std::ifstream{p};
  if (!in) {
    throw std::runtime_error(fmt::format("cannot open {}", p.string()));
  }
  auto line = std::string{};
  std::getline(in, line);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != "point,stop,minutes") {
    throw std::runtime_error(
        fmt::format("{}: expected header point,stop,minutes", p.string()));
  }
  auto out = std::vector<connector>{};
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    auto const parts = split(line, ',');
    if (parts.size() != 3U) {
      throw std::runtime_error(fmt::format("bad connector line '{}'", line));
    }
    out.push_back({parse_num<std::uint32_t>(parts[0]),
                   parse_num<std::uint32_t>(parts[1]),
                   parse_num<std::uint16_t>(parts[2])});
  }
  return out;
}

// CSR index: for each key, list of (value, minutes).
struct csr {
  std::vector<std::uint32_t> begin_;
  std::vector<std::uint32_t> value_;
  std::vector<std::uint16_t> minutes_;

  static csr build(std::vector<connector> const& c,
                   std::size_t const n_keys,
                   bool const key_is_stop) {
    auto r = csr{};
    r.begin_.assign(n_keys + 1U, 0U);
    for (auto const& x : c) {
      auto const k = key_is_stop ? x.stop_ : x.point_;
      if (k >= n_keys) {
        throw std::runtime_error(fmt::format("connector key {} >= {}", k, n_keys));
      }
      ++r.begin_[k + 1U];
    }
    for (auto i = 0U; i != n_keys; ++i) {
      r.begin_[i + 1U] += r.begin_[i];
    }
    r.value_.resize(c.size());
    r.minutes_.resize(c.size());
    auto pos = std::vector<std::uint32_t>(r.begin_.begin(), r.begin_.end() - 1);
    for (auto const& x : c) {
      auto const k = key_is_stop ? x.stop_ : x.point_;
      auto const v = key_is_stop ? x.point_ : x.stop_;
      r.value_[pos[k]] = v;
      r.minutes_[pos[k]] = x.minutes_;
      ++pos[k];
    }
    return r;
  }
};

// ---------------------------------------------------------------- data
struct loaded {
  explicit loaded(fs::path const& data_path)
      : c_{motis::config::read(data_path / "config.yml")} {
    auto const t = clock_t_::now();
    d_ = std::make_unique<motis::data>(data_path, c_);
    if (!d_->tt_) {
      throw std::runtime_error("timetable required");
    }
    load_ms_ = ms_since(t);
  }
  motis::config c_;
  std::unique_ptr<motis::data> d_;
  double load_ms_{};
};

motis::osr_parameters walk_params(args const& a) {
  auto p = motis::osr_parameters{};
  p.pedestrian_speed_ = static_cast<float>(
      a.num("pedestrian-speed", motis::osr_parameters::kFootSpeed));
  return p;
}

// ---------------------------------------------------------------- stops
int cmd_stops(args const& a) {
  auto const l = loaded{a.str("data")};
  auto const& tt = *l.d_->tt_;
  auto out = std::ofstream{a.str("out")};
  out << "stop,id,src,lat,lon,parent,type,transfer_time,traffic\n";
  for (auto i = 0U; i != tt.n_locations(); ++i) {
    auto const x = n::location_idx_t{i};
    auto const parent = tt.locations_.parents_[x];
    out << i << ',' << std::string_view{tt.locations_.ids_[x].view()} << ','
        << to_idx(tt.locations_.src_[x]) << ','
        << fmt::format("{:.7f},{:.7f}", tt.locations_.coordinates_[x].lat_,
                       tt.locations_.coordinates_[x].lng_)
        << ','
        << (parent == n::location_idx_t::invalid()
                ? std::string{"-1"}
                : std::to_string(to_idx(parent)))
        << ',' << static_cast<int>(tt.locations_.types_[x]) << ','
        << static_cast<int>(tt.locations_.transfer_time_[x].count()) << ','
        << (n::any_route_at(tt, x) ? 1 : 0) << '\n';
  }
  if (a.has("footpaths")) {
    auto fp = std::ofstream{a.str("footpaths")};
    fp << "from,to,minutes\n";
    for (auto i = 0U; i != tt.n_locations(); ++i) {
      auto const x = n::location_idx_t{i};
      for (auto const& f : tt.locations_.footpaths_out_[0][x]) {
        fp << i << ',' << to_idx(f.target()) << ',' << f.duration().count()
           << '\n';
      }
    }
  }
  a.check_all_used();
  fmt::println(stderr, "stops: n_locations={} load_ms={:.1f}", tt.n_locations(),
               l.load_ms_);
  return 0;
}

// ------------------------------------------------- OSR connectors (stock)
int cmd_connectors(args const& a) {
  auto const l = loaded{a.str("data")};
  auto r = utl::init_from<motis::ep::routing>(*l.d_);
  if (!r.has_value() || !r->is_osr_loaded()) {
    throw std::runtime_error("connectors require street routing (OSM) data");
  }
  auto const points = read_points(a.str("points"));
  auto const dir_str = a.str("dir");
  if (dir_str != "forward" && dir_str != "backward") {
    throw std::runtime_error("--dir must be forward|backward");
  }
  auto const dir = dir_str == "forward" ? osr::direction::kForward
                                        : osr::direction::kBackward;
  auto const max = std::chrono::seconds{a.integer("max-seconds")};
  auto const max_matching = a.num("max-matching-distance", 25.0);
  auto const params = walk_params(a);
  auto const n_threads = static_cast<unsigned>(a.integer("threads", 1));
  auto const out_path = a.str("out");
  a.check_all_used();

  auto const t = clock_t_::now();
  auto results = std::vector<std::vector<n::routing::offset>>(points.size());
  auto next = std::atomic_size_t{0U};
  auto worker = [&]() {
    auto gbfs_rd =
        motis::gbfs::gbfs_routing_data{r->w_, r->l_, std::atomic_load(&r->gbfs_)};
    auto stats = std::map<std::string, std::uint64_t>{};
    for (auto i = next++; i < points.size(); i = next++) {
      auto const p = motis::place_t{osr::location{points[i], osr::kNoLevel}};
      results[i] = r->get_offsets(
          nullptr, p, dir, {motis::api::ModeEnum::WALK}, motis::rental_options{},
          params, motis::api::PedestrianProfileEnum::FOOT,
          motis::api::ElevationCostsEnum::NONE, max, max_matching, gbfs_rd,
          stats);
    }
  };
  auto threads = std::vector<std::thread>{};
  for (auto i = 0U; i != std::max(1U, n_threads); ++i) {
    threads.emplace_back(worker);
  }
  for (auto& th : threads) {
    th.join();
  }
  auto out = std::ofstream{out_path};
  out << "point,stop,minutes\n";
  auto n = std::size_t{0U};
  for (auto i = 0U; i != points.size(); ++i) {
    auto sorted = results[i];
    std::sort(begin(sorted), end(sorted), [](auto const& x, auto const& y) {
      return std::pair{to_idx(x.target()), x.duration()} <
             std::pair{to_idx(y.target()), y.duration()};
    });
    auto last = std::numeric_limits<std::uint32_t>::max();
    for (auto const& o : sorted) {
      if (to_idx(o.target()) == last) {
        continue;  // keep the fastest duplicate
      }
      last = to_idx(o.target());
      out << i << ',' << to_idx(o.target()) << ',' << o.duration().count()
          << '\n';
      ++n;
    }
  }
  fmt::println(stderr,
               "connectors: dir={} points={} connectors={} load_ms={:.1f} "
               "compute_ms={:.1f} peak_rss_kb={}",
               dir_str, points.size(), n, l.load_ms_, ms_since(t),
               peak_rss_kb());
  return 0;
}

// --------------------------------------------- OSR direct walk (stock fn)
int cmd_direct(args const& a) {
  auto const l = loaded{a.str("data")};
  auto r = utl::init_from<motis::ep::routing>(*l.d_);
  if (!r.has_value() || !r->is_osr_loaded()) {
    throw std::runtime_error("direct requires street routing (OSM) data");
  }
  auto const origins = read_points(a.str("origins"));
  auto const dests = read_points(a.str("dests"));
  auto const max_seconds = a.num("max-seconds");
  auto const radius = a.num("radius-m");
  auto const max_matching = a.num("max-matching-distance", 25.0);
  auto const params = walk_params(a);
  auto const n_threads = static_cast<unsigned>(a.integer("threads", 1));
  auto const out_path = a.str("out");
  a.check_all_used();

  auto const t = clock_t_::now();
  auto results =
      std::vector<std::vector<std::pair<std::uint32_t, std::uint16_t>>>(
          origins.size());
  auto next = std::atomic_size_t{0U};
  auto worker = [&]() {
    for (auto i = next++; i < origins.size(); i = next++) {
      auto cand = std::vector<std::uint32_t>{};
      auto many = std::vector<osr::location>{};
      for (auto j = 0U; j != dests.size(); ++j) {
        if (geo::distance(origins[i], dests[j]) <= radius) {
          cand.push_back(j);
          many.push_back(osr::location{dests[j], osr::kNoLevel});
        }
      }
      if (many.empty()) {
        continue;
      }
      auto const res = motis::ep::one_to_many_direct(
          r->config_, *r->w_, *r->l_, motis::api::ModeEnum::WALK,
          osr::location{origins[i], osr::kNoLevel}, many, max_seconds,
          max_matching, osr::direction::kForward, params,
          motis::api::PedestrianProfileEnum::FOOT,
          motis::api::ElevationCostsEnum::NONE, r->elevations_, false);
      for (auto k = 0U; k != res.size(); ++k) {
        if (res[k].duration_.has_value()) {
          results[i].emplace_back(
              cand[k], static_cast<std::uint16_t>(
                           std::ceil(*res[k].duration_ / 60.0)));
        }
      }
    }
  };
  auto threads = std::vector<std::thread>{};
  for (auto i = 0U; i != std::max(1U, n_threads); ++i) {
    threads.emplace_back(worker);
  }
  for (auto& th : threads) {
    th.join();
  }
  auto out = std::ofstream{out_path};
  out << "point,stop,minutes\n";  // "stop" column holds the destination index
  auto n = std::size_t{0U};
  for (auto i = 0U; i != origins.size(); ++i) {
    for (auto const& [d, m] : results[i]) {
      out << i << ',' << d << ',' << m << '\n';
      ++n;
    }
  }
  fmt::println(stderr, "direct: origins={} dests={} pairs={} compute_ms={:.1f}",
               origins.size(), dests.size(), n, ms_since(t));
  return 0;
}

// ---------------------------------------------------------------- route
struct stream_header {
  char magic_[8]{'V', 'R', 'T', 'T', 'M', 'A', 'T', 'X'};
  std::uint32_t version_{kFormatVersion};
  std::uint32_t n_origins_total_{};
  std::uint32_t n_dests_{};
  std::uint32_t n_departures_{};
  std::uint32_t origin_from_{};
  std::uint32_t origin_to_{};
  std::uint16_t max_travel_minutes_{};
  std::uint8_t flags_{};  // bit0 transfers, bit1 egress-stop, bit2 u16 travel times
  std::uint8_t target_{};  // 0: destinations, 1: stops
  std::uint32_t reserved_[5]{};
};
static_assert(sizeof(stream_header) == 56);

int cmd_route(args const& a) {
  auto const l = loaded{a.str("data")};
  auto const& tt = *l.d_->tt_;
  auto const target_stops = a.str("target", "dests") == "stops";
  auto const n_origins = static_cast<std::uint32_t>(a.integer("n-origins"));
  auto const n_dests = target_stops
                           ? static_cast<std::uint32_t>(tt.n_locations())
                           : static_cast<std::uint32_t>(a.integer("n-dests"));
  auto const origin_from = static_cast<std::uint32_t>(a.integer("origin-from", 0));
  auto const origin_to = static_cast<std::uint32_t>(
      std::min<std::int64_t>(a.integer("origin-to", n_origins), n_origins));
  // Optional explicit origin subset (comma-separated indices), else the range.
  auto origin_list = std::vector<std::uint32_t>{};
  if (a.has("origin-list")) {
    for (auto const x : split(a.str("origin-list"), ',')) {
      auto const v = parse_num<std::uint32_t>(x);
      if (v >= n_origins) {
        throw std::runtime_error("origin-list entry out of range");
      }
      origin_list.push_back(v);
    }
  } else {
    for (auto o = origin_from; o < origin_to; ++o) {
      origin_list.push_back(o);
    }
  }
  auto const dep_start = a.integer("dep-start-unix-minutes");
  auto const dep_count = static_cast<std::uint32_t>(a.integer("dep-count", 1));
  auto const dep_step = a.integer("dep-step-minutes", 1);
  auto const max_travel_arg = a.integer("max-travel-minutes");
  if (max_travel_arg < 0 || max_travel_arg > static_cast<std::int64_t>(kMaxCap)) {
    throw std::runtime_error("max-travel-minutes must be in [0, 1440]");
  }
  auto const max_travel = static_cast<std::uint16_t>(max_travel_arg);
  auto const tt16 = max_travel > 253U;  // lossless u16 records above the u8 code range
  auto const max_transfers =
      static_cast<std::uint8_t>(a.integer("max-transfers", n::routing::kMaxTransfers));
  auto const with_transfers = a.has("with-transfers");
  auto const with_via = a.has("with-egress-stop");
  auto const n_threads = static_cast<unsigned>(a.integer("threads", 1));
  auto const out_path = a.str("out");
  auto const timing_path = a.str("timing-json", "");
  auto const discard = a.has("discard");  // timing-only: compute, write no records

  auto const t_conn = clock_t_::now();
  auto const access =
      csr::build(read_connectors(a.str("access")), n_origins, false);
  auto egress = csr{};
  auto direct = csr{};
  if (!target_stops) {
    egress = csr::build(read_connectors(a.str("egress")), tt.n_locations(), true);
    if (a.has("direct")) {
      direct = csr::build(read_connectors(a.str("direct")), n_origins, false);
    } else {
      direct.begin_.assign(n_origins + 1U, 0U);
    }
  }
  a.check_all_used();
  auto const conn_ms = ms_since(t_conn);
  for (auto const v : access.value_) {
    if (v >= tt.n_locations()) {
      throw std::runtime_error("access connector stop out of range");
    }
  }
  for (auto const v : egress.value_) {
    if (v >= n_dests) {
      throw std::runtime_error("egress connector destination out of range");
    }
  }
  for (auto const v : direct.value_) {
    if (v >= n_dests) {
      throw std::runtime_error("direct connector destination out of range");
    }
  }

  auto const clasz_mask =
      motis::to_clasz_mask(std::vector{motis::api::ModeEnum::TRANSIT});

  auto out = std::ofstream{out_path, std::ios::binary};
  auto h = stream_header{};
  h.n_origins_total_ = n_origins;
  h.n_dests_ = n_dests;
  h.n_departures_ = dep_count;
  h.origin_from_ = origin_from;
  h.origin_to_ = origin_to;
  h.max_travel_minutes_ = max_travel;
  h.flags_ = static_cast<std::uint8_t>((with_transfers ? 1U : 0U) |
                                       (with_via ? 2U : 0U) | (tt16 ? 4U : 0U));
  h.target_ = target_stops ? 1U : 0U;
  out.write(reinterpret_cast<char const*>(&h), sizeof(h));
  for (auto i = 0U; i != dep_count; ++i) {
    auto const t = static_cast<std::int64_t>(dep_start + i * dep_step);
    out.write(reinterpret_cast<char const*>(&t), sizeof(t));
  }

  auto write_mutex = std::mutex{};
  auto next = std::atomic_size_t{0U};
  auto n_queries = std::atomic_uint64_t{0U};
  auto kernel_ns = std::atomic_uint64_t{0U};
  auto reduce_ns = std::atomic_uint64_t{0U};
  auto write_ns = std::atomic_uint64_t{0U};
  auto const t_route = clock_t_::now();

  auto worker = [&]() {
    auto const block = static_cast<std::size_t>(dep_count) * n_dests;
    auto tt_buf = std::vector<std::uint16_t>(block);
    auto tt8_buf = std::vector<std::uint8_t>(tt16 ? 0U : block);
    auto tr_buf = std::vector<std::uint8_t>(with_transfers ? block : 0U);
    auto via_buf = std::vector<std::uint32_t>(with_via ? block : 0U);
    for (auto oi = next++; oi < origin_list.size(); oi = next++) {
      auto const o = origin_list[oi];
      std::fill(begin(tt_buf), end(tt_buf), kUnreachable16);
      std::fill(begin(tr_buf), end(tr_buf), kUnreachable);
      std::fill(begin(via_buf), end(via_buf), kNoStop);
      auto starts = std::vector<n::routing::offset>{};
      for (auto j = access.begin_[o]; j != access.begin_[o + 1U]; ++j) {
        starts.emplace_back(n::location_idx_t{access.value_[j]},
                            n::duration_t{static_cast<n::duration_t::rep>(access.minutes_[j])}, 0U);
      }
      for (auto di = 0U; di != dep_count; ++di) {
        auto* row_tt = tt_buf.data() + static_cast<std::size_t>(di) * n_dests;
        auto* row_tr = with_transfers
                           ? tr_buf.data() + static_cast<std::size_t>(di) * n_dests
                           : nullptr;
        auto* row_via = with_via
                            ? via_buf.data() + static_cast<std::size_t>(di) * n_dests
                            : nullptr;
        // Departure-independent direct walk (destinations only).
        if (!target_stops) {
          for (auto j = direct.begin_[o]; j != direct.begin_[o + 1U]; ++j) {
            auto const d = direct.value_[j];
            auto const m = direct.minutes_[j];
            if (m <= max_travel && m < row_tt[d]) {
              row_tt[d] = m;
              if (row_tr) row_tr[d] = kDirectWalk;
              if (row_via) row_via[d] = kNoStop;
            }
          }
        }
        if (starts.empty()) {
          continue;
        }
        auto const start_time = n::unixtime_t{
            n::i32_minutes{static_cast<std::int32_t>(dep_start + di * dep_step)}};
        auto q = n::routing::query{
            .start_time_ = start_time,
            .start_match_mode_ = n::routing::location_match_mode::kIntermodal,
            .start_ = starts,
            .max_transfers_ = max_transfers,
            .max_travel_time_ = n::duration_t{static_cast<n::duration_t::rep>(max_travel)},
            .prf_idx_ = 0U,
            .allowed_claszes_ = clasz_mask,
        };
        auto const tk = clock_t_::now();
        auto const state = n::routing::one_to_all<n::direction::kForward>(
            tt, nullptr, q);
        auto const tr = clock_t_::now();
        kernel_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tr - tk).count());
        ++n_queries;

        if (target_stops) {
          // Kernel output: fastest arrival at every location including round 0
          // (same as the stock /api/v6/one-to-all endpoint).
          for (auto s = 0U; s != tt.n_locations(); ++s) {
            auto const f = n::routing::get_fastest_one_to_all_offsets(
                tt, state, n::direction::kForward, n::location_idx_t{s},
                start_time, max_transfers);
            if (f.k_ != std::numeric_limits<std::uint8_t>::max() &&
                f.duration_ >= 0 && f.duration_ <= max_travel) {
              row_tt[s] = static_cast<std::uint16_t>(f.duration_);
              if (row_tr) row_tr[s] = f.k_;
            }
          }
        } else {
          // Egress exactly as stock transit_durations: rounds k >= 1 only,
          // arrival at the stop (vehicle arrival, no final transfer buffer)
          // plus the egress connector; transfers = k - 1.
          for (auto s = 0U; s != tt.n_locations(); ++s) {
            if (egress.begin_[s] == egress.begin_[s + 1U]) {
              continue;
            }
            auto best = std::numeric_limits<std::int64_t>::max();
            auto best_k = std::uint8_t{0U};
            n::routing::for_each_one_to_all_round_time(
                tt, state, n::direction::kForward, n::location_idx_t{s},
                start_time, max_transfers,
                [&](std::uint8_t const k, n::duration_t const d) {
                  if (k != 0U && d.count() < best) {
                    best = d.count();
                    best_k = k;
                  }
                });
            if (best == std::numeric_limits<std::int64_t>::max()) {
              continue;
            }
            for (auto j = egress.begin_[s]; j != egress.begin_[s + 1U]; ++j) {
              auto const d = egress.value_[j];
              auto const total = best + egress.minutes_[j];
              auto const transfers = static_cast<std::uint8_t>(best_k - 1U);
              if (total > max_travel) {
                continue;
              }
              // Lexicographic (travel time, transfers); a tie with a direct
              // walk keeps the walk (stock reports both, the reduction takes
              // the walk on ties).
              auto const better =
                  total < row_tt[d] ||
                  (row_tr != nullptr && total == row_tt[d] &&
                   row_tr[d] != kDirectWalk && transfers < row_tr[d]);
              if (better) {
                row_tt[d] = static_cast<std::uint16_t>(total);
                if (row_tr) row_tr[d] = transfers;
                if (row_via) row_via[d] = s;
              }
            }
          }
        }
        reduce_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - tr)
                .count());
      }
      auto const tw = clock_t_::now();
      if (!discard) {
        auto const lock = std::lock_guard{write_mutex};
        out.write(reinterpret_cast<char const*>(&o), sizeof(o));
        if (tt16) {
          out.write(reinterpret_cast<char const*>(tt_buf.data()),
                    static_cast<std::streamsize>(tt_buf.size() * 2U));
        } else {
          for (auto i = 0U; i != tt_buf.size(); ++i) {
            tt8_buf[i] = tt_buf[i] == kUnreachable16
                             ? kUnreachable
                             : static_cast<std::uint8_t>(tt_buf[i]);
          }
          out.write(reinterpret_cast<char const*>(tt8_buf.data()),
                    static_cast<std::streamsize>(tt8_buf.size()));
        }
        if (with_transfers) {
          out.write(reinterpret_cast<char const*>(tr_buf.data()),
                    static_cast<std::streamsize>(tr_buf.size()));
        }
        if (with_via) {
          out.write(reinterpret_cast<char const*>(via_buf.data()),
                    static_cast<std::streamsize>(via_buf.size() * 4U));
        }
      }
      write_ns += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - tw)
              .count());
    }
  };
  auto threads = std::vector<std::thread>{};
  for (auto i = 0U; i != std::max(1U, n_threads); ++i) {
    threads.emplace_back(worker);
  }
  for (auto& th : threads) {
    th.join();
  }
  out.close();
  if (!out) {
    throw std::runtime_error("write failed");
  }
  auto const route_ms = ms_since(t_route);
  auto const json = fmt::format(
      "{{\"tool\":\"motis-accessibility\",\"format_version\":{},"
      "\"target\":\"{}\",\"origins\":[{},{}],\"n_dests\":{},\"n_departures\":{},"
      "\"threads\":{},\"discard\":{},\"max_travel_minutes\":{},\"queries\":{},\"load_ms\":{:.3f},\"connectors_ms\":{:.3f},"
      "\"route_wall_ms\":{:.3f},\"kernel_thread_ms\":{:.3f},"
      "\"reduce_thread_ms\":{:.3f},\"write_thread_ms\":{:.3f},"
      "\"peak_rss_kb\":{},\"n_locations\":{}}}",
      kFormatVersion, target_stops ? "stops" : "dests", origin_from, origin_to,
      n_dests, dep_count, n_threads, discard, max_travel, n_queries.load(), l.load_ms_, conn_ms,
      route_ms, kernel_ns.load() / 1e6, reduce_ns.load() / 1e6,
      write_ns.load() / 1e6, peak_rss_kb(), tt.n_locations());
  if (!timing_path.empty()) {
    auto tj = std::ofstream{timing_path};
    tj << json << '\n';
  }
  fmt::println(stderr, "{}", json);
  return 0;
}

}  // namespace

int main(int ac, char** av) {
  if (ac < 2) {
    fmt::println(stderr,
                 "usage: motis-accessibility <stops|connectors|direct|route> "
                 "--data DIR [options]\n(see tools/accessibility/README.md)");
    return 1;
  }
  try {
    auto const cmd = std::string_view{av[1]};
    auto const a = args{ac, av};
    if (cmd == "stops") {
      return cmd_stops(a);
    } else if (cmd == "connectors") {
      return cmd_connectors(a);
    } else if (cmd == "direct") {
      return cmd_direct(a);
    } else if (cmd == "route") {
      return cmd_route(a);
    }
    fmt::println(stderr, "unknown command {}", cmd);
    return 1;
  } catch (std::exception const& e) {
    fmt::println(stderr, "error: {}", e.what());
    return 2;
  }
}
