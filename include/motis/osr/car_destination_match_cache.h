#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "osr/lookup.h"
#include "osr/routing/profiles/car.h"

namespace motis {

struct metrics_registry;

// One instance per street graph. Reuses ordered goal matches for the static
// CAR matrix, with no blocked nodes or routing time.
struct car_destination_match_cache {
  car_destination_match_cache(osr::lookup const&,
                              std::size_t max_bytes,
                              metrics_registry&);

  bool can_prepare(std::vector<osr::location> const&, double radius) const;

  // Preserve the order, duplicates and empty buckets of the destination list.
  std::shared_ptr<osr::match_result const> get(
      osr::car::parameters const&,
      std::vector<osr::location> const&,
      osr::direction,
      double radius);

private:
  static constexpr auto kMaxPreparations = 8U;

  struct preparation {
    osr::car::parameters parameters_;
    std::vector<osr::location> destinations_;
    osr::direction direction_;
    double radius_;
    std::shared_ptr<osr::match_result const> matches_;
    std::size_t bytes_;
  };

  std::shared_ptr<osr::match_result const> find(
      osr::car::parameters const&,
      std::vector<osr::location> const&,
      osr::direction,
      double radius) const;

  osr::lookup const& lookup_;
  std::size_t const max_bytes_;
  metrics_registry& metrics_;
  std::mutex mutex_;
  std::deque<preparation> entries_;
  std::size_t cached_bytes_{0U};
};

}  // namespace motis
