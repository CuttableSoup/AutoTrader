#include "calendar.hpp"

#include <stdexcept>

namespace at {

using namespace std::chrono;

Date easter_sunday(int y) {
    // Anonymous Gregorian algorithm.
    int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4;
    int f = (b + 8) / 25, g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    int month = (h + l - 7 * m + 114) / 31;
    int day = ((h + l - 7 * m + 114) % 31) + 1;
    return make_date(y, unsigned(month), unsigned(day));
}

namespace {

// n-th given weekday of month (n>=1), or last if n == -1.
Date nth_weekday(int y, unsigned m, weekday wd, int n) {
    if (n > 0) {
        Date first = make_date(y, m, 1);
        int delta = (int(wd.c_encoding()) - int(weekday_of(first).c_encoding()) + 7) % 7;
        return add_days(first, delta + 7 * (n - 1));
    }
    Date last = Date{year{y} / month{m} / last_spec{}};
    int delta = (int(weekday_of(last).c_encoding()) - int(wd.c_encoding()) + 7) % 7;
    return add_days(last, -delta);
}

// NYSE observed rule: Saturday -> Friday, Sunday -> Monday.
// Exception: New Year's Day on Saturday is NOT observed on Friday Dec 31 (NYSE Rule 7.2).
Date observed(Date d, bool new_years = false) {
    auto wd = weekday_of(d);
    if (wd == Saturday) return new_years ? d : add_days(d, -1);
    if (wd == Sunday) return add_days(d, 1);
    return d;
}

struct Holiday { Date date; const char* name; };

std::vector<Holiday> holidays_for_year(int y) {
    std::vector<Holiday> h;
    h.push_back({observed(make_date(y, 1, 1), true), "New Year's Day"});
    h.push_back({nth_weekday(y, 1, Monday, 3), "Martin Luther King Jr. Day"});
    h.push_back({nth_weekday(y, 2, Monday, 3), "Presidents' Day"});
    h.push_back({add_days(easter_sunday(y), -2), "Good Friday"});
    h.push_back({nth_weekday(y, 5, Monday, -1), "Memorial Day"});
    if (y >= 2022) h.push_back({observed(make_date(y, 6, 19)), "Juneteenth"});
    h.push_back({observed(make_date(y, 7, 4)), "Independence Day"});
    h.push_back({nth_weekday(y, 9, Monday, 1), "Labor Day"});
    h.push_back({nth_weekday(y, 11, Thursday, 4), "Thanksgiving"});
    h.push_back({observed(make_date(y, 12, 25)), "Christmas"});
    return h;
}

} // namespace

TradingCalendar::TradingCalendar() {
    // Ad-hoc closures since 2000.
    add_closure(make_date(2001, 9, 11), "September 11 attacks");
    add_closure(make_date(2001, 9, 12), "September 11 attacks");
    add_closure(make_date(2001, 9, 13), "September 11 attacks");
    add_closure(make_date(2001, 9, 14), "September 11 attacks");
    add_closure(make_date(2004, 6, 11), "Reagan funeral");
    add_closure(make_date(2007, 1, 2), "Ford funeral");
    add_closure(make_date(2012, 10, 29), "Hurricane Sandy");
    add_closure(make_date(2012, 10, 30), "Hurricane Sandy");
    add_closure(make_date(2018, 12, 5), "G.H.W. Bush funeral");
    add_closure(make_date(2025, 1, 9), "Carter funeral");
}

void TradingCalendar::add_closure(Date d, std::string name) {
    closures_.insert(date_key(d));
    closure_names_.emplace_back(date_key(d), std::move(name));
}

bool TradingCalendar::is_holiday(Date d) const {
    if (closures_.count(date_key(d))) return true;
    for (const auto& h : holidays_for_year(year_of(d)))
        if (h.date == d) return true;
    // New Year's observed on Monday Jan 1 of next year when Dec 31 is ... handled by next year's list.
    // Christmas observed Dec 24 (Friday) when Dec 25 is Saturday is in this year's list already.
    return false;
}

std::string TradingCalendar::holiday_name(Date d) const {
    for (const auto& [k, n] : closure_names_)
        if (k == date_key(d)) return n;
    for (const auto& h : holidays_for_year(year_of(d)))
        if (h.date == d) return h.name;
    return "";
}

bool TradingCalendar::is_trading_day(Date d) const { return !is_weekend(d) && !is_holiday(d); }

bool TradingCalendar::is_half_day(Date d) const {
    if (!is_trading_day(d)) return false;
    int y = year_of(d), m = month_of(d), dd = day_of(d);
    auto wd = weekday_of(d);
    // Day after Thanksgiving.
    if (d == add_days(nth_weekday(y, 11, Thursday, 4), 1)) return true;
    // July 3 when it is Mon-Thu (July 4 then falls Tue-Fri and is a full holiday).
    if (m == 7 && dd == 3 && wd != Friday) return true;
    // Christmas Eve when Mon-Thu.
    if (m == 12 && dd == 24 && wd != Friday) return true;
    return false;
}

SessionTimes TradingCalendar::session_times(Date d) const {
    SessionTimes s;
    if (is_half_day(d)) { s.close_minutes = 13 * 60; s.half_day = true; }
    return s;
}

Date TradingCalendar::next_trading_day(Date d) const {
    Date x = add_days(d, 1);
    int guard = 0;
    while (!is_trading_day(x)) { x = add_days(x, 1); if (++guard > 30) throw std::runtime_error("calendar: no trading day found"); }
    return x;
}

Date TradingCalendar::prev_trading_day(Date d) const {
    Date x = add_days(d, -1);
    int guard = 0;
    while (!is_trading_day(x)) { x = add_days(x, -1); if (++guard > 30) throw std::runtime_error("calendar: no trading day found"); }
    return x;
}

Date TradingCalendar::add_sessions(Date d, int n) const {
    Date x = d;
    if (!is_trading_day(x)) x = n >= 0 ? next_trading_day(x) : prev_trading_day(x);
    // Now x is a trading day; the "n == 0" case is x itself.
    while (n > 0) { x = next_trading_day(x); --n; }
    while (n < 0) { x = prev_trading_day(x); ++n; }
    return x;
}

int TradingCalendar::sessions_between(Date a, Date b) const {
    if (b < a) return -sessions_between(b, a);
    int n = 0;
    Date x = a;
    while (x < b) { x = next_trading_day(x); if (x <= b) ++n; }
    return n;
}

std::vector<Date> TradingCalendar::sessions(Date from, Date to) const {
    std::vector<Date> out;
    for (Date x = from; x <= to; x = add_days(x, 1))
        if (is_trading_day(x)) out.push_back(x);
    return out;
}

Date TradingCalendar::session_for(SysTime t) const {
    NyLocal ny = to_ny(t);
    Date d = ny.date;
    if (!is_trading_day(d)) return prev_trading_day(d);
    auto st = session_times(d);
    if (ny.minutes_since_midnight() < st.open_minutes) return prev_trading_day(d);
    return d;
}

bool TradingCalendar::is_open_at(SysTime t) const {
    NyLocal ny = to_ny(t);
    if (!is_trading_day(ny.date)) return false;
    auto st = session_times(ny.date);
    int m = ny.minutes_since_midnight();
    return m >= st.open_minutes && m < st.close_minutes;
}

const TradingCalendar& nyse() {
    static const TradingCalendar cal;
    return cal;
}

} // namespace at
