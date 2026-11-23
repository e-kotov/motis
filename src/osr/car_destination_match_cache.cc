#include "motis/osr/car_destination_match_cache.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>

#include "cista/reflection/to_tuple.h"

#include "motis/metrics_registry.h"

namespace motis {

namespace {

bool same_coordinate(osr::location const& a, osr::location const& b) {
  auto const bits = [](double const x) {
    return std::bit_cast<std::uint64_t>(x);
  };
  return a.lvl_ == b.lvl_ && bits(a.pos_.lat()) == bits(b.pos_.lat()) &&
         bits(a.pos_.lng()) == bits(b.pos_.lng());
}

std::size_t bytes(auto const& vec) {
  return vec.allocated_size_ * sizeof(*vec.data());
}

}  // namespace

car_destination_match_cache::car_destination_match_cache(
    osr::lookup const& lookup,
    std::size_t const max_bytes,
    metrics_registry& metrics)
    : lookup_{lookup}, max_bytes_{max_bytes}, metrics_{metrics} {}

bool car_destination_match_cache::can_prepare(
    std::vector<osr::location> const& destinations, double const radius) const {
  return max_bytes_ != 0U && !destinations.empty() && std::isfinite(radius) &&
         radius > 0.0 &&
         std::ranges::all_of(destinations, [](osr::location const& location) {
           return std::abs(location.pos_.lat()) <= 90.0 &&
                  std::abs(location.pos_.lng()) <= 180.0;
         });
}

std::shared_ptr<osr::match_result const> car_destination_match_cache::get(
    osr::car::parameters const& parameters,
    std::vector<osr::location> const& destinations,
    osr::direction const direction,
    double const radius) {
  auto const start = std::chrono::steady_clock::now();
  {
    auto const lock = std::scoped_lock{mutex_};
    if (auto const hit = find(parameters, destinations, direction, radius);
        hit) {
      metrics_.car_snap_cache_hits_.Increment();
      return hit;
    }
  }

  auto prepared = std::make_shared<osr::match_result>();
  for (auto const& destination : destinations) {
    lookup_.match<osr::car>(parameters, destination, true, direction, radius,
                            nullptr, *prepared);
  }
  metrics_.car_snap_cache_misses_.Increment();
  metrics_.car_snap_cache_coordinates_.Increment(
      static_cast<double>(destinations.size()));
  metrics_.car_snap_cache_preparation_seconds_.Increment(
      std::chrono::duration<double>{std::chrono::steady_clock::now() - start}
          .count());

  auto const& m = *prepared;
  auto const payload = sizeof(preparation) + sizeof(osr::match_result) +
                       bytes(m.begin_) + bytes(m.lvl_) + bytes(m.penalty_ref_) +
                       bytes(m.dist_to_way_) + bytes(m.way_) + bytes(m.nodes_) +
                       bytes(m.state_);
  if (payload > max_bytes_ ||
      destinations.size() > (max_bytes_ - payload) / sizeof(osr::location)) {
    return prepared;
  }

  auto entry =
      preparation{parameters, destinations, direction, radius, prepared, 0U};
  entry.bytes_ =
      payload + entry.destinations_.capacity() * sizeof(osr::location);
  if (entry.bytes_ <= max_bytes_) {
    auto const lock = std::scoped_lock{mutex_};
    // Concurrent cold requests may prepare independently. Only one result
    // for a key occupies the cache, and no thread matches under the lock.
    if (auto const hit = find(parameters, destinations, direction, radius);
        hit) {
      return hit;
    }
    while (entries_.size() == kMaxPreparations ||
           cached_bytes_ > max_bytes_ - entry.bytes_) {
      cached_bytes_ -= entries_.front().bytes_;
      entries_.pop_front();
    }
    entries_.push_back(std::move(entry));
    cached_bytes_ += entries_.back().bytes_;
    metrics_.car_snap_cache_bytes_.Set(static_cast<double>(cached_bytes_));
  }
  return prepared;
}

std::shared_ptr<osr::match_result const> car_destination_match_cache::find(
    osr::car::parameters const& parameters,
    std::vector<osr::location> const& destinations,
    osr::direction const direction,
    double const radius) const {
  for (auto const& entry : entries_) {
    if (entry.direction_ == direction && entry.radius_ == radius &&
        cista::to_tuple(entry.parameters_) == cista::to_tuple(parameters) &&
        std::ranges::equal(entry.destinations_, destinations,
                           same_coordinate)) {
      return entry.matches_;
    }
  }
  return {};
}

}  // namespace motis
