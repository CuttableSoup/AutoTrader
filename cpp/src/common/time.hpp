// Time helpers. UTC on the wire; America/New_York for every market-hours
// decision. The NY offset is computed from the US DST rule in force since 2007
// so the core never depends on a tz database being present.
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace at {

using Clock = std::chrono::system_clock;
using SysTime = std::chrono::sys_time<std::chrono::milliseconds>;
using Date = std::chrono::year_month_day;

// ---- ISO formatting / parsing -------------------------------------------
std::string iso_date(Date d);                        // "2026-09-15"
std::optional<Date> parse_date(std::string_view s);  // "2026-09-15"
Date parse_date_or_throw(std::string_view s);

std::string iso_utc(SysTime t);                      // "2026-09-15T13:30:00.123Z"
std::optional<SysTime> parse_iso_utc(std::string_view s); // accepts Z or +00:00, optional fraction
SysTime now_utc();
std::string now_utc_iso();

// ---- New York local time -------------------------------------------------
struct NyLocal {
    Date date;
    int hour = 0;     // 0-23
    int minute = 0;   // 0-59
    int second = 0;
    bool dst = false;
    int utc_offset_seconds = 0;
    int minutes_since_midnight() const { return hour * 60 + minute; }
};

bool ny_is_dst(SysTime t);
int ny_utc_offset_seconds(SysTime t); // -14400 (EDT) or -18000 (EST)
NyLocal to_ny(SysTime t);
// NY wall clock -> UTC. hour/minute in NY local time on the given date.
SysTime from_ny(Date d, int hour, int minute, int second = 0);

// ---- Date arithmetic (calendar days, not sessions) -------------------------
Date add_days(Date d, int days);
int days_between(Date a, Date b);   // b - a
std::chrono::weekday weekday_of(Date d);
bool is_weekend(Date d);
int year_of(Date d);
int month_of(Date d);
int day_of(Date d);
Date make_date(int y, unsigned m, unsigned d);

// Sortable integer key YYYYMMDD.
std::int32_t date_key(Date d);

} // namespace at
