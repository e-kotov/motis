#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "osr/location.h"
#include "osr/routing/profile.h"

#include "nigiri/routing/query.h"

#include "motis-api/motis-api.h"

#include "motis/osr/parameters.h"

namespace motis::ep {

// Street offsets of the "many" places of one-to-many-intermodal requests.
// They depend only on the place and the street-routing parameters, not on the
// "one" place or the time, so batches of requests with the same many places
// (e.g. accessibility grids) compute them once instead of once per request.
//
// Only offsets that depend on static data alone are cached: a coordinate (not
// a stop), modes WALK / BIKE / CAR, a non-wheelchair pedestrian profile, no
// rental and no real-time timetable. Everything that enters the street search
// is part of the key. The cache is bounded by its number of entries and does
// not evict: once full, further places are computed as before and not stored.
struct one_to_many_offsets_cache {
  struct key {
    bool operator==(key const&) const = default;

    osr::location place_;
    osr::direction dir_;
    std::vector<api::ModeEnum> modes_;
    osr_parameters osr_params_;
    api::PedestrianProfileEnum pedestrian_profile_;
    api::ElevationCostsEnum elevation_costs_;
    std::chrono::seconds max_;
    double max_matching_distance_;
  };

  struct key_hash {
    std::size_t operator()(key const& k) const noexcept {
      auto h = std::size_t{0xcbf29ce484222325ULL};
      auto const mix = [&](std::uint64_t const x) {
        h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
      };
      mix(std::bit_cast<std::uint64_t>(k.place_.pos_.lat_));
      mix(std::bit_cast<std::uint64_t>(k.place_.pos_.lng_));
      mix(to_idx(k.place_.lvl_));
      mix(static_cast<std::uint64_t>(k.dir_));
      for (auto const m : k.modes_) {
        mix(static_cast<std::uint64_t>(m));
      }
      mix(std::bit_cast<std::uint32_t>(k.osr_params_.pedestrian_speed_));
      mix(std::bit_cast<std::uint32_t>(k.osr_params_.cycling_speed_));
      mix(static_cast<std::uint64_t>(k.pedestrian_profile_));
      mix(static_cast<std::uint64_t>(k.elevation_costs_));
      mix(static_cast<std::uint64_t>(k.max_.count()));
      mix(std::bit_cast<std::uint64_t>(k.max_matching_distance_));
      return h;
    }
  };

  using offsets_t = std::vector<nigiri::routing::offset>;

  static bool is_cacheable(std::vector<api::ModeEnum> const& modes,
                           api::PedestrianProfileEnum const pedestrian_profile,
                           osr_parameters const& osr_params) {
    return pedestrian_profile != api::PedestrianProfileEnum::WHEELCHAIR &&
           !osr_params.use_wheelchair_ &&
           std::all_of(begin(modes), end(modes), [](api::ModeEnum const m) {
             return m == api::ModeEnum::WALK || m == api::ModeEnum::BIKE ||
                    m == api::ModeEnum::CAR;
           });
  }

  // Returns the cached offsets for `k`, or computes them with `compute`
  // (outside the lock) and stores them if there is room.
  template <typename Compute>
  offsets_t get_or_compute(std::size_t const max_entries,
                           key const& k,
                           Compute&& compute) {
    if (max_entries == 0U) {
      return compute();
    }
    {
      auto const lock = std::shared_lock{mutex_};
      if (auto const it = entries_.find(k); it != end(entries_)) {
        hits_.fetch_add(1U, std::memory_order_relaxed);
        return it->second;
      }
    }
    misses_.fetch_add(1U, std::memory_order_relaxed);
    auto offsets = compute();
    {
      auto const lock = std::unique_lock{mutex_};
      if (entries_.size() < max_entries) {
        entries_.try_emplace(k, offsets);
      }
    }
    return offsets;
  }

  std::size_t size() const {
    auto const lock = std::shared_lock{mutex_};
    return entries_.size();
  }

  std::atomic_uint64_t hits_{0U};
  std::atomic_uint64_t misses_{0U};

private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<key, offsets_t, key_hash> entries_;
};

}  // namespace motis::ep
