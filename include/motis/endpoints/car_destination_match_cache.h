#pragma once

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "osr/lookup.h"
#include "osr/routing/profiles/car.h"

#include "motis/metrics_registry.h"

namespace motis {

// One instance belongs to one loaded street graph. Only the goal side of
// the static CAR matrix is prepared here, with default car parameters, no
// blocked nodes and no routing time.
class car_destination_match_cache {
public:
  car_destination_match_cache(osr::lookup const& lookup,
                              std::size_t const max_bytes,
                              metrics_registry& metrics)
      : lookup_{lookup}, max_bytes_{max_bytes}, metrics_{metrics} {}

  // A null result requests normal routing. A non-null result contains one
  // bucket per input coordinate, including empty buckets and duplicates.
  std::shared_ptr<osr::match_result const> get(
      std::vector<osr::location> const& destinations,
      osr::direction const direction,
      double const radius) {
    if (max_bytes_ == 0U || destinations.empty() || !std::isfinite(radius) ||
        radius <= 0.0 ||
        !std::ranges::all_of(destinations, [](osr::location const& location) {
          return std::abs(location.pos_.lat()) <= 90.0 &&
                 std::abs(location.pos_.lng()) <= 180.0;
        })) {
      return {};
    }

    auto const start = std::chrono::steady_clock::now();
    {
      auto const lock = std::scoped_lock{mutex_};
      if (auto const hit = find(destinations, direction, radius); hit) {
        metrics_.car_snap_cache_hits_.Increment();
        return hit;
      }
    }

    auto prepared = std::make_shared<osr::match_result>();
    for (auto const& destination : destinations) {
      lookup_.match<osr::car>(osr::car::parameters{}, destination, true,
                              direction, radius, nullptr, *prepared);
    }
    metrics_.car_snap_cache_misses_.Increment();
    metrics_.car_snap_cache_coordinates_.Increment(
        static_cast<double>(destinations.size()));
    metrics_.car_snap_cache_preparation_seconds_.Increment(
        std::chrono::duration<double>{std::chrono::steady_clock::now() - start}
            .count());

    auto entry = preparation{destinations, direction, radius, prepared, 0U};
    auto const& m = *prepared;
    entry.bytes_ = sizeof(preparation) + sizeof(osr::match_result) +
                   entry.destinations_.capacity() * sizeof(osr::location) +
                   bytes(m.begin_) + bytes(m.lvl_) + bytes(m.penalty_ref_) +
                   bytes(m.dist_to_way_) + bytes(m.way_) + bytes(m.nodes_) +
                   bytes(m.state_);
    if (entry.bytes_ <= max_bytes_) {
      auto const lock = std::scoped_lock{mutex_};
      // Concurrent cold requests may prepare independently. Only one result
      // for a key occupies the cache, and no thread matches under the lock.
      if (auto const hit = find(destinations, direction, radius); hit) {
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

private:
  static constexpr auto kMaxPreparations = 8U;

  struct preparation {
    std::vector<osr::location> destinations_;
    osr::direction direction_;
    double radius_;
    std::shared_ptr<osr::match_result const> matches_;
    std::size_t bytes_;
  };

  static bool same_coordinate(osr::location const& a, osr::location const& b) {
    auto const bits = [](double const x) { return std::bit_cast<std::uint64_t>(x); };
    return a.lvl_ == b.lvl_ && bits(a.pos_.lat()) == bits(b.pos_.lat()) &&
           bits(a.pos_.lng()) == bits(b.pos_.lng());
  }

  static std::size_t bytes(auto const& vec) {
    return vec.allocated_size_ * sizeof(*vec.data());
  }

  std::shared_ptr<osr::match_result const> find(
      std::vector<osr::location> const& destinations,
      osr::direction const direction,
      double const radius) const {
    for (auto const& entry : entries_) {
      if (entry.direction_ == direction && entry.radius_ == radius &&
          std::ranges::equal(entry.destinations_, destinations, same_coordinate)) {
        return entry.matches_;
      }
    }
    return {};
  }

  osr::lookup const& lookup_;
  std::size_t const max_bytes_;
  metrics_registry& metrics_;
  std::mutex mutex_;
  std::deque<preparation> entries_;
  std::size_t cached_bytes_{0U};
};

}  // namespace motis
