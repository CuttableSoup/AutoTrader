// NYSE trading calendar: holidays, observed rules, half days, ad-hoc closures.
// Rules are coded explicitly so the backtester and the live services agree
// without an external calendar file. Covers 2000-2035.
#pragma once
#include "time.hpp"

#include <set>
#include <string>
#include <vector>

namespace at {

struct SessionTimes {
    int open_minutes = 9 * 60 + 30;   // 09:30 ET
    int close_minutes = 16 * 60;      // 16:00 ET, 13:00 on half days
    bool half_day = false;
};

class TradingCalendar {
public:
    TradingCalendar();

    bool is_holiday(Date d) const;      // weekday closure
    bool is_trading_day(Date d) const;  // not weekend, not holiday
    bool is_half_day(Date d) const;
    SessionTimes session_times(Date d) const;
    std::string holiday_name(Date d) const; // "" if none

    // n may be negative. add_sessions(d, 0) returns d if trading day else next trading day.
    Date add_sessions(Date d, int n) const;
    Date next_trading_day(Date d) const;   // strictly after d
    Date prev_trading_day(Date d) const;   // strictly before d
    // Number of sessions strictly after a and up to and including b (b - a in sessions). Negative if b < a.
    int sessions_between(Date a, Date b) const;
    // All trading days in [from, to].
    std::vector<Date> sessions(Date from, Date to) const;

    // Session that a UTC timestamp belongs to for daily-bar purposes: if the
    // NY local time is after the close, it is still that date; before the open
    // on a trading day, it is the previous session (pre-market data belongs to
    // the prior close). Weekend/holiday -> previous trading day.
    Date session_for(SysTime t) const;
    // Is the regular session open at t?
    bool is_open_at(SysTime t) const;

    // Add an unscheduled closure (e.g. national day of mourning).
    void add_closure(Date d, std::string name);

private:
    std::set<std::int32_t> closures_;  // date_key
    std::vector<std::pair<std::int32_t, std::string>> closure_names_;
};

// Process-wide default calendar.
const TradingCalendar& nyse();

// Easter Sunday (Gregorian), for Good Friday.
Date easter_sunday(int year);

} // namespace at
