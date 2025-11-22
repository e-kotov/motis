#include "nigiri/routing/one_to_all.h"
#include "nigiri/routing/query.h"
#include "nigiri/timetable.h"

#include "osr/lookup.h"
#include "osr/platforms.h"
#include "osr/routing/profile.h"
#include "osr/routing/route.h"
#include "osr/ways.h"

#include "motis/adr_extend_tt.h"
#include "motis/config.h"
#include "motis/endpoints/one_to_many.h"
#include "motis/endpoints/routing.h"
#include "motis/match_platforms.h"
#include "motis/place.h"
#include "motis/tag_lookup.h"
#include "motis/timetable/modes_to_clasz_mask.h"

namespace motis::ep {

namespace n = nigiri;

api::oneToMany_response one_to_many_transit(
    api::oneToMany_params const& query,
    nigiri::timetable const& tt,
    tag_lookup const& tags,
    osr::ways const& w,
    osr::lookup const& l,
    osr::platforms const* pl,
    osr::elevation_storage const* elevations,
    platform_matches_t const* matches,
    adr_ext const* ae,
    tz_map_t const* tz,
    config const& config) {
  auto const time = std::chrono::time_point_cast<std::chrono::minutes>(
      *query.time_.value_or(openapi::now()));
  auto const max_travel_time = n::duration_t{query.max_};

  auto const one = get_place(&tt, &tags, query.one_);
  auto const one_modes = std::vector<api::ModeEnum>{api::ModeEnum::WALK};
  auto const one_max_time = std::min(
      std::chrono::duration_cast<std::chrono::seconds>(max_travel_time),
      std::chrono::seconds{
          config.limits_.value().street_routing_max_prepost_transit_seconds_});
  auto const one_dir =
      query.arriveBy_ ? osr::direction::kBackward : osr::direction::kForward;

  auto const r =
      routing{config,  &w,      &l,      pl,      elevations, &tt,     nullptr,
              &tags,   nullptr, nullptr, matches, nullptr,    nullptr, nullptr,
              nullptr, nullptr, nullptr, nullptr, nullptr,    nullptr};

  auto prepare_stats = std::map<std::string, std::uint64_t>{};
  auto gbfs_rd = gbfs::gbfs_routing_data{&w, &l, nullptr};
  auto q = n::routing::query{
      .start_time_ = time,
      .start_match_mode_ = get_match_mode(one),
      .start_ = r.get_offsets(
          nullptr, one, one_dir, one_modes, std::nullopt, std::nullopt,
          std::nullopt, std::nullopt, false, get_osr_parameters(query),
          query.pedestrianProfile_, query.elevationCosts_, one_max_time,
          query.maxMatchingDistance_, gbfs_rd, prepare_stats),
      .td_start_ = r.get_td_offsets(
          nullptr, nullptr, one, one_dir, one_modes, get_osr_parameters(query),
          query.pedestrianProfile_, query.elevationCosts_,
          query.maxMatchingDistance_, one_max_time, time, prepare_stats),
      .max_transfers_ = n::routing::kMaxTransfers,
      .max_travel_time_ = max_travel_time,
      .prf_idx_ = 0U,
      .allowed_claszes_ = n::routing::all_clasz_mask(),
      .require_bike_transport_ = false,
      .require_car_transport_ = false,
      .transfer_time_settings_ = {},
  };

  auto const state =
      query.arriveBy_
          ? n::routing::one_to_all<n::direction::kBackward>(tt, nullptr, q)
          : n::routing::one_to_all<n::direction::kForward>(tt, nullptr, q);

  auto const many = utl::to_vec(query.many_, [](auto&& x) {
    auto const y = parse_location(x, ';');
    utl::verify(y.has_value(), "{} is not a valid geo coordinate", x);
    return *y;
  });

  return utl::to_vec(many, [&](osr::location const& loc) {
    auto const offsets = r.get_offsets(
        nullptr, loc,
        query.arriveBy_ ? osr::direction::kForward : osr::direction::kBackward,
        one_modes, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        false, get_osr_parameters(query), query.pedestrianProfile_,
        query.elevationCosts_, one_max_time, query.maxMatchingDistance_,
        gbfs_rd, prepare_stats);

    auto min_duration =
        n::duration_t{std::numeric_limits<n::duration_t::rep>::max()};

    for (auto const& o : offsets) {
      auto const time_at_stop = query.arriveBy_
                                    ? state.get_best<0>()[o.target_][0].arr_
                                    : state.get_best<0>()[o.target_][0].dep_;
      if (time_at_stop == n::kInvalidTime) {
        continue;
      }
      auto const duration =
          query.arriveBy_ ? (time - time_at_stop) : (time_at_stop - time);
      min_duration = std::min(min_duration, duration + o.duration());
    }

    return min_duration !=
                   n::duration_t{std::numeric_limits<n::duration_t::rep>::max()}
               ? api::Duration{.duration_ =
                                   std::chrono::duration_cast<
                                       std::chrono::seconds>(min_duration)
                                       .count()}
               : api::Duration{};
  });
}

api::oneToMany_response one_to_many::operator()(
    boost::urls::url_view const& url) const {
  auto const query = api::oneToMany_params{url.params()};
  if (query.mode_ == api::ModeEnum::TRANSIT) {
    return one_to_many_transit(query, tt_, tags_, w_, l_, pl_, elevations_,
                               matches_, ae_, tz_, config_);
  }
  return one_to_many_handle_request(query, w_, l_, elevations_);
}

}  // namespace motis::ep
