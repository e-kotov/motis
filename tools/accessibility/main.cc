// motis-accessibility: offline many-origin accessibility batch on top of stock
// MOTIS/Nigiri. The transit kernel is the unmodified nigiri one_to_all RAPTOR;
// access/egress connectors are either read from a file (e.g. straight-line
// connectors computed by the Rust CLI) or computed once per point with the
// same motis::ep::routing::get_offsets call the stock one-to-many endpoint uses
// per request. Results are streamed as a compact binary travel-time matrix
// (see tools/accessibility/README.md for the format and semantics).

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include "fmt/core.h"
#include "fmt/ranges.h"

#include "cista/hash.h"

#include "utl/init_from.h"

#include "geo/latlng.h"

#include "osr/location.h"
#include "osr/routing/profile.h"

#include "nigiri/for_each_meta.h"
#include "nigiri/location_routes.h"
#include "nigiri/routing/limits.h"
#include "nigiri/routing/one_to_all.h"
#include "nigiri/routing/query.h"
#include "nigiri/routing/raptor/raptor.h"
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
// --stream-encoding sparse: version 2, per record a dense or a bitmap payload.
constexpr auto kSparseFormatVersion = std::uint32_t{2U};
constexpr auto kRecordDense = std::uint8_t{0U};
constexpr auto kRecordBitmap = std::uint8_t{1U};
constexpr auto kUnreachable = std::uint8_t{255U};
constexpr auto kUnreachable16 = std::uint16_t{0xFFFFU};
constexpr auto kMaxCap = 1440U;  // u16 path; one day
constexpr auto kDirectWalk = std::uint8_t{254U};
constexpr auto kWalkViaStop =
    std::uint8_t{253U};  // round-0 egress (no vehicle)
constexpr auto kNoStop = std::uint32_t{0xFFFFFFFFU};

using clock_t_ = std::chrono::steady_clock;

double ms_since(clock_t_::time_point const t) {
  return std::chrono::duration<double, std::milli>(clock_t_::now() - t).count();
}

// CPUs this process may run on (the job's cpuset), ascending.
std::vector<int> allowed_cpus() {
  auto set = cpu_set_t{};
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_getaffinity failed");
  }
  auto cpus = std::vector<int>{};
  for (auto c = 0; c != CPU_SETSIZE; ++c) {
    if (CPU_ISSET(c, &set)) {
      cpus.push_back(c);
    }
  }
  return cpus;
}

// Pins the calling thread to one CPU (--pin-threads: worker i -> i-th allowed
// CPU).
void pin_current_thread(int const cpu) {
  auto set = cpu_set_t{};
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    throw std::runtime_error(fmt::format("cannot pin thread to cpu {}", cpu));
  }
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
  std::int64_t checked(std::string const& k,
                       std::int64_t const def,
                       std::int64_t const lo,
                       std::int64_t const hi) const {
    auto const v = integer(k, def);
    if (v < lo || v > hi) {
      throw std::runtime_error(
          fmt::format("--{} must be in [{}, {}], got {}", k, lo, hi, v));
    }
    return v;
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

std::vector<std::string> parse_csv_line(std::string_view line) {
  auto row = std::vector<std::string>{};
  while (true) {
    auto& field = row.emplace_back();
    if (line.starts_with('"')) {
      line.remove_prefix(1U);
      while (true) {
        auto const quote = line.find('"');
        if (quote == std::string_view::npos) {
          throw std::runtime_error("unclosed quoted CSV field");
        }
        field.append(line.substr(0U, quote));
        line.remove_prefix(quote + 1U);
        if (!line.starts_with('"')) {
          break;
        }
        field.push_back('"');
        line.remove_prefix(1U);
      }
    } else {
      auto const field_view = line.substr(0U, line.find(','));
      if (field_view.find('"') != std::string_view::npos) {
        throw std::runtime_error("quote in unquoted CSV field");
      }
      field = field_view;
      line.remove_prefix(field_view.size());
    }
    if (line.empty()) {
      return row;
    }
    if (!line.starts_with(',')) {
      throw std::runtime_error("expected comma after quoted CSV field");
    }
    line.remove_prefix(1U);
  }
}

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
    auto row = parse_csv_line(line);
    if (first) {
      t.header_ = std::move(row);
      first = false;
    } else {
      t.rows_.push_back(std::move(row));
    }
  }
  return t;
}

struct opportunity_data {
  std::vector<std::vector<double>> values_;
  std::vector<std::string> names_;
};

double parse_opportunity_count(std::string const& token) {
  auto const consume_sign = [](std::string_view& s) {
    if (s.starts_with('+') || s.starts_with('-')) {
      s.remove_prefix(1U);
    }
  };
  auto const consume_digits = [](std::string_view& s) {
    auto const n = std::min(s.find_first_not_of("0123456789"), s.size());
    s.remove_prefix(n);
    return n != 0U;
  };
  // ASCII decimal syntax with an optional sign, fraction and exponent.
  auto rest = std::string_view{token};
  consume_sign(rest);
  auto has_digits = consume_digits(rest);
  if (rest.starts_with('.')) {
    rest.remove_prefix(1U);
    has_digits = consume_digits(rest) || has_digits;
  }
  if (rest.starts_with('e') || rest.starts_with('E')) {
    rest.remove_prefix(1U);
    consume_sign(rest);
    has_digits = consume_digits(rest) && has_digits;
  }
  if (!has_digits || !rest.empty()) {
    throw std::runtime_error(fmt::format("bad number '{}'", token));
  }
  auto value = 0.0;
  try {
    value = std::stod(token);
  } catch (std::out_of_range const&) {
    throw std::runtime_error(
        fmt::format("opportunity count out of range '{}'", token));
  }
  if (!std::isfinite(value) || value < 0.0) {
    throw std::runtime_error(
        "opportunity counts must be finite and non-negative");
  }
  return value == 0.0 ? 0.0 : value;
}

opportunity_data read_opportunities(args const& a, std::uint32_t n_dests) {
  auto const cols = a.str("opportunity-columns", "");
  if (cols.empty()) {
    throw std::runtime_error("missing --opportunity-columns");
  }
  auto const table = read_csv(a.str("opportunities"));
  if (table.rows_.size() != n_dests) {
    throw std::runtime_error(
        fmt::format("--opportunities: {} rows, expected {} destinations",
                    table.rows_.size(), n_dests));
  }
  for (auto i = std::size_t{0U}; i < table.rows_.size(); ++i) {
    auto const width = table.rows_[i].size();
    if (width != table.header_.size()) {
      throw std::runtime_error(
          fmt::format("--opportunities: row {}: expected {} columns, got {}",
                      i + 1U, table.header_.size(), width));
    }
  }
  auto out = opportunity_data{};
  for (auto const name : split(cols, ',')) {
    auto const col = table.col(name);
    auto& values = out.values_.emplace_back();
    values.reserve(n_dests);
    for (auto const& row : table.rows_) {
      values.push_back(parse_opportunity_count(row[col]));
    }
    out.names_.emplace_back(name);
  }
  return out;
}

// Points: CSV with lat,lon columns; row order defines the point index.
std::vector<geo::latlng> read_points(fs::path const& p) {
  auto const t = read_csv(p);
  auto const lat = t.col("lat");
  auto const lon = t.col("lon");
  auto out = std::vector<geo::latlng>{};
  out.reserve(t.rows_.size());
  for (auto const& r : t.rows_) {
    out.emplace_back(parse_num<double>(r.at(lat)),
                     parse_num<double>(r.at(lon)));
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
        throw std::runtime_error(
            fmt::format("connector key {} >= {}", k, n_keys));
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

  // Orders each key's entries by minutes, keeping their previous order within
  // one minute (a stable counting sort, linear in the entries). Within one key
  // the order of entries does not change any result of the egress loop: all
  // entries of a stop share its arrival and transfer count, so each
  // destination keeps the minimum over that stop's entries either way. Sorted
  // entries let the loop stop at the first one beyond the travel-time cap.
  void sort_by_minutes() {
    auto count = std::vector<std::uint32_t>{};
    auto tmp_v = std::vector<std::uint32_t>{};
    auto tmp_m = std::vector<std::uint16_t>{};
    for (auto k = std::size_t{0U}; k + 1U < begin_.size(); ++k) {
      auto const b = begin_[k];
      auto const e = begin_[k + 1U];
      if (std::is_sorted(begin(minutes_) + b, begin(minutes_) + e)) {
        continue;
      }
      auto const max_m = *std::max_element(begin(minutes_) + b,
                                           begin(minutes_) + e);
      count.assign(static_cast<std::size_t>(max_m) + 2U, 0U);
      for (auto j = b; j != e; ++j) {
        ++count[minutes_[j] + 1U];
      }
      for (auto m = std::size_t{1U}; m < count.size(); ++m) {
        count[m] += count[m - 1U];
      }
      tmp_v.resize(e - b);
      tmp_m.resize(e - b);
      for (auto j = b; j != e; ++j) {
        auto const pos = count[minutes_[j]]++;
        tmp_v[pos] = value_[j];
        tmp_m[pos] = minutes_[j];
      }
      std::copy(begin(tmp_v), end(tmp_v), begin(value_) + b);
      std::copy(begin(tmp_m), end(tmp_m), begin(minutes_) + b);
    }
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

// --------------------------------------- numeric timetable export, binary
// `export-tt --format bin`: the same tables as the CSV export in one file,
// `timetable.vrtt`, for zero-copy loading (mmap). Little-endian throughout.
//
//   header (64 bytes)
//     char[8]  magic "VRTTEXP1"
//     u32      version (1)
//     u32      n_sections
//     i64      from_unix_minutes, to_unix_minutes
//     i64      internal_interval_days_from, internal_interval_days_to (days
//     since epoch) u64      data_offset (first byte after the section
//     directory) u64      checksum: FNV-1a 64 over bytes [data_offset, end of
//     file)
//   section directory: n_sections x 40 bytes, in file order
//     char[16] name (NUL padded), u32 elem_size, u32 reserved (0),
//     u64 offset (absolute, 8-byte aligned), u64 count (elements)
//   data: each section is a plain array of `count` fixed-width elements at
//   `offset`; gaps between sections are zero padding (covered by the checksum).
//   The section names and element types are listed in the README.
namespace vrtt {

struct header {
  char magic_[8]{'V', 'R', 'T', 'T', 'E', 'X', 'P', '1'};
  std::uint32_t version_{1U};
  std::uint32_t n_sections_{};
  std::int64_t from_{}, to_{};
  std::int64_t days_from_{}, days_to_{};
  std::uint64_t data_offset_{};
  std::uint64_t checksum_{};
};
static_assert(sizeof(header) == 64);

struct section_entry {
  char name_[16]{};
  std::uint32_t elem_size_{};
  std::uint32_t reserved_{};
  std::uint64_t offset_{};
  std::uint64_t count_{};
};
static_assert(sizeof(section_entry) == 40);

struct section {
  std::string name_;
  std::uint32_t elem_size_;
  std::vector<std::uint8_t> bytes_;
};

template <typename T>
section make_section(std::string name, std::vector<T> const& v) {
  static_assert(std::is_trivially_copyable_v<T>);
  auto s = section{std::move(name), static_cast<std::uint32_t>(sizeof(T)), {}};
  s.bytes_.resize(v.size() * sizeof(T));
  if (!v.empty()) {
    std::memcpy(s.bytes_.data(), v.data(), s.bytes_.size());
  }
  return s;
}

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

std::uint64_t fnv1a(std::uint64_t h,
                    std::uint8_t const* p,
                    std::size_t const n) {
  for (auto i = std::size_t{0U}; i != n; ++i) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

}  // namespace vrtt

std::uint64_t export_tt_bin(n::timetable const& tt,
                            std::int64_t const from,
                            std::int64_t const to,
                            fs::path const& file,
                            std::map<std::string, std::uint64_t>& counts) {
  static_assert(std::endian::native == std::endian::little);
  // locations
  auto loc_parent = std::vector<std::int32_t>{};
  auto loc_type = std::vector<std::uint8_t>{};
  auto loc_transfer_time = std::vector<std::uint16_t>{};
  auto loc_base = std::vector<std::uint32_t>{};
  auto loc_traffic = std::vector<std::uint8_t>{};
  auto loc_src = std::vector<std::uint32_t>{};
  auto loc_lat = std::vector<double>{};
  auto loc_lon = std::vector<double>{};
  auto id_offsets = std::vector<std::uint64_t>{0U};
  auto id_bytes = std::vector<char>{};
  for (auto i = 0U; i != tt.n_locations(); ++i) {
    auto const x = n::location_idx_t{i};
    auto const parent = tt.locations_.parents_[x];
    loc_parent.push_back(parent == n::location_idx_t::invalid()
                             ? -1
                             : static_cast<std::int32_t>(to_idx(parent)));
    loc_type.push_back(static_cast<std::uint8_t>(tt.locations_.types_[x]));
    loc_transfer_time.push_back(
        static_cast<std::uint16_t>(tt.locations_.transfer_time_[x].count()));
    loc_base.push_back(
        static_cast<std::uint32_t>(to_idx(tt.locations_.get_base_idx(x))));
    loc_traffic.push_back(n::any_route_at(tt, x) ? 1U : 0U);
    loc_src.push_back(
        static_cast<std::uint32_t>(to_idx(tt.locations_.src_[x])));
    loc_lat.push_back(tt.locations_.coordinates_[x].lat_);
    loc_lon.push_back(tt.locations_.coordinates_[x].lng_);
    auto const id = std::string_view{tt.locations_.ids_[x].view()};
    id_bytes.insert(end(id_bytes), begin(id), end(id));
    id_offsets.push_back(id_bytes.size());
  }
  // footpaths (CSR by source location, profile 0)
  auto fp_begin = std::vector<std::uint32_t>{0U};
  auto fp_to = std::vector<std::uint32_t>{};
  auto fp_minutes = std::vector<std::uint16_t>{};
  for (auto i = 0U; i != tt.n_locations(); ++i) {
    for (auto const& f :
         tt.locations_.footpaths_out_[0][n::location_idx_t{i}]) {
      fp_to.push_back(static_cast<std::uint32_t>(to_idx(f.target())));
      fp_minutes.push_back(static_cast<std::uint16_t>(f.duration().count()));
    }
    fp_begin.push_back(static_cast<std::uint32_t>(fp_to.size()));
  }
  // route stop sequences (CSR by route); flags bit0 in_allowed, bit1
  // out_allowed
  auto rs_begin = std::vector<std::uint32_t>{0U};
  auto rs_loc = std::vector<std::uint32_t>{};
  auto rs_flags = std::vector<std::uint8_t>{};
  for (auto r = 0U; r != tt.n_routes(); ++r) {
    auto const seq = tt.route_location_seq_[n::route_idx_t{r}];
    for (auto i = 0U; i != seq.size(); ++i) {
      auto const st = n::stop{seq[i]};
      rs_loc.push_back(static_cast<std::uint32_t>(to_idx(st.location_idx())));
      rs_flags.push_back(static_cast<std::uint8_t>(
          (st.in_allowed() ? 1U : 0U) | (st.out_allowed() ? 2U : 0U)));
    }
    rs_begin.push_back(static_cast<std::uint32_t>(rs_loc.size()));
  }
  // trip instances overlapping [from, to] and their events (CSR by trip, in
  // route stop order; -1 = no arrival at seq 0 / no departure at the last seq)
  auto trip_route = std::vector<std::uint32_t>{};
  auto trip_transport = std::vector<std::uint32_t>{};
  auto trip_day = std::vector<std::uint32_t>{};
  auto trip_first_dep = std::vector<std::int64_t>{};
  auto trip_last_arr = std::vector<std::int64_t>{};
  auto ev_begin = std::vector<std::uint64_t>{0U};
  auto ev_arr = std::vector<std::int64_t>{};
  auto ev_dep = std::vector<std::int64_t>{};
  auto const n_days = static_cast<std::uint32_t>(
      (tt.internal_interval_days().to_ - tt.internal_interval_days().from_)
          .count());
  auto const minutes = [](n::unixtime_t const t) {
    return static_cast<std::int64_t>(t.time_since_epoch().count());
  };
  for (auto r = 0U; r != tt.n_routes(); ++r) {
    auto const ri = n::route_idx_t{r};
    auto const n_stops =
        static_cast<n::stop_idx_t>(tt.route_location_seq_[ri].size());
    for (auto const t : tt.route_transport_ranges_[ri]) {
      for (auto d = 0U; d != n_days; ++d) {
        auto const day = n::day_idx_t{d};
        if (!tt.is_transport_active(t, day)) {
          continue;
        }
        auto const tr = n::transport{t, day};
        auto const first = minutes(tt.event_time(tr, 0U, n::event_type::kDep));
        auto const last = minutes(tt.event_time(
            tr, static_cast<n::stop_idx_t>(n_stops - 1U), n::event_type::kArr));
        if (last < from || first > to) {
          continue;
        }
        trip_route.push_back(r);
        trip_transport.push_back(static_cast<std::uint32_t>(to_idx(t)));
        trip_day.push_back(d);
        trip_first_dep.push_back(first);
        trip_last_arr.push_back(last);
        for (auto i = n::stop_idx_t{0U}; i != n_stops; ++i) {
          ev_arr.push_back(
              i == 0U ? std::int64_t{-1}
                      : minutes(tt.event_time(tr, i, n::event_type::kArr)));
          ev_dep.push_back(
              i == n_stops - 1U
                  ? std::int64_t{-1}
                  : minutes(tt.event_time(tr, i, n::event_type::kDep)));
        }
        ev_begin.push_back(ev_arr.size());
      }
    }
  }
  auto sections = std::vector<vrtt::section>{};
  sections.push_back(vrtt::make_section("loc_parent", loc_parent));
  sections.push_back(vrtt::make_section("loc_type", loc_type));
  sections.push_back(vrtt::make_section("loc_transfer", loc_transfer_time));
  sections.push_back(vrtt::make_section("loc_base", loc_base));
  sections.push_back(vrtt::make_section("loc_traffic", loc_traffic));
  sections.push_back(vrtt::make_section("loc_src", loc_src));
  sections.push_back(vrtt::make_section("loc_lat", loc_lat));
  sections.push_back(vrtt::make_section("loc_lon", loc_lon));
  sections.push_back(vrtt::make_section("loc_id_offsets", id_offsets));
  sections.push_back(vrtt::make_section("loc_id_bytes", id_bytes));
  sections.push_back(vrtt::make_section("fp_begin", fp_begin));
  sections.push_back(vrtt::make_section("fp_to", fp_to));
  sections.push_back(vrtt::make_section("fp_minutes", fp_minutes));
  sections.push_back(vrtt::make_section("rs_begin", rs_begin));
  sections.push_back(vrtt::make_section("rs_loc", rs_loc));
  sections.push_back(vrtt::make_section("rs_flags", rs_flags));
  sections.push_back(vrtt::make_section("trip_route", trip_route));
  sections.push_back(vrtt::make_section("trip_transport", trip_transport));
  sections.push_back(vrtt::make_section("trip_day", trip_day));
  sections.push_back(vrtt::make_section("trip_first_dep", trip_first_dep));
  sections.push_back(vrtt::make_section("trip_last_arr", trip_last_arr));
  sections.push_back(vrtt::make_section("ev_begin", ev_begin));
  sections.push_back(vrtt::make_section("ev_arr", ev_arr));
  sections.push_back(vrtt::make_section("ev_dep", ev_dep));

  auto const align8 = [](std::uint64_t const x) {
    return (x + 7U) & ~std::uint64_t{7U};
  };
  auto h = vrtt::header{};
  h.n_sections_ = static_cast<std::uint32_t>(sections.size());
  h.from_ = from;
  h.to_ = to;
  h.days_from_ = tt.internal_interval_days().from_.time_since_epoch().count();
  h.days_to_ = tt.internal_interval_days().to_.time_since_epoch().count();
  h.data_offset_ =
      sizeof(vrtt::header) + sections.size() * sizeof(vrtt::section_entry);
  auto dir = std::vector<vrtt::section_entry>{};
  auto offset = align8(h.data_offset_);
  for (auto const& sec : sections) {
    auto e = vrtt::section_entry{};
    if (sec.name_.size() >= sizeof(e.name_)) {
      throw std::runtime_error("section name too long");
    }
    std::memcpy(e.name_, sec.name_.data(), sec.name_.size());
    e.elem_size_ = sec.elem_size_;
    e.offset_ = offset;
    e.count_ = sec.bytes_.size() / sec.elem_size_;
    dir.push_back(e);
    counts[sec.name_] = e.count_;
    offset = align8(offset + sec.bytes_.size());
  }
  // data region: [data_offset, end); padding bytes are zero and part of the
  // checksum
  auto data = std::vector<std::uint8_t>(offset - h.data_offset_, 0U);
  for (auto i = 0U; i != sections.size(); ++i) {
    if (!sections[i].bytes_.empty()) {
      std::memcpy(data.data() + (dir[i].offset_ - h.data_offset_),
                  sections[i].bytes_.data(), sections[i].bytes_.size());
    }
  }
  h.checksum_ = vrtt::fnv1a(vrtt::kFnvOffset, data.data(), data.size());
  auto out = std::ofstream{file, std::ios::binary};
  out.write(reinterpret_cast<char const*>(&h), sizeof(h));
  out.write(
      reinterpret_cast<char const*>(dir.data()),
      static_cast<std::streamsize>(dir.size() * sizeof(vrtt::section_entry)));
  out.write(reinterpret_cast<char const*>(data.data()),
            static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) {
    throw std::runtime_error(fmt::format("write failed: {}", file.string()));
  }
  return h.checksum_;
}

// ------------------------------------------------ numeric timetable export
// Exports exactly what the pinned Nigiri RAPTOR scans (profile 0, scheduled
// timetable): locations with base/parent mapping and transfer times, the
// directed footpath graph, route stop sequences with boarding/alighting flags,
// and every transport instance (transport, day) overlapping [from, to] with
// absolute UTC event times in minutes. Block / stay-seated trips are already
// merged into one transport by the loader.
int cmd_export_tt(args const& a) {
  auto const l = loaded{a.str("data")};
  auto const& tt = *l.d_->tt_;
  auto const from = a.integer("from-unix-minutes");
  auto const to = a.integer("to-unix-minutes");
  auto const dir = fs::path{a.str("out")};
  auto const format = a.str("format", "csv");
  a.check_all_used();
  if (format != "csv" && format != "bin") {
    throw std::runtime_error("--format must be csv|bin");
  }
  if (to < from) {
    throw std::runtime_error("to < from");
  }
  if (fs::exists(dir)) {
    throw std::runtime_error(
        fmt::format("refusing to overwrite {}", dir.string()));
  }
  fs::create_directories(dir);
  if (format == "bin") {
    auto const t = clock_t_::now();
    auto counts = std::map<std::string, std::uint64_t>{};
    auto const checksum =
        export_tt_bin(tt, from, to, dir / "timetable.vrtt", counts);
    auto meta = std::ofstream{dir / "export.json"};
    meta << fmt::format(
        "{{\"tool\":\"motis-accessibility "
        "export-tt\",\"schema\":1,\"format\":\"bin\","
        "\"file\":\"timetable.vrtt\",\"magic\":\"VRTTEXP1\",\"version\":1,"
        "\"checksum_fnv1a64\":\"{:016x}\",\"from_unix_minutes\":{},\"to_unix_"
        "minutes\":{},"
        "\"n_locations\":{},\"n_routes\":{},\"n_route_stops\":{},\"n_"
        "footpaths\":{},"
        "\"n_trips\":{},\"n_events\":{},\"profile\":0,\"internal_interval_"
        "days\":[{},{}]}}\n",
        checksum, from, to, tt.n_locations(), tt.n_routes(), counts["rs_loc"],
        counts["fp_to"], counts["trip_route"], counts["ev_arr"],
        tt.internal_interval_days().from_.time_since_epoch().count(),
        tt.internal_interval_days().to_.time_since_epoch().count());
    fmt::println(stderr,
                 "export-tt (bin): trips={} events={} footpaths={} "
                 "load_ms={:.1f} write_ms={:.1f}",
                 counts["trip_route"], counts["ev_arr"], counts["fp_to"],
                 l.load_ms_, ms_since(t));
    return 0;
  }
  {
    auto out = std::ofstream{dir / "locations.csv"};
    out << "loc,id,src,lat,lon,parent,type,transfer_time,base,traffic\n";
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
          << to_idx(tt.locations_.get_base_idx(x)) << ','
          << (n::any_route_at(tt, x) ? 1 : 0) << '\n';
    }
  }
  auto n_fp = std::size_t{0U};
  {
    auto out = std::ofstream{dir / "footpaths.csv"};
    out << "from,to,minutes\n";
    for (auto i = 0U; i != tt.n_locations(); ++i) {
      for (auto const& f :
           tt.locations_.footpaths_out_[0][n::location_idx_t{i}]) {
        out << i << ',' << to_idx(f.target()) << ',' << f.duration().count()
            << '\n';
        ++n_fp;
      }
    }
  }
  auto n_route_stops = std::size_t{0U};
  {
    auto out = std::ofstream{dir / "route_stops.csv"};
    out << "route,seq,loc,in_allowed,out_allowed\n";
    for (auto r = 0U; r != tt.n_routes(); ++r) {
      auto const seq = tt.route_location_seq_[n::route_idx_t{r}];
      for (auto i = 0U; i != seq.size(); ++i) {
        auto const st = n::stop{seq[i]};
        out << r << ',' << i << ',' << to_idx(st.location_idx()) << ','
            << (st.in_allowed() ? 1 : 0) << ',' << (st.out_allowed() ? 1 : 0)
            << '\n';
        ++n_route_stops;
      }
    }
  }
  auto n_trips = std::size_t{0U};
  auto n_events = std::size_t{0U};
  {
    auto trips = std::ofstream{dir / "trips.csv"};
    auto events = std::ofstream{dir / "events.csv"};
    trips << "trip,route,transport,day,first_dep,last_arr\n";
    events << "trip,seq,arr,dep\n";
    auto const n_days = static_cast<std::uint32_t>(
        (tt.internal_interval_days().to_ - tt.internal_interval_days().from_)
            .count());
    auto const minutes = [](n::unixtime_t const t) {
      return static_cast<std::int64_t>(t.time_since_epoch().count());
    };
    for (auto r = 0U; r != tt.n_routes(); ++r) {
      auto const ri = n::route_idx_t{r};
      auto const n_stops =
          static_cast<n::stop_idx_t>(tt.route_location_seq_[ri].size());
      for (auto const t : tt.route_transport_ranges_[ri]) {
        for (auto d = 0U; d != n_days; ++d) {
          auto const day = n::day_idx_t{d};
          if (!tt.is_transport_active(t, day)) {
            continue;
          }
          auto const tr = n::transport{t, day};
          auto const first =
              minutes(tt.event_time(tr, 0U, n::event_type::kDep));
          auto const last = minutes(
              tt.event_time(tr, static_cast<n::stop_idx_t>(n_stops - 1U),
                            n::event_type::kArr));
          if (last < from || first > to) {
            continue;
          }
          trips << n_trips << ',' << r << ',' << to_idx(t) << ',' << d << ','
                << first << ',' << last << '\n';
          for (auto i = n::stop_idx_t{0U}; i != n_stops; ++i) {
            auto const arr =
                i == 0U ? std::int64_t{-1}
                        : minutes(tt.event_time(tr, i, n::event_type::kArr));
            auto const dep =
                i == n_stops - 1U
                    ? std::int64_t{-1}
                    : minutes(tt.event_time(tr, i, n::event_type::kDep));
            events << n_trips << ',' << i << ',' << arr << ',' << dep << '\n';
            ++n_events;
          }
          ++n_trips;
        }
      }
    }
  }
  auto meta = std::ofstream{dir / "export.json"};
  meta << fmt::format(
      "{{\"tool\":\"motis-accessibility "
      "export-tt\",\"schema\":1,\"from_unix_minutes\":{},"
      "\"to_unix_minutes\":{},\"n_locations\":{},\"n_routes\":{},\"n_route_"
      "stops\":{},"
      "\"n_footpaths\":{},\"n_trips\":{},\"n_events\":{},\"profile\":0,"
      "\"internal_interval_days\":[{},{}]}}\n",
      from, to, tt.n_locations(), tt.n_routes(), n_route_stops, n_fp, n_trips,
      n_events, tt.internal_interval_days().from_.time_since_epoch().count(),
      tt.internal_interval_days().to_.time_since_epoch().count());
  fmt::println(
      stderr,
      "export-tt: routes={} trips={} events={} footpaths={} load_ms={:.1f}",
      tt.n_routes(), n_trips, n_events, n_fp, l.load_ms_);
  return 0;
}

// ------------------------------------------------- OSR connectors (stock)
int cmd_connectors(args const& a) {
  auto const l = loaded{a.str("data")};
  auto r = utl::init_from<motis::ep::routing>(*l.d_);
  if (!r.has_value() || !r->is_osr_loaded()) {
    throw std::runtime_error("connectors require street routing (OSM) data");
  }
  // --points: coordinates (osr::location); --stop-points: CSV with a `stop`
  // column (Nigiri location index) routed as stock tt_location places, i.e.
  // walking offsets plus zero-cost equivalent/meta stops exactly as the stock
  // endpoints start from a stop.
  auto points = std::vector<motis::place_t>{};
  if (a.has("stop-points")) {
    auto const t = read_csv(a.str("stop-points"));
    auto const c = t.col("stop");
    for (auto const& r : t.rows_) {
      auto const li = parse_num<std::uint32_t>(r.at(c));
      if (li >= l.d_->tt_->n_locations()) {
        throw std::runtime_error("stop-points entry out of range");
      }
      points.emplace_back(motis::tt_location{n::location_idx_t{li}});
    }
  } else {
    for (auto const& p : read_points(a.str("points"))) {
      points.emplace_back(osr::location{p, osr::kNoLevel});
    }
  }
  auto const dir_str = a.str("dir");
  if (dir_str != "forward" && dir_str != "backward") {
    throw std::runtime_error("--dir must be forward|backward");
  }
  auto const dir = dir_str == "forward" ? osr::direction::kForward
                                        : osr::direction::kBackward;
  auto const max = std::chrono::seconds{a.integer("max-seconds")};
  auto const max_matching = a.num("max-matching-distance", 25.0);
  auto const params = walk_params(a);
  auto const n_threads =
      static_cast<unsigned>(a.checked("threads", 1, 1, 4096));
  auto const out_path = a.str("out");
  a.check_all_used();

  auto const t = clock_t_::now();
  auto results = std::vector<std::vector<n::routing::offset>>(points.size());
  auto next = std::atomic_size_t{0U};
  auto worker = [&]() {
    auto gbfs_rd = motis::gbfs::gbfs_routing_data{r->w_, r->l_,
                                                  std::atomic_load(&r->gbfs_)};
    auto stats = std::map<std::string, std::uint64_t>{};
    for (auto i = next++; i < points.size(); i = next++) {
      results[i] =
          r->get_offsets(nullptr, points[i], dir, {motis::api::ModeEnum::WALK},
                         motis::rental_options{}, params,
                         motis::api::PedestrianProfileEnum::FOOT,
                         motis::api::ElevationCostsEnum::NONE, max,
                         max_matching, gbfs_rd, stats);
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
  auto const n_threads =
      static_cast<unsigned>(a.checked("threads", 1, 1, 4096));
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
              cand[k],
              static_cast<std::uint16_t>(std::ceil(*res[k].duration_ / 60.0)));
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

// ------------------------------------------- one_to_all with reused state
// Experimental (--reuse-state): the body of nigiri::routing::one_to_all
// (7575542b src/routing/one_to_all.cc, forward, no RT) with a caller-owned
// raptor_state and lower-bound / destination vectors reused per worker. The
// raptor constructor resizes (no-op after the first query) and resets all
// arrival arrays, and next_start_time() resets best/tmp/marks, so every query
// starts from a fresh state. Stock semantics and all kMaxTransfers + 2 rounds
// are unchanged.
struct reuse_ctx {
  n::routing::raptor_state state_;
  n::bitvec is_dest_;
  std::array<n::bitvec, n::routing::kMaxVias> is_via_;
  std::vector<std::uint16_t> dist_to_dest_;
  std::vector<std::uint16_t> lb_;
  std::uint64_t init_ns_{}, scan_ns_{}, finish_ns_{};
};

// Nigiri versions with `one_to_all(tt, rtt, q, raptor_state&)` run the stock
// one_to_all into a reused state (then only the total is timed, as scan).
template <typename TT>
constexpr bool kHasReusableOneToAll = requires(
    TT const& tt, n::routing::query const& q, n::routing::raptor_state& s) {
  n::routing::one_to_all<n::direction::kForward>(tt, nullptr, q, s);
};

// A template so that the branch for the other Nigiri is discarded.
template <typename TT>
n::routing::raptor_state const& one_to_all_reuse(TT const& tt,
                                                 n::routing::query const& q,
                                                 reuse_ctx& c) {
  namespace r = n::routing;
  constexpr auto kVias = n::via_offset_t{0U};
  auto const ns = [](auto const a, auto const b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
  };
  auto const t0 = clock_t_::now();
  if constexpr (kHasReusableOneToAll<TT>) {
    r::one_to_all<n::direction::kForward>(tt, nullptr, q, c.state_);
    c.scan_ns_ += ns(t0, clock_t_::now());
    return c.state_;
  }
  auto const start_time = std::get<n::unixtime_t>(q.start_time_);
  if (c.is_dest_.size() != tt.n_locations()) {
    c.is_dest_ = n::bitvec{tt.n_locations()};
    c.lb_.assign(tt.n_locations(), 0U);
  }
  auto const base =
      n::day_idx_t{std::chrono::duration_cast<date::days>(
                       std::chrono::round<std::chrono::days>(start_time) -
                       tt.internal_interval().from_)
                       .count()};
  auto ra =
      r::raptor<n::direction::kForward, false, kVias,
                r::search_mode::kOneToAll>{tt,
                                           nullptr,
                                           c.state_,
                                           c.is_dest_,
                                           c.is_via_,
                                           c.dist_to_dest_,
                                           q.td_dest_,
                                           c.lb_,
                                           q.via_stops_,
                                           base,
                                           q.allowed_claszes_,
                                           q.require_bike_transport_,
                                           q.require_car_transport_,
                                           q.prf_idx_ == n::kWheelchairProfile,
                                           q.no_compulsory_reservation_,
                                           q.transfer_time_settings_,
                                           q.prf_idx_};
  auto const worst_time_at_dest = start_time + q.max_travel_time_;
  auto const worst_at_dest = ra.to_delta(worst_time_at_dest);
  auto const t1 = clock_t_::now();
  auto results = n::pareto_set<r::journey>{};
  ra.next_start_time();
  for (auto const& s : q.start_) {
    auto const t = start_time + s.duration();
    r::for_each_meta(tt, r::rt_virts(nullptr, q.prf_idx_), q.start_match_mode_,
                     s.target(),
                     [&](n::location_idx_t const l) { ra.add_start(l, t); });
  }
  auto worst = std::array<n::unixtime_t, r::kMaxTransfers + 2U>{};
  worst.fill(worst_time_at_dest);
  ra.execute(start_time, q.max_transfers_, worst, results);
  auto const t2 = clock_t_::now();
  // finish_one_to_all (forward)
  constexpr auto kInvalid = n::kInvalidDelta<n::direction::kForward>;
  auto best = c.state_.get_best<kVias>();
  auto& arrivals = c.state_.one_to_all_arrivals_;
  for (auto l = n::location_idx_t{0U}; l != c.state_.n_locations_; ++l) {
    auto& b = best[to_idx(l)][kVias];
    b = kInvalid;
    for (auto k = 0U; k != r::kMaxTransfers + 2U; ++k) {
      auto& t = arrivals[k][to_idx(l)];
      if (t < b && !(worst_at_dest < t)) {
        b = t;
      } else {
        t = kInvalid;
      }
    }
  }
  auto const t3 = clock_t_::now();
  c.init_ns_ += ns(t0, t1);
  c.scan_ns_ += ns(t1, t2);
  c.finish_ns_ += ns(t2, t3);
  return c.state_;
}

// ------------------------------------- r5r-style travel time / accessibility
// Ports of Conveyal R5 (github.com/conveyal/r5, dev 500ce263, 2026-11-11):
// analyst/TravelTimeReducer.java (percentiles, accessibility accumulation) and
// analyst/decay/*.java. Inputs here are whole minutes (Nigiri resolution)
// passed to the R5 formulas as seconds (60 * minutes). R5 evaluates exp/log
// with FastMath; std::exp / std::log may differ in the last bit.
namespace r5 {

constexpr auto kUnreached = std::numeric_limits<std::int32_t>::max();
constexpr auto kTwoHours = 2 * 60 * 60;
constexpr auto kZeroEpsilon = 0.001;

// TravelTimeReducer.findPercentileIndex, (int) ceil(p / 100 * n) - 1: the
// smallest value such that no more than p % of the sorted list is below it
// (non-interpolated). Percentiles are integers 1..100 (R5's task.percentiles is
// an int[]); the rank is computed in exact integer arithmetic,
// ceil(p * n / 100) = (p * n + 99) / 100, so that it cannot drift on exact
// boundaries (in doubles 7 / 100.0 * 100 > 7).
int percentile_index(int const n, int const p) {
  return (p * n + 99) / 100 - 1;
}

struct decay {
  enum class kind {
    kStep,
    kLinear,
    kExponential,
    kFixedExponential,
    kLogistic
  } kind_;
  int width_seconds_{}, half_width_seconds_{};
  std::vector<double> linear_table_;
  double decay_constant_{};
  double sd_seconds_{};

  static decay parse(std::string const& spec) {
    auto d = decay{};
    auto const colon = spec.find(':');
    auto const name = spec.substr(0, colon);
    auto const param =
        colon == std::string::npos ? std::string{} : spec.substr(colon + 1);
    if (name == "step") {
      d.kind_ = kind::kStep;
    } else if (name == "linear") {  // linear:WIDTH_MINUTES, 0 <= W < 60
      d.kind_ = kind::kLinear;
      auto const w = std::stoi(param);
      if (w < 0 || w >= 60) {
        throw std::runtime_error(
            "linear decay width must be in [0, 60) minutes");
      }
      d.width_seconds_ = w * 60;
      d.half_width_seconds_ = d.width_seconds_ / 2;
      for (auto s = 0; s < d.width_seconds_; ++s) {
        d.linear_table_.push_back(1 - ((s + 0.5) / d.width_seconds_));
      }
    } else if (name == "exponential") {  // half-life = cutoff
      d.kind_ = kind::kExponential;
    } else if (name == "fixed-exponential") {  // fixed-exponential:K, per
                                               // second, 0 < K < 1
      d.kind_ = kind::kFixedExponential;
      d.decay_constant_ = std::stod(param);
      if (!(d.decay_constant_ > 0 && d.decay_constant_ < 1)) {
        throw std::runtime_error(
            "fixed-exponential decay constant must be in (0, 1)");
      }
    } else if (name == "logistic") {  // logistic:SD_MINUTES
      d.kind_ = kind::kLogistic;
      d.sd_seconds_ = std::stod(param) * 60;
      if (!(d.sd_seconds_ > 0 && d.sd_seconds_ < kTwoHours)) {
        throw std::runtime_error(
            "logistic standard deviation must be in (0, 120) minutes");
      }
    } else {
      throw std::runtime_error(fmt::format("unknown decay function {}", spec));
    }
    return d;
  }

  double weight(int const cutoff, int const t) const {
    switch (kind_) {
      case kind::kStep: return t < cutoff ? 1 : 0;
      case kind::kLinear: {
        auto const idx = t - (cutoff - half_width_seconds_);
        if (idx < 0) {
          return 1;
        }
        return idx < width_seconds_
                   ? linear_table_[static_cast<std::size_t>(idx)]
                   : 0;
      }
      case kind::kExponential:
        if (t <= 0) {
          return 1;
        }
        if (t >= kTwoHours) {
          return 0;
        }
        return std::exp(std::log(0.5) / cutoff * t);
      case kind::kFixedExponential:
        if (t <= 0) {
          return 1;
        }
        if (t >= kTwoHours) {
          return 0;
        }
        return std::exp(-decay_constant_ * t);
      case kind::kLogistic: {
        auto const g = [&](int const median, int const tt) {
          return 1 + std::exp(((tt - median) * std::numbers::pi) /
                              (sd_seconds_ * std::numbers::sqrt3));
        };
        return g(cutoff, 0) / g(cutoff, t);
      }
    }
    return 0;
  }

  // DecayFunction.reachesZeroAt (step / linear override; others bisect).
  int reaches_zero_at(int const cutoff) const {
    if (kind_ == kind::kStep) {
      return cutoff;
    }
    if (kind_ == kind::kLinear) {
      return cutoff + half_width_seconds_;
    }
    auto low = 0;
    auto high = 4 * 60 * 60;
    while (low < high) {
      auto const mid = (low + high) / 2;
      if (weight(cutoff, mid) < kZeroEpsilon) {
        high = mid;
      } else {
        low = mid + 1;
      }
    }
    return low;
  }
};

}  // namespace r5

// --percentiles / --cutoffs of `route --r5r-out` and `r5r-reduce` (and
// r5r_reference.py): strictly increasing, unique integers in [lo, hi]. Each
// percentile and cutoff is evaluated on its own; the order only fixes the
// output order (and R5's early exit over sorted cutoffs).
std::vector<int> parse_strictly_increasing(std::string const& spec,
                                           int const lo,
                                           int const hi,
                                           std::string_view const name) {
  auto v = std::vector<int>{};
  for (auto const x : split(spec, ',')) {
    auto const i = parse_num<int>(x);
    if (i < lo || i > hi) {
      throw std::runtime_error(fmt::format(
          "--{}: {} is not an integer in [{}, {}]", name, i, lo, hi));
    }
    if (!v.empty() && i <= v.back()) {
      throw std::runtime_error(fmt::format(
          "--{} must be strictly increasing and unique, got {} after {}", name,
          i, v.back()));
    }
    v.push_back(i);
  }
  return v;
}

// r5r-style outputs of one window, shared by `route --r5r-out` and
// `r5r-reduce`. ttm.bin: magic "VRR5TTM1", u32 version 1, u32 n_dests, u32
// n_percentiles, u32 n_departures, f64 percentiles[n_percentiles] (integers),
// then per origin (completion order) u32 origin + u16
// minutes[n_dests][n_percentiles] (65535 = not reached within the cap at that
// percentile). accessibility.csv:
// origin,opportunity,percentile,cutoff,accessibility. Per origin: for each
// destination sort its per-departure travel times (seconds, unreached last),
// take the percentiles, then accumulate opportunities with the decay function
// in destination order (as TravelTimeReducer does).
struct r5r_output {
  r5r_output(fs::path const& dir,
             std::uint32_t const n_dests,
             std::uint32_t const n_deps,
             std::vector<int> percentiles,
             std::vector<int> cutoffs_min,
             std::string const& decay_spec,
             std::vector<std::vector<double>> opportunities,
             std::vector<std::string> opportunity_names)
      : n_dests_{n_dests},
        n_deps_{n_deps},
        percentiles_{std::move(percentiles)},
        cutoffs_min_{std::move(cutoffs_min)},
        decay_{r5::decay::parse(decay_spec)},
        opportunities_{std::move(opportunities)},
        opportunity_names_{std::move(opportunity_names)} {
    for (auto const pc : percentiles_) {
      pct_idx_.push_back(r5::percentile_index(static_cast<int>(n_deps_), pc));
    }
    for (auto const c : cutoffs_min_) {
      zero_points_.push_back(decay_.reaches_zero_at(c * 60));
    }
    fs::create_directories(dir);
    ttm_.open(dir / "ttm.bin", std::ios::binary);
    ttm_.write("VRR5TTM1", 8);
    auto const hdr = std::array<std::uint32_t, 4>{
        1U, n_dests_, static_cast<std::uint32_t>(percentiles_.size()), n_deps_};
    ttm_.write(reinterpret_cast<char const*>(hdr.data()), sizeof(hdr));
    for (auto const pc : percentiles_) {
      auto const d = static_cast<double>(pc);
      ttm_.write(reinterpret_cast<char const*>(&d), sizeof(d));
    }
    acc_.open(dir / "accessibility.csv");
    acc_ << "origin,opportunity,percentile,cutoff,accessibility\n";
  }

  // buf: n_deps x n_dests travel minutes (departure-major), `unreachable` =
  // none
  template <typename T>
  void origin(std::uint32_t const o, T const* buf, T const unreachable) {
    auto const n_p = percentiles_.size();
    auto ttm =
        std::vector<std::uint16_t>(static_cast<std::size_t>(n_dests_) * n_p);
    auto acc = std::vector<double>(
        opportunities_.size() * n_p * cutoffs_min_.size(), 0.0);
    auto times = std::vector<std::int32_t>(n_deps_);
    auto pt = std::vector<std::int32_t>(n_p);
    for (auto d = 0U; d != n_dests_; ++d) {
      for (auto di = 0U; di != n_deps_; ++di) {
        auto const v = buf[static_cast<std::size_t>(di) * n_dests_ + d];
        times[di] = v == unreachable ? r5::kUnreached
                                     : static_cast<std::int32_t>(v) * 60;
      }
      std::sort(begin(times), end(times));
      for (auto pi = 0U; pi != n_p; ++pi) {
        pt[pi] = times[static_cast<std::size_t>(pct_idx_[pi])];
        ttm[static_cast<std::size_t>(d) * n_p + pi] =
            pt[pi] == r5::kUnreached ? std::uint16_t{0xFFFFU}
                                     : static_cast<std::uint16_t>(pt[pi] / 60);
      }
      for (auto oi = 0U; oi != opportunities_.size(); ++oi) {
        auto const count = opportunities_[oi][d];
        if (!(count > 0)) {
          continue;
        }
        for (auto pi = 0U; pi != n_p; ++pi) {
          auto const t = pt[pi];
          if (t == r5::kUnreached) {
            continue;  // this percentile only; the others are independent
          }
          for (auto ci = static_cast<int>(cutoffs_min_.size()) - 1; ci >= 0;
               --ci) {
            if (t >= zero_points_[static_cast<std::size_t>(ci)]) {
              break;
            }
            auto const w = decay_.weight(
                cutoffs_min_[static_cast<std::size_t>(ci)] * 60, t);
            if (w > 0) {
              acc[(oi * n_p + pi) * cutoffs_min_.size() +
                  static_cast<std::size_t>(ci)] += count * w;
            }
          }
        }
      }
    }
    auto lines = std::string{};
    for (auto oi = 0U; oi != opportunities_.size(); ++oi) {
      for (auto pi = 0U; pi != n_p; ++pi) {
        for (auto ci = 0U; ci != cutoffs_min_.size(); ++ci) {
          lines +=
              fmt::format("{},{},{},{},{:.17g}\n", o, opportunity_names_[oi],
                          percentiles_[pi], cutoffs_min_[ci],
                          acc[(oi * n_p + pi) * cutoffs_min_.size() + ci]);
        }
      }
    }
    auto const lock = std::lock_guard{mutex_};
    ttm_.write(reinterpret_cast<char const*>(&o), sizeof(o));
    ttm_.write(
        reinterpret_cast<char const*>(ttm.data()),
        static_cast<std::streamsize>(ttm.size() * sizeof(std::uint16_t)));
    acc_ << lines;
  }

  void close() {
    ttm_.close();
    acc_.close();
    if (!ttm_ || !acc_) {
      throw std::runtime_error("r5r write failed");
    }
  }

  std::uint32_t n_dests_, n_deps_;
  std::vector<int> percentiles_, cutoffs_min_;
  r5::decay decay_;
  std::vector<std::vector<double>> opportunities_;
  std::vector<std::string> opportunity_names_;
  std::vector<int> pct_idx_, zero_points_;
  std::ofstream ttm_, acc_;
  std::mutex mutex_;
};

// r5r-selftest: hand-ranked boundaries. With N travel times that are a
// permutation of 1..N minutes, percentile p is the value at rank ceil(p * N /
// 100) (counted from 1), here obtained by counting: the smallest rank k with
// 100 * k >= p * N. Checks every p in 1..100 for N in {1, 2, 3, 7, 30, 99, 100,
// 101, 120, 150, 151, 1440}, through r5r_output itself (one origin, one
// destination). Exit 0 if all agree.
int cmd_r5r_selftest(args const& a) {
  auto const dir = fs::path{a.str("out")};
  a.check_all_used();
  auto failures = 0;
  auto checked = 0;
  for (auto const n : {1, 2, 3, 7, 30, 99, 100, 101, 120, 150, 151, 1440}) {
    auto ps = std::vector<int>{};
    for (auto p = 1; p <= 100; ++p) {
      ps.push_back(p);
    }
    auto buf = std::vector<std::uint16_t>(static_cast<std::size_t>(n));
    for (auto i = 0; i != n; ++i) {
      buf[static_cast<std::size_t>(i)] =
          static_cast<std::uint16_t>(((i * 37) % n) + 1);  // a permutation
    }
    auto const sub = dir / fmt::format("n{}", n);
    {
      auto out = r5r_output{
          sub, 1U, static_cast<std::uint32_t>(n), ps, {}, "step", {}, {}};
      out.origin(0U, buf.data(), std::uint16_t{0xFFFFU});
      out.close();
    }
    auto in = std::ifstream{sub / "ttm.bin", std::ios::binary};
    auto bytes = std::vector<char>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    auto const off = 8U + 16U + 8U * ps.size() + 4U;
    for (auto pi = 0U; pi != ps.size(); ++pi) {
      auto v = std::uint16_t{};
      std::memcpy(&v, bytes.data() + off + 2U * pi, 2U);
      auto k = 1;
      while (100 * k < ps[pi] * n) {
        ++k;
      }
      ++checked;
      if (v != k) {
        ++failures;
        fmt::println("N={} p={}: got {} expected {}", n, ps[pi], v, k);
      }
    }
  }
  fmt::println("{{\"r5r_selftest_checked\":{},\"failures\":{}}}", checked,
               failures);
  return failures == 0 ? 0 : 3;
}

// r5r-reduce: the same r5r-style outputs from a raw `route --out` stream (the
// output-equivalent "raw stream + reducer" path).
int cmd_r5r_reduce(args const& a) {
  auto const stream = fs::path{a.str("stream")};
  auto const dir = fs::path{a.str("r5r-out")};
  auto in = std::ifstream{stream, std::ios::binary};
  if (!in) {
    throw std::runtime_error(fmt::format("cannot open {}", stream.string()));
  }
  // every read must deliver exactly the bytes the format requires
  auto const read_exact = [&](void* dst, std::size_t const n,
                              std::string_view const what) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(in.gcount()) != n) {
      throw std::runtime_error(fmt::format(
          "truncated stream: {} ({} of {} bytes)", what, in.gcount(), n));
    }
  };
  char magic[8];
  read_exact(magic, 8, "magic");
  if (std::string_view{magic, 8} != "VRTTMATX") {
    throw std::runtime_error("not a route stream");
  }
  auto hdr = std::array<std::uint32_t,
                        6>{};  // version, n_total, n_dests, n_dep, from, to
  read_exact(hdr.data(), sizeof(hdr), "header");
  auto cap = std::uint16_t{};
  auto flags = std::uint8_t{};
  auto target = std::uint8_t{};
  read_exact(&cap, 2, "header");
  read_exact(&flags, 1, "header");
  read_exact(&target, 1, "header");
  auto reserved = std::array<char, 20>{};
  read_exact(reserved.data(), reserved.size(), "header");
  if (hdr[0] != 1U) {
    throw std::runtime_error(fmt::format(
        "unsupported stream version {}{}", hdr[0],
        hdr[0] == kSparseFormatVersion
            ? " (sparse; convert it with `stream-decode` first)"
            : ""));
  }
  auto const n_dests = hdr[2];
  auto const n_deps = hdr[3];
  if (n_deps == 0U) {
    throw std::runtime_error("stream n_departures must be at least 1");
  }
  auto departures = std::vector<std::int64_t>(n_deps);
  read_exact(departures.data(), 8U * n_deps, "departures");
  auto const percentiles = parse_strictly_increasing(a.str("percentiles", "50"),
                                                     1, 100, "percentiles");
  auto cutoffs_min = std::vector<int>{};
  auto opportunities = opportunity_data{};
  auto decay_spec = std::string{"step"};
  if (a.has("opportunities")) {
    cutoffs_min =
        parse_strictly_increasing(a.str("cutoffs"), 1, 240, "cutoffs");
    decay_spec = a.str("decay", "step");
    opportunities = read_opportunities(a, n_dests);
  }
  a.check_all_used();
  auto out = r5r_output{dir,
                        n_dests,
                        n_deps,
                        percentiles,
                        cutoffs_min,
                        decay_spec,
                        opportunities.values_,
                        opportunities.names_};
  auto const block = static_cast<std::size_t>(n_deps) * n_dests;
  // optional transfers (u8) and egress-stop (u32) blocks, read and discarded
  auto const extra =
      ((flags & 1U) ? block : 0U) + ((flags & 2U) ? 4U * block : 0U);
  auto tail = std::vector<char>(extra);
  auto const read_origins = [&](auto const type_tag) {
    auto seen = std::unordered_set<std::uint32_t>{};
    using T = std::decay_t<decltype(type_tag)>;
    auto buf = std::vector<T>(block);
    while (true) {
      auto o = std::uint32_t{};
      in.read(reinterpret_cast<char*>(&o), sizeof(o));
      if (in.gcount() == 0 && in.eof()) {
        break;  // clean end: no bytes after the last complete record
      }
      if (in.gcount() != sizeof(o)) {
        throw std::runtime_error(fmt::format(
            "truncated stream: {} trailing byte(s), not a complete origin id",
            in.gcount()));
      }
      if (!seen.insert(o).second) {
        throw std::runtime_error(
            fmt::format("duplicate stream record for origin {}", o));
      }
      read_exact(buf.data(), block * sizeof(T), "travel-time block");
      read_exact(tail.data(), extra, "transfer / egress-stop block");
      out.origin(o, buf.data(), std::numeric_limits<T>::max());
    }
  };
  if (flags & 4U) {
    read_origins(std::uint16_t{});
  } else {
    read_origins(std::uint8_t{});
  }
  out.close();
  return 0;
}

// `stream-decode --in S2.bin --out S1.bin`: rewrites a version-2 (sparse)
// stream as the version-1 stream the same run writes with the default
// encoding: same header (version 1), departures and records in file order.
int cmd_stream_decode(args const& a) {
  auto const in_path = fs::path{a.str("in")};
  auto const out_path = fs::path{a.str("out")};
  a.check_all_used();
  auto in = std::ifstream{in_path, std::ios::binary};
  if (!in) {
    throw std::runtime_error(fmt::format("cannot open {}", in_path.string()));
  }
  auto const read_exact = [&](void* dst, std::size_t const n,
                              std::string_view const what) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(in.gcount()) != n) {
      throw std::runtime_error(fmt::format(
          "truncated stream: {} ({} of {} bytes)", what, in.gcount(), n));
    }
  };
  auto hdr = std::array<char, 56>{};
  read_exact(hdr.data(), hdr.size(), "header");
  if (std::string_view{hdr.data(), 8} != "VRTTMATX") {
    throw std::runtime_error("not a route stream");
  }
  auto version = std::uint32_t{};
  auto n_dests = std::uint32_t{};
  auto n_deps = std::uint32_t{};
  std::memcpy(&version, hdr.data() + 8, 4);
  std::memcpy(&n_dests, hdr.data() + 16, 4);
  std::memcpy(&n_deps, hdr.data() + 20, 4);
  auto const flags = static_cast<std::uint8_t>(hdr[34]);
  if (version != kSparseFormatVersion) {
    throw std::runtime_error(
        fmt::format("stream-decode expects version 2, got {}", version));
  }
  if (flags & ~7U) {
    throw std::runtime_error(fmt::format("unknown stream flags {}", flags));
  }
  auto deps = std::vector<char>(8U * n_deps);
  read_exact(deps.data(), deps.size(), "departures");
  auto const cells = static_cast<std::size_t>(n_deps) * n_dests;
  auto const tt_size = (flags & 4U) ? 2U : 1U;
  auto const has_tr = (flags & 1U) != 0U;
  auto const has_via = (flags & 2U) != 0U;
  auto const cell_bytes = tt_size + (has_tr ? 1U : 0U) + (has_via ? 4U : 0U);
  auto const dense_bytes = cells * cell_bytes;
  auto const bitmap_bytes = (cells + 7U) / 8U;

  auto out = std::ofstream{out_path, std::ios::binary};
  auto const v1 = kFormatVersion;
  std::memcpy(hdr.data() + 8, &v1, 4);
  out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
  out.write(deps.data(), static_cast<std::streamsize>(deps.size()));

  auto dense = std::vector<char>(dense_bytes);
  auto bitmap = std::vector<std::uint8_t>(bitmap_bytes);
  auto payload = std::vector<char>{};
  auto seen = std::unordered_set<std::uint32_t>{};
  auto n_records = std::uint64_t{0U};
  auto n_sparse = std::uint64_t{0U};
  while (true) {
    auto o = std::uint32_t{};
    in.read(reinterpret_cast<char*>(&o), sizeof(o));
    if (in.gcount() == 0 && in.eof()) {
      break;
    }
    if (in.gcount() != sizeof(o)) {
      throw std::runtime_error("truncated stream: incomplete origin id");
    }
    if (!seen.insert(o).second) {
      throw std::runtime_error(
          fmt::format("duplicate stream record for origin {}", o));
    }
    auto tag = std::uint8_t{};
    read_exact(&tag, 1U, "record encoding");
    if (tag == kRecordDense) {
      read_exact(dense.data(), dense.size(), "dense record");
    } else if (tag == kRecordBitmap) {
      ++n_sparse;
      auto n = std::uint32_t{};
      read_exact(&n, 4U, "reached count");
      read_exact(bitmap.data(), bitmap.size(), "bitmap");
      auto count = std::size_t{0U};
      for (auto i = std::size_t{0U}; i != cells; ++i) {
        count += (bitmap[i / 8U] >> (i % 8U)) & 1U;
      }
      if (cells % 8U != 0U && (bitmap.back() >> (cells % 8U)) != 0U) {
        throw std::runtime_error("bitmap padding bits set");
      }
      if (count != n) {
        throw std::runtime_error(
            fmt::format("bitmap holds {} cells, count says {}", count, n));
      }
      payload.resize(static_cast<std::size_t>(n) * cell_bytes);
      read_exact(payload.data(), payload.size(), "bitmap payload");
      auto* d_tt = dense.data();
      auto* d_tr = d_tt + cells * tt_size;
      auto* d_via = d_tr + (has_tr ? cells : 0U);
      std::memset(d_tt, 0xFF, dense.size());  // unreachable / 255 / no stop
      auto const* p_tt = payload.data();
      auto const* p_tr = p_tt + static_cast<std::size_t>(n) * tt_size;
      auto const* p_via = p_tr + (has_tr ? n : 0U);
      auto r = std::size_t{0U};
      for (auto i = std::size_t{0U}; i != cells; ++i) {
        if (((bitmap[i / 8U] >> (i % 8U)) & 1U) == 0U) {
          continue;
        }
        std::memcpy(d_tt + i * tt_size, p_tt + r * tt_size, tt_size);
        if (has_tr) {
          d_tr[i] = p_tr[r];
        }
        if (has_via) {
          std::memcpy(d_via + i * 4U, p_via + r * 4U, 4U);
        }
        ++r;
      }
    } else {
      throw std::runtime_error(
          fmt::format("unknown record encoding {} for origin {}", tag, o));
    }
    out.write(reinterpret_cast<char const*>(&o), sizeof(o));
    out.write(dense.data(), static_cast<std::streamsize>(dense.size()));
    ++n_records;
  }
  out.close();
  if (!out) {
    throw std::runtime_error("write failed");
  }
  fmt::println(stderr, "decoded {} records ({} bitmap, {} dense)", n_records,
               n_sparse, n_records - n_sparse);
  return 0;
}

// ------------------------------------------ Range-RAPTOR over a window
// --range: per origin, one raptor runs the departures of the window latest
// first, as Nigiri's range search does: between departures only
// next_start_time() resets (best / tmp / marks); round times and the one-to-all
// round arrivals are kept, so a departure's labels start from what later
// departures reached (they stay valid: one can wait). The kept arrival rows
// hold the best over this and all later departures; each departure gets a
// finished copy (strict improvement over earlier rounds, its own cap) in `out`,
// with exactly the layout one_to_all returns, so the readers below are
// unchanged. Requires one Nigiri day base for the whole window (checked by the
// caller).
using range_raptor_t = n::routing::raptor<n::direction::kForward,
                                          false,
                                          n::via_offset_t{0U},
                                          n::routing::search_mode::kOneToAll>;

void make_range_raptor(n::timetable const& tt,
                       n::routing::query const& q,
                       n::day_idx_t const base,
                       reuse_ctx& c,
                       std::optional<range_raptor_t>& ra) {
  if (c.is_dest_.size() != tt.n_locations()) {
    c.is_dest_ = n::bitvec{tt.n_locations()};
    c.lb_.assign(tt.n_locations(), 0U);
  }
  ra.emplace(tt, nullptr, c.state_, c.is_dest_, c.is_via_, c.dist_to_dest_,
             q.td_dest_, c.lb_, q.via_stops_, base, q.allowed_claszes_,
             q.require_bike_transport_, q.require_car_transport_,
             q.prf_idx_ == n::kWheelchairProfile, q.no_compulsory_reservation_,
             q.transfer_time_settings_, q.prf_idx_);
}

void range_step(n::timetable const& tt,
                n::routing::query const& q,
                range_raptor_t& ra,
                reuse_ctx const& c,
                n::routing::raptor_state& out) {
  namespace r = n::routing;
  constexpr auto kInvalid = n::kInvalidDelta<n::direction::kForward>;
  auto const start_time = std::get<n::unixtime_t>(q.start_time_);
  auto results = n::pareto_set<r::journey>{};
  ra.next_start_time();
  for (auto const& s : q.start_) {
    auto const t = start_time + s.duration();
    r::for_each_meta(tt, r::rt_virts(nullptr, q.prf_idx_), q.start_match_mode_,
                     s.target(),
                     [&](n::location_idx_t const l) { ra.add_start(l, t); });
  }
  auto const worst_time_at_dest = start_time + q.max_travel_time_;
  auto worst = std::array<n::unixtime_t, r::kMaxTransfers + 2U>{};
  worst.fill(worst_time_at_dest);
  ra.execute(start_time, q.max_transfers_, worst, results);

  // finished copy for this departure (finish_one_to_all on rows
  // 0..max_transfers+1)
  auto const worst_at_dest = ra.to_delta(worst_time_at_dest);
  auto const n_rows = static_cast<std::size_t>(q.max_transfers_) + 2U;
  auto const n_loc = static_cast<std::size_t>(tt.n_locations());
  auto const& acc = c.state_.one_to_all_arrivals_;
  auto& fin = out.one_to_all_arrivals_;
  if (fin.n_rows_ != r::kMaxTransfers + 2U || fin.n_columns_ != n_loc) {
    fin.resize(r::kMaxTransfers + 2U, n_loc);
    std::fill(begin(fin.entries_), end(fin.entries_), kInvalid);
  }
  std::copy_n(begin(acc.entries_), n_rows * n_loc, begin(fin.entries_));
  for (auto l = std::size_t{0U}; l != n_loc; ++l) {
    auto b = kInvalid;
    for (auto k = std::size_t{0U}; k != n_rows; ++k) {
      auto& t = fin[k][l];
      if (t < b && !(worst_at_dest < t)) {
        b = t;
      } else {
        t = kInvalid;
      }
    }
  }
}

// ---------------------------------------------------------------- route
// --egress-stats: work counters of the destination egress loop and the
// occupancy of the finished rows (one row = one departure of one origin).
// `bitmap_bytes` sizes a row as a presence bitmap (ceil(n_dests / 8) bytes)
// plus the stream's per-cell bytes for reached cells only; `best_bytes` takes
// the smaller of that and the dense row, row by row.
struct egress_stats_t {
  std::uint64_t stops_with_egress_{};  // summed over queries
  std::uint64_t stops_reached_{};  // stops with egress and a valid arrival
  std::uint64_t links_total_{};  // egress links of the reached stops
  std::uint64_t links_within_cap_{};
  std::uint64_t links_improved_{};
  std::uint64_t rows_{};
  std::uint64_t reached_cells_{};
  std::uint64_t dense_bytes_{};
  std::uint64_t bitmap_bytes_{};
  std::uint64_t best_bytes_{};
  std::array<std::uint64_t, 11> reached_decile_{};  // floor(10 * fraction)

  template <typename T>
  void add_rows(T const* tt,
                std::uint32_t const n_rows,
                std::uint32_t const n_dests,
                T const unreachable,
                bool const with_transfers,
                bool const with_via) {
    auto const cell = sizeof(T) + (with_transfers ? 1U : 0U) +
                      (with_via ? 4U : 0U);
    for (auto r = 0U; r != n_rows; ++r) {
      auto const* row = tt + static_cast<std::size_t>(r) * n_dests;
      auto reached = std::uint64_t{0U};
      for (auto d = 0U; d != n_dests; ++d) {
        reached += row[d] != unreachable ? 1U : 0U;
      }
      auto const dense = std::uint64_t{n_dests} * cell;
      auto const bitmap = (std::uint64_t{n_dests} + 7U) / 8U + reached * cell;
      ++rows_;
      reached_cells_ += reached;
      dense_bytes_ += dense;
      bitmap_bytes_ += bitmap;
      best_bytes_ += std::min(dense, bitmap);
      ++reached_decile_[n_dests == 0U ? 0U : (10U * reached) / n_dests];
    }
  }

  void merge(egress_stats_t const& o) {
    stops_with_egress_ += o.stops_with_egress_;
    stops_reached_ += o.stops_reached_;
    links_total_ += o.links_total_;
    links_within_cap_ += o.links_within_cap_;
    links_improved_ += o.links_improved_;
    rows_ += o.rows_;
    reached_cells_ += o.reached_cells_;
    dense_bytes_ += o.dense_bytes_;
    bitmap_bytes_ += o.bitmap_bytes_;
    best_bytes_ += o.best_bytes_;
    for (auto i = 0U; i != reached_decile_.size(); ++i) {
      reached_decile_[i] += o.reached_decile_[i];
    }
  }

  std::string json() const {
    return fmt::format(
        "{{\"stops_with_egress\":{},\"stops_reached\":{},\"links_total\":{},"
        "\"links_within_cap\":{},\"links_improved\":{},\"rows\":{},"
        "\"reached_cells\":{},\"dense_bytes\":{},\"bitmap_bytes\":{},"
        "\"best_bytes\":{},\"reached_decile\":[{}]}}",
        stops_with_egress_, stops_reached_, links_total_, links_within_cap_,
        links_improved_, rows_, reached_cells_, dense_bytes_, bitmap_bytes_,
        best_bytes_, fmt::join(reached_decile_, ","));
  }
};

// --row-digest: per record, wyhash (cista::hash) chained over the bytes a
// version-1 stream record holds (u32 origin, travel times, then the optional
// transfers and egress-stop blocks). Records are combined by a wrapping sum
// and an xor, so the digest does not depend on thread completion order.
struct row_digest_t {
  std::uint64_t sum_{};
  std::uint64_t xor_{};
  std::uint64_t records_{};

  template <typename T>
  static std::string_view bytes(std::vector<T> const& v) {
    return {reinterpret_cast<char const*>(v.data()), v.size() * sizeof(T)};
  }

  template <typename T>
  void add(std::uint32_t const o,
           std::vector<T> const& tt,
           std::vector<std::uint8_t> const& tr,
           std::vector<std::uint32_t> const& via) {
    auto h = cista::hash(
        std::string_view{reinterpret_cast<char const*>(&o), sizeof(o)});
    h = cista::hash(bytes(tt), h);
    if (!tr.empty()) {
      h = cista::hash(bytes(tr), h);
    }
    if (!via.empty()) {
      h = cista::hash(bytes(via), h);
    }
    sum_ += h;
    xor_ ^= h;
    ++records_;
  }

  void merge(row_digest_t const& o) {
    sum_ += o.sum_;
    xor_ ^= o.xor_;
    records_ += o.records_;
  }
};

// Version-2 record payload choice. A record covers n_departures x n_dests
// cells in [dep][dest] order. The bitmap payload is: u32 reached count, a
// presence bitmap of ceil(cells / 8) bytes (cell i is bit i % 8 of byte i / 8,
// set iff its travel time is not unreachable; padding bits zero), then the
// travel times of the reached cells in cell order, then their transfer codes
// and egress stops if the stream carries those blocks. Unreached cells always
// hold unreachable / 255 / 0xFFFFFFFF, so the dense record is recovered
// exactly. Returns true and fills `enc` when the bitmap payload is smaller.
template <typename T>
bool encode_bitmap(std::vector<T> const& tt,
                   std::vector<std::uint8_t> const& tr,
                   std::vector<std::uint32_t> const& via,
                   T const unreachable,
                   std::vector<char>& enc) {
  auto const cells = tt.size();
  auto reached = std::size_t{0U};
  for (auto const v : tt) {
    reached += v != unreachable ? 1U : 0U;
  }
  auto const cell_bytes =
      sizeof(T) + (tr.empty() ? 0U : 1U) + (via.empty() ? 0U : 4U);
  auto const bitmap_bytes = (cells + 7U) / 8U;
  auto const sparse = 4U + bitmap_bytes + reached * cell_bytes;
  if (sparse >= cells * cell_bytes || reached > 0xFFFFFFFFU) {
    return false;
  }
  enc.assign(sparse, char{0});
  auto const n = static_cast<std::uint32_t>(reached);
  std::memcpy(enc.data(), &n, 4U);
  auto* bits = reinterpret_cast<std::uint8_t*>(enc.data() + 4U);
  auto* vals = enc.data() + 4U + bitmap_bytes;
  auto* trs = vals + reached * sizeof(T);
  auto* vias = trs + (tr.empty() ? 0U : reached);
  auto r = std::size_t{0U};
  for (auto i = std::size_t{0U}; i != cells; ++i) {
    if (tt[i] == unreachable) {
      continue;
    }
    bits[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
    std::memcpy(vals + r * sizeof(T), &tt[i], sizeof(T));
    if (!tr.empty()) {
      trs[r] = static_cast<char>(tr[i]);
    }
    if (!via.empty()) {
      std::memcpy(vias + r * 4U, &via[i], 4U);
    }
    ++r;
  }
  return true;
}

struct stream_header {
  char magic_[8]{'V', 'R', 'T', 'T', 'M', 'A', 'T', 'X'};
  std::uint32_t version_{kFormatVersion};
  std::uint32_t n_origins_total_{};
  std::uint32_t n_dests_{};
  std::uint32_t n_departures_{};
  std::uint32_t origin_from_{};
  std::uint32_t origin_to_{};
  std::uint16_t max_travel_minutes_{};
  std::uint8_t
      flags_{};  // bit0 transfers, bit1 egress-stop, bit2 u16 travel times
  std::uint8_t target_{};  // 0: destinations, 1: stops
  std::uint32_t reserved_[5]{};
};
static_assert(sizeof(stream_header) == 56);

int cmd_route(args const& a) {
  auto const l = loaded{a.str("data")};
  auto const& tt = *l.d_->tt_;
  auto const target_stops = a.str("target", "dests") == "stops";
  constexpr auto kMaxPoints = std::int64_t{100'000'000};
  auto const n_origins =
      static_cast<std::uint32_t>(a.checked("n-origins", -1, 0, kMaxPoints));
  auto const n_dests =
      target_stops
          ? static_cast<std::uint32_t>(tt.n_locations())
          : static_cast<std::uint32_t>(a.checked("n-dests", -1, 0, kMaxPoints));
  auto const origin_from =
      static_cast<std::uint32_t>(a.checked("origin-from", 0, 0, n_origins));
  auto const origin_to = static_cast<std::uint32_t>(
      a.checked("origin-to", n_origins, origin_from, n_origins));
  // Optional explicit origin subset (comma-separated indices), else the range.
  auto origin_list = std::vector<std::uint32_t>{};
  if (a.has("origin-list")) {
    auto seen = std::unordered_set<std::uint32_t>{};
    auto const list = a.str("origin-list");  // keep alive: split returns views
    for (auto const x : split(list, ',')) {
      auto const v = parse_num<std::uint32_t>(x);
      if (v >= n_origins) {
        throw std::runtime_error("origin-list entry out of range");
      }
      if (!seen.insert(v).second) {
        throw std::runtime_error(
            fmt::format("duplicate origin-list entry {}", v));
      }
      origin_list.push_back(v);
    }
  } else {
    for (auto o = origin_from; o < origin_to; ++o) {
      origin_list.push_back(o);
    }
  }
  auto const dep_start = a.integer("dep-start-unix-minutes");
  auto const dep_count =
      static_cast<std::uint32_t>(a.checked("dep-count", 1, 1, 100'000));
  auto const dep_step = a.checked("dep-step-minutes", 1, 1, 1440);
  {
    // every departure must lie inside the loaded timetable interval
    auto const ext = tt.external_interval();
    auto const first =
        n::unixtime_t{n::i32_minutes{static_cast<std::int32_t>(dep_start)}};
    auto const last = n::unixtime_t{n::i32_minutes{
        static_cast<std::int32_t>(dep_start + (dep_count - 1) * dep_step)}};
    if (!ext.contains(first) || !ext.contains(last)) {
      throw std::runtime_error(
          "departures outside the loaded timetable interval");
    }
  }
  auto const day_base = [&](std::int64_t const m) {
    auto const t = n::unixtime_t{n::i32_minutes{static_cast<std::int32_t>(m)}};
    return n::day_idx_t{std::chrono::duration_cast<date::days>(
                            std::chrono::round<std::chrono::days>(t) -
                            tt.internal_interval().from_)
                            .count()};
  };
  auto const window_base = day_base(dep_start);
  auto const max_travel_arg = a.integer("max-travel-minutes");
  if (max_travel_arg < 0 ||
      max_travel_arg > static_cast<std::int64_t>(kMaxCap)) {
    throw std::runtime_error("max-travel-minutes must be in [0, 1440]");
  }
  auto const max_travel = static_cast<std::uint16_t>(max_travel_arg);
  auto const tt16 =
      max_travel > 253U;  // lossless u16 records above the u8 code range
  // Validate before narrowing: Nigiri allocates kMaxTransfers + 2 rounds and
  // the one_to_all result readers iterate up to the supplied value + 1.
  auto const max_transfers_arg = a.integer(
      "max-transfers", static_cast<std::int64_t>(n::routing::kMaxTransfers));
  if (max_transfers_arg < 0 ||
      max_transfers_arg >
          static_cast<std::int64_t>(n::routing::kMaxTransfers)) {
    throw std::runtime_error(
        fmt::format("--max-transfers must be in [0, {}], got {}",
                    n::routing::kMaxTransfers, max_transfers_arg));
  }
  auto const max_transfers = static_cast<std::uint8_t>(max_transfers_arg);
  auto const with_transfers = a.has("with-transfers");
  auto const reuse_state =
      a.has("reuse-state");  // experimental, see one_to_all_reuse
  auto const range = a.has("range");  // Range-RAPTOR over the departure window
  if (range) {
    // Nigiri's day base is the departure rounded to the nearest UTC day
    // (std::chrono::round: ties at exactly 12:00 UTC go to the even day
    // number). Name the first departure whose base differs from the start's.
    for (auto i = 0U; i != dep_count; ++i) {
      auto const m = dep_start + static_cast<std::int64_t>(i) * dep_step;
      if (day_base(m) != window_base) {
        auto const t =
            n::unixtime_t{n::i32_minutes{static_cast<std::int32_t>(m)}};
        auto const t0 =
            n::unixtime_t{n::i32_minutes{static_cast<std::int32_t>(dep_start)}};
        throw std::runtime_error(fmt::format(
            "--range: departure {} ({} UTC, unix minute {}) rounds to day {} "
            "but the window start ({} UTC) rounds to day {}; one Range-RAPTOR "
            "window needs one Nigiri day base, so end the window before "
            "departure {}",
            i, date::format("%F %R", t), m,
            date::format("%F", std::chrono::round<std::chrono::days>(t)),
            date::format("%F %R", t0),
            date::format("%F", std::chrono::round<std::chrono::days>(t0)), i));
      }
    }
  }
  // transit (default, = stock one-to-many): egress only from rounds k >= 1.
  // all: also from round 0, i.e. walk origin->stop->destination without a
  // vehicle.
  auto const egress_rounds = a.str("egress-rounds", "transit");
  if (egress_rounds != "transit" && egress_rounds != "all") {
    throw std::runtime_error("--egress-rounds must be transit|all");
  }
  auto const egress_round0 = egress_rounds == "all";
  auto const with_via = a.has("with-egress-stop");
  auto const n_threads =
      static_cast<unsigned>(a.checked("threads", 1, 1, 4096));
  auto const pin_threads = a.has("pin-threads");
  auto const cpus = allowed_cpus();
  if (pin_threads && cpus.size() < n_threads) {
    throw std::runtime_error(
        fmt::format("--pin-threads: {} threads but only {} allowed cpus",
                    n_threads, cpus.size()));
  }
  auto const out_path = a.str("out", "");
  auto const timing_path = a.str("timing-json", "");
  auto const discard =
      a.has("discard");  // timing-only: compute, write no records
  // --egress-stats: count egress work and row occupancy (diagnostics only,
  // outputs unchanged). --row-digest: order-independent digest of the records
  // a stream would hold, also with --discard.
  auto const egress_stats = a.has("egress-stats");
  auto const row_digest = a.has("row-digest");
  // --stream-encoding dense (default): version-1 stream. sparse: version 2,
  // each record dense or bitmap-encoded, whichever is smaller (see
  // encode_bitmap); `stream-decode` turns it back into the version-1 stream.
  auto const stream_encoding = a.str("stream-encoding", "dense");
  if (stream_encoding != "dense" && stream_encoding != "sparse") {
    throw std::runtime_error("--stream-encoding must be dense|sparse");
  }
  auto const sparse_stream = stream_encoding == "sparse";

  // r5r-style outputs (--r5r-out DIR): per origin, over the departures of the
  // window, percentiles of travel time to each destination (ttm.bin) and, with
  // --opportunities, accessibility per percentile and cutoff
  // (accessibility.csv).
  auto const r5r_dir = a.str("r5r-out", "");
  auto const emit_r5r = !r5r_dir.empty();
  if (out_path.empty() && !emit_r5r) {
    throw std::runtime_error("missing --out (or --r5r-out)");
  }
  auto percentiles = std::vector<int>{};
  auto cutoffs_min = std::vector<int>{};
  auto opportunities = opportunity_data{};
  auto decay_spec = std::string{"step"};
  if (emit_r5r) {
    percentiles = parse_strictly_increasing(a.str("percentiles", "50"), 1, 100,
                                            "percentiles");
    if (a.has("opportunities")) {
      cutoffs_min =
          parse_strictly_increasing(a.str("cutoffs"), 1, 240, "cutoffs");
      decay_spec = a.str("decay", "step");
      opportunities = read_opportunities(a, n_dests);
    }
  }
  if (emit_r5r) {
    r5::decay::parse(decay_spec);  // validate before routing
  }

  auto const t_conn = clock_t_::now();
  auto const access =
      csr::build(read_connectors(a.str("access")), n_origins, false);
  auto egress = csr{};
  auto direct = csr{};
  if (!target_stops) {
    egress =
        csr::build(read_connectors(a.str("egress")), tt.n_locations(), true);
    egress.sort_by_minutes();  // the egress loop stops at the cap
    if (a.has("direct")) {
      direct = csr::build(read_connectors(a.str("direct")), n_origins, false);
    } else {
      direct.begin_.assign(n_origins + 1U, 0U);
    }
  }
  a.check_all_used();
  auto const conn_ms = ms_since(t_conn);
  auto n_egress_stops = std::uint64_t{0U};
  for (auto s = std::size_t{0U}; s + 1U < egress.begin_.size(); ++s) {
    n_egress_stops += egress.begin_[s] != egress.begin_[s + 1U] ? 1U : 0U;
  }
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

  auto const write_stream = !out_path.empty();
  auto out = std::ofstream{};
  if (write_stream) {
    out.open(out_path, std::ios::binary);
  }
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
  h.version_ = sparse_stream ? kSparseFormatVersion : kFormatVersion;
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
  auto reuse_init_ns = std::atomic_uint64_t{0U};
  auto reuse_scan_ns = std::atomic_uint64_t{0U};
  auto reuse_finish_ns = std::atomic_uint64_t{0U};
  auto init_ns = std::atomic_uint64_t{0U};
  auto digest_ns = std::atomic_uint64_t{0U};
  auto bitmap_records = std::atomic_uint64_t{0U};
  auto stats_mutex = std::mutex{};
  auto stats = egress_stats_t{};
  auto digest = row_digest_t{};
  auto const t_route = clock_t_::now();

  auto r5r = std::optional<r5r_output>{};
  if (emit_r5r) {
    r5r.emplace(r5r_dir, n_dests, dep_count, percentiles, cutoffs_min,
                decay_spec, opportunities.values_, opportunities.names_);
  }

  // The travel-time buffer uses the narrowest record type (u8 for caps <= 253,
  // else u16): the egress loop is memory-bound and a u16 buffer slowed it by
  // ~70 %. The stats tag selects, at compile time, whether the egress loop
  // counts its work (--egress-stats); without it the loop has no counters.
  auto worker_t = [&](auto const type_tag, auto const stats_tag) {
    using tt_t = std::decay_t<decltype(type_tag)>;
    constexpr auto kStats = decltype(stats_tag)::value;
    auto rctx = reuse_ctx{};
    auto range_out = n::routing::raptor_state{};
    constexpr auto kUnreachableT = std::numeric_limits<tt_t>::max();
    auto const block = static_cast<std::size_t>(dep_count) * n_dests;
    auto tt_buf = std::vector<tt_t>(block);
    auto tr_buf = std::vector<std::uint8_t>(with_transfers ? block : 0U);
    auto via_buf = std::vector<std::uint32_t>(with_via ? block : 0U);
    auto st = egress_stats_t{};
    auto dg = row_digest_t{};
    auto my_init_ns = std::uint64_t{0U};
    auto my_digest_ns = std::uint64_t{0U};
    auto enc = std::vector<char>{};
    auto my_bitmap_records = std::uint64_t{0U};
    for (auto oi = next++; oi < origin_list.size(); oi = next++) {
      auto const o = origin_list[oi];
      auto const ti = clock_t_::now();
      std::fill(begin(tt_buf), end(tt_buf), kUnreachableT);
      std::fill(begin(tr_buf), end(tr_buf), kUnreachable);
      std::fill(begin(via_buf), end(via_buf), kNoStop);
      my_init_ns += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              clock_t_::now() - ti)
              .count());
      auto starts = std::vector<n::routing::offset>{};
      for (auto j = access.begin_[o]; j != access.begin_[o + 1U]; ++j) {
        starts.emplace_back(
            n::location_idx_t{access.value_[j]},
            n::duration_t{static_cast<n::duration_t::rep>(access.minutes_[j])},
            0U);
      }
      // --range: departures latest first through one raptor per origin
      auto ra = std::optional<range_raptor_t>{};
      for (auto ii = 0U; ii != dep_count; ++ii) {
        auto const di = range ? dep_count - 1U - ii : ii;
        auto* row_tt = tt_buf.data() + static_cast<std::size_t>(di) * n_dests;
        auto* row_tr =
            with_transfers
                ? tr_buf.data() + static_cast<std::size_t>(di) * n_dests
                : nullptr;
        auto* row_via =
            with_via ? via_buf.data() + static_cast<std::size_t>(di) * n_dests
                     : nullptr;
        // Departure-independent direct walk (destinations only).
        if (!target_stops) {
          for (auto j = direct.begin_[o]; j != direct.begin_[o + 1U]; ++j) {
            auto const d = direct.value_[j];
            auto const m = direct.minutes_[j];
            if (m <= max_travel && m < row_tt[d]) {
              row_tt[d] = static_cast<tt_t>(m);
              if (row_tr) row_tr[d] = kDirectWalk;
              if (row_via) row_via[d] = kNoStop;
            }
          }
        }
        if (starts.empty()) {
          continue;
        }
        auto const start_time = n::unixtime_t{n::i32_minutes{
            static_cast<std::int32_t>(dep_start + di * dep_step)}};
        auto q = n::routing::query{
            .start_time_ = start_time,
            .start_match_mode_ = n::routing::location_match_mode::kIntermodal,
            .start_ = starts,
            .max_transfers_ = max_transfers,
            .max_travel_time_ =
                n::duration_t{static_cast<n::duration_t::rep>(max_travel)},
            .prf_idx_ = 0U,
            .allowed_claszes_ = clasz_mask,
        };
        auto const tk = clock_t_::now();
        auto const fresh = (reuse_state || range)
                               ? n::routing::raptor_state{}
                               : n::routing::one_to_all<n::direction::kForward>(
                                     tt, nullptr, q);
        if (range) {
          if (!ra.has_value()) {
            make_range_raptor(tt, q, window_base, rctx, ra);
          }
          range_step(tt, q, *ra, rctx, range_out);
        }
        auto const& state =
            range ? range_out
                  : (reuse_state ? one_to_all_reuse(tt, q, rctx) : fresh);
        auto const tr = clock_t_::now();
        kernel_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tr - tk)
                .count());
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
              row_tt[s] = static_cast<tt_t>(f.duration_);
              if (row_tr) row_tr[s] = f.k_;
            }
          }
        } else {
          auto const& arrivals = state.one_to_all_arrivals_;
          auto const base_offset = static_cast<std::int64_t>(
              (std::chrono::time_point_cast<n::i32_minutes>(
                   std::chrono::round<std::chrono::days>(start_time)) -
               start_time)
                  .count());
          // Egress exactly as stock transit_durations: rounds k >= 1 only,
          // arrival at the stop (vehicle arrival, no final transfer buffer)
          // plus the egress connector; transfers = k - 1.
          for (auto s = 0U; s != tt.n_locations(); ++s) {
            if (egress.begin_[s] == egress.begin_[s + 1U]) {
              continue;
            }
            // Same values as nigiri::routing::for_each_one_to_all_round_time
            // (rounds 0..max_transfers+1, duration = arrival on the day base
            // round<days>(start) minus start), read directly to avoid a
            // std::function call per stop and round.
            auto best = std::numeric_limits<std::int64_t>::max();
            auto best_k = std::uint8_t{0U};
            for (auto k = egress_round0 ? 0U : 1U; k <= max_transfers + 1U;
                 ++k) {
              auto const v = arrivals[k][s];
              if (v != n::kInvalidDelta<n::direction::kForward>) {
                auto const d = base_offset + static_cast<std::int64_t>(v);
                if (d < best) {
                  best = d;
                  best_k = static_cast<std::uint8_t>(k);
                }
              }
            }
            if (best == std::numeric_limits<std::int64_t>::max()) {
              continue;
            }
            if constexpr (kStats) {
              ++st.stops_reached_;
              st.links_total_ += egress.begin_[s + 1U] - egress.begin_[s];
            }
            auto const transfers =
                best_k == 0U ? kWalkViaStop
                             : static_cast<std::uint8_t>(best_k - 1U);
            // Entries are sorted by minutes: the first one beyond the cap
            // ends this stop (exactly the entries the cap check skipped).
            for (auto j = egress.begin_[s]; j != egress.begin_[s + 1U]; ++j) {
              auto const total = best + egress.minutes_[j];
              if (total > max_travel) {
                break;
              }
              auto const d = egress.value_[j];
              if constexpr (kStats) {
                ++st.links_within_cap_;
              }
              // Lexicographic (travel time, transfers); a tie with a direct
              // walk keeps the walk (stock reports both, the reduction takes
              // the walk on ties).
              auto const better =
                  total < row_tt[d] ||
                  (row_tr != nullptr && total == row_tt[d] &&
                   row_tr[d] != kDirectWalk && transfers < row_tr[d]);
              if (better) {
                if constexpr (kStats) {
                  ++st.links_improved_;
                }
                row_tt[d] = static_cast<tt_t>(total);
                if (row_tr) row_tr[d] = transfers;
                if (row_via) row_via[d] = s;
              }
            }
          }
          if constexpr (kStats) {
            st.stops_with_egress_ += n_egress_stops;
          }
        }
        reduce_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock_t_::now() - tr)
                .count());
      }
      if constexpr (kStats) {
        st.add_rows(tt_buf.data(), dep_count, n_dests, kUnreachableT,
                    with_transfers, with_via);
      }
      if (row_digest) {
        auto const td = clock_t_::now();
        dg.add(o, tt_buf, tr_buf, via_buf);
        my_digest_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock_t_::now() - td)
                .count());
      }
      auto const tw = clock_t_::now();
      if (emit_r5r) {
        r5r->origin(o, tt_buf.data(), kUnreachableT);
      }
      if (!discard && write_stream) {
        auto const bitmap =
            sparse_stream &&
            encode_bitmap(tt_buf, tr_buf, via_buf, kUnreachableT, enc);
        auto const lock = std::lock_guard{write_mutex};
        out.write(reinterpret_cast<char const*>(&o), sizeof(o));
        if (sparse_stream) {
          auto const tag = bitmap ? kRecordBitmap : kRecordDense;
          out.write(reinterpret_cast<char const*>(&tag), 1U);
        }
        if (bitmap) {
          out.write(enc.data(), static_cast<std::streamsize>(enc.size()));
          ++my_bitmap_records;
        } else {
          out.write(reinterpret_cast<char const*>(tt_buf.data()),
                    static_cast<std::streamsize>(tt_buf.size() * sizeof(tt_t)));
          if (with_transfers) {
            out.write(reinterpret_cast<char const*>(tr_buf.data()),
                      static_cast<std::streamsize>(tr_buf.size()));
          }
          if (with_via) {
            out.write(reinterpret_cast<char const*>(via_buf.data()),
                      static_cast<std::streamsize>(via_buf.size() * 4U));
          }
        }
      }
      write_ns += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() -
                                                               tw)
              .count());
    }
    reuse_init_ns += rctx.init_ns_;
    reuse_scan_ns += rctx.scan_ns_;
    reuse_finish_ns += rctx.finish_ns_;
    init_ns += my_init_ns;
    digest_ns += my_digest_ns;
    bitmap_records += my_bitmap_records;
    auto const lock = std::lock_guard{stats_mutex};
    stats.merge(st);
    digest.merge(dg);
  };
  auto worker = [&]() {
    auto const run = [&](auto const type_tag) {
      if (egress_stats) {
        worker_t(type_tag, std::true_type{});
      } else {
        worker_t(type_tag, std::false_type{});
      }
    };
    if (tt16) {
      run(std::uint16_t{});
    } else {
      run(std::uint8_t{});
    }
  };
  auto threads = std::vector<std::thread>{};
  for (auto i = 0U; i != std::max(1U, n_threads); ++i) {
    threads.emplace_back([&, i]() {
      if (pin_threads) {
        pin_current_thread(cpus[i]);
      }
      worker();
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  if (write_stream) {
    out.close();
    if (!out) {
      throw std::runtime_error("write failed");
    }
  }
  if (emit_r5r) {
    r5r->close();
  }
  auto const route_ms = ms_since(t_route);
  auto const json = fmt::format(
      "{{\"tool\":\"motis-accessibility\",\"format_version\":{},"
      "\"target\":\"{}\",\"egress_rounds\":\"{}\",\"origins\":[{},{}],\"n_"
      "dests\":{},\"n_departures\":{},"
      "\"threads\":{},\"discard\":{},\"max_travel_minutes\":{},\"max_"
      "transfers\":{},\"queries\":{},\"load_ms\":{:.3f},\"connectors_ms\":{:."
      "3f},"
      "\"route_wall_ms\":{:.3f},\"kernel_thread_ms\":{:.3f},"
      "\"reduce_thread_ms\":{:.3f},\"write_thread_ms\":{:.3f},"
      "\"peak_rss_kb\":{},\"n_locations\":{},\"reuse_state\":{},\"nigiri_"
      "reusable_one_to_all\":{},"
      "\"pin_threads\":{},\"allowed_cpus\":\"{}\",\"range\":{},"
      "\"reuse_init_thread_ms\":{:.3f},\"reuse_scan_thread_ms\":{:.3f},\"reuse_"
      "finish_thread_ms\":{:.3f},\"row_init_thread_ms\":{:.3f},"
      "\"stream_encoding\":\"{}\",\"bitmap_records\":{}{}{}}}",
      h.version_, target_stops ? "stops" : "dests", egress_rounds,
      origin_from, origin_to, n_dests, dep_count, n_threads, discard,
      max_travel, static_cast<int>(max_transfers), n_queries.load(), l.load_ms_,
      conn_ms, route_ms, kernel_ns.load() / 1e6, reduce_ns.load() / 1e6,
      write_ns.load() / 1e6, peak_rss_kb(), tt.n_locations(), reuse_state,
      kHasReusableOneToAll<n::timetable>, pin_threads, fmt::join(cpus, " "),
      range, reuse_init_ns.load() / 1e6, reuse_scan_ns.load() / 1e6,
      reuse_finish_ns.load() / 1e6, init_ns.load() / 1e6, stream_encoding,
      bitmap_records.load(),
      row_digest ? fmt::format(",\"row_digest\":{{\"sum\":\"{:016x}\","
                               "\"xor\":\"{:016x}\",\"records\":{},"
                               "\"thread_ms\":{:.3f}}}",
                               digest.sum_, digest.xor_, digest.records_,
                               digest_ns.load() / 1e6)
                 : std::string{},
      egress_stats ? ",\"egress_stats\":" + stats.json() : std::string{});
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
    } else if (cmd == "export-tt") {
      return cmd_export_tt(a);
    } else if (cmd == "connectors") {
      return cmd_connectors(a);
    } else if (cmd == "direct") {
      return cmd_direct(a);
    } else if (cmd == "route") {
      return cmd_route(a);
    } else if (cmd == "r5r-reduce") {
      return cmd_r5r_reduce(a);
    } else if (cmd == "r5r-selftest") {
      return cmd_r5r_selftest(a);
    } else if (cmd == "stream-decode") {
      return cmd_stream_decode(a);
    }
    fmt::println(stderr, "unknown command {}", cmd);
    return 1;
  } catch (std::exception const& e) {
    fmt::println(stderr, "error: {}", e.what());
    return 2;
  }
}
