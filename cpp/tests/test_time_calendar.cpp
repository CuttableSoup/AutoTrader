#include "common/calendar.hpp"
#include "common/time.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace at;

TEST_CASE("ISO date/time round trip", "[time]") {
    auto d = parse_date("2026-09-15");
    REQUIRE(d);
    CHECK(iso_date(*d) == "2026-09-15");
    CHECK(!parse_date("2026-13-01"));
    CHECK(!parse_date("garbage"));
    auto t = parse_iso_utc("2026-09-15T13:30:00.123Z");
    REQUIRE(t);
    CHECK(iso_utc(*t) == "2026-09-15T13:30:00.123Z");
    auto t2 = parse_iso_utc("2026-09-15T09:30:00-04:00");
    REQUIRE(t2);
    CHECK(iso_utc(*t2) == "2026-09-15T13:30:00.000Z");
    CHECK(!parse_iso_utc("2026-09-15"));
}

TEST_CASE("New York DST rule", "[time]") {
    // 2026: DST starts Mar 8, ends Nov 1.
    CHECK(ny_is_dst(*parse_iso_utc("2026-07-01T12:00:00Z")));
    CHECK(!ny_is_dst(*parse_iso_utc("2026-01-15T12:00:00Z")));
    CHECK(!ny_is_dst(*parse_iso_utc("2026-03-08T06:59:00Z")));
    CHECK(ny_is_dst(*parse_iso_utc("2026-03-08T07:00:00Z")));
    CHECK(ny_is_dst(*parse_iso_utc("2026-11-01T05:59:00Z")));
    CHECK(!ny_is_dst(*parse_iso_utc("2026-11-01T06:00:00Z")));
    NyLocal ny = to_ny(*parse_iso_utc("2026-09-15T13:30:00Z"));
    CHECK(iso_date(ny.date) == "2026-09-15");
    CHECK(ny.hour == 9);
    CHECK(ny.minute == 30);
    CHECK(iso_utc(from_ny(make_date(2026, 9, 15), 9, 30)) == "2026-09-15T13:30:00.000Z");
    CHECK(iso_utc(from_ny(make_date(2026, 1, 15), 16, 0)) == "2026-01-15T21:00:00.000Z");
}

TEST_CASE("NYSE holidays and observed rules", "[calendar]") {
    const auto& cal = nyse();
    CHECK(cal.is_holiday(make_date(2026, 1, 1)));
    CHECK(cal.is_holiday(make_date(2026, 1, 19)));   // MLK
    CHECK(cal.is_holiday(make_date(2026, 2, 16)));   // Presidents
    CHECK(cal.is_holiday(make_date(2026, 4, 3)));    // Good Friday 2026
    CHECK(cal.is_holiday(make_date(2026, 5, 25)));   // Memorial
    CHECK(cal.is_holiday(make_date(2026, 6, 19)));   // Juneteenth
    CHECK(cal.is_holiday(make_date(2026, 7, 3)));    // Independence Day observed (Jul 4 is Saturday)
    CHECK(cal.is_holiday(make_date(2026, 9, 7)));    // Labor
    CHECK(cal.is_holiday(make_date(2026, 11, 26)));  // Thanksgiving
    CHECK(cal.is_holiday(make_date(2026, 12, 25)));
    CHECK(cal.is_holiday(make_date(2025, 1, 9)));    // Carter
    CHECK(cal.is_holiday(make_date(2021, 12, 24)));  // Christmas observed Friday
    CHECK(!cal.is_holiday(make_date(2021, 12, 31))); // New Year's 2022 on Saturday: NOT observed on Friday
    CHECK(cal.is_holiday(make_date(2023, 1, 2)));    // New Year's 2023 observed Monday
    CHECK(cal.is_trading_day(make_date(2026, 9, 15)));
    CHECK(!cal.is_trading_day(make_date(2026, 9, 13))); // Sunday
    CHECK(easter_sunday(2026) == make_date(2026, 4, 5));
    CHECK(easter_sunday(2024) == make_date(2024, 3, 31));
}

TEST_CASE("half days", "[calendar]") {
    const auto& cal = nyse();
    CHECK(cal.is_half_day(make_date(2026, 11, 27)));   // day after Thanksgiving
    CHECK(cal.is_half_day(make_date(2026, 12, 24)));   // Thursday
    CHECK(cal.is_half_day(make_date(2025, 7, 3)));     // Thursday, Jul 4 Friday
    CHECK(!cal.is_half_day(make_date(2026, 7, 2)));
    CHECK(cal.session_times(make_date(2026, 11, 27)).close_minutes == 13 * 60);
    CHECK(cal.session_times(make_date(2026, 9, 15)).close_minutes == 16 * 60);
}

TEST_CASE("session arithmetic", "[calendar]") {
    const auto& cal = nyse();
    Date fri = make_date(2026, 9, 11);
    CHECK(cal.next_trading_day(fri) == make_date(2026, 9, 14));
    CHECK(cal.prev_trading_day(make_date(2026, 9, 14)) == fri);
    CHECK(cal.add_sessions(fri, 1) == make_date(2026, 9, 14));
    CHECK(cal.add_sessions(make_date(2026, 9, 12), 0) == make_date(2026, 9, 14)); // Saturday snaps forward
    CHECK(cal.add_sessions(fri, -1) == make_date(2026, 9, 10));
    CHECK(cal.sessions_between(fri, make_date(2026, 9, 18)) == 5);
    CHECK(cal.sessions_between(make_date(2026, 9, 18), fri) == -5);
    CHECK(cal.add_sessions(make_date(2026, 9, 4), 1) == make_date(2026, 9, 8)); // over Labor Day
    auto s = cal.sessions(make_date(2026, 1, 1), make_date(2026, 12, 31));
    CHECK(s.size() == 251);
}

TEST_CASE("session_for maps timestamps to sessions", "[calendar]") {
    const auto& cal = nyse();
    CHECK(cal.session_for(*parse_iso_utc("2026-09-15T14:00:00Z")) == make_date(2026, 9, 15));
    CHECK(cal.session_for(*parse_iso_utc("2026-09-15T12:00:00Z")) == make_date(2026, 9, 14)); // pre-open -> prior session
    CHECK(cal.session_for(*parse_iso_utc("2026-09-15T22:00:00Z")) == make_date(2026, 9, 15)); // after close
    CHECK(cal.session_for(*parse_iso_utc("2026-09-13T15:00:00Z")) == make_date(2026, 9, 11)); // Sunday
    CHECK(cal.is_open_at(*parse_iso_utc("2026-09-15T14:00:00Z")));
    CHECK(!cal.is_open_at(*parse_iso_utc("2026-09-15T20:00:00Z")));
    CHECK(!cal.is_open_at(*parse_iso_utc("2026-11-27T18:00:00Z"))); // half day, 13:00 ET close
}
