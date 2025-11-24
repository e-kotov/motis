#include "gtest/gtest.h"

#include "motis/config.h"
#include "motis/data.h"
#include "motis/endpoints/one_to_many.h"
#include "motis/import.h"
#include "motis/types.h"

#include "./util.h"

using namespace motis;
using namespace motis::ep;
using namespace std::string_view_literals;
using namespace date;

constexpr auto const kGTFS = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,Deutsche Bahn,https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_lat,stop_lon,location_type,parent_station,platform_code
A,Station A,50.0,8.0,1,,
A1,Station A,50.0,8.0,0,A,1
B,Station B,50.1,8.1,1,,
B1,Station B,50.1,8.1,0,B,1
C,Station C,50.2,8.2,1,,
C1,Station C,50.2,8.2,0,C,1

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_desc,route_type
R1,DB,R1,,,109

# trips.txt
route_id,service_id,trip_id,trip_headsign,block_id
R1,S1,T1,,

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence,pickup_type,drop_off_type
T1,08:00:00,08:00:00,A1,1,0,0
T1,09:00:00,09:00:00,B1,2,0,0
T1,10:00:00,10:00:00,C1,3,0,0

# calendar_dates.txt
service_id,date,exception_type
S1,20190501,1
)"sv;

TEST(motis, one_to_many_transit) {
  auto ec = std::error_code{};
  std::filesystem::remove_all("test/data", ec);

  auto const c =
      config{.timetable_ =
                 config::timetable{
                     .first_day_ = "2019-05-01",
                     .num_days_ = 2,
                     .datasets_ = {{"test", {.path_ = std::string{kGTFS}}}}},
             .street_routing_ = true,
             .osr_footpath_ = true};
  auto d = import(c, "test/data", true);
  d.init_rtt(date::sys_days{2019_y / May / 1});

  auto const one_to_many = utl::init_from<ep::one_to_many>(d).value();

  // One: Near Station A (50.0, 8.0)
  // Many: Near Station B (50.1, 8.1), Near Station C (50.2, 8.2)
  auto const res = one_to_many(
      "?one=50.0,8.0"
      "&many=50.1,8.1;50.2,8.2"
      "&mode=TRANSIT"
      "&time=2019-05-01T07:50Z"
      "&max=7200");  // 2 hours

  ASSERT_EQ(2, res.size());
  // A -> B: 08:00 -> 09:00 = 60 min + walk time.
  // Wait time at A: 07:50 -> 08:00 = 10 min.
  // Total travel time approx 70 min (4200 sec).
  EXPECT_NEAR(4200, res[0].duration_, 600);  // Allow some tolerance for walking

  // A -> C: 08:00 -> 10:00 = 120 min + walk time.
  // Wait time 10 min.
  // Total approx 130 min (7800 sec).
  EXPECT_NEAR(7800, res[1].duration_, 600);
}
