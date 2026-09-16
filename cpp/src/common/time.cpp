#include "time.hpp"

#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace at {

using namespace std::chrono;

std::string iso_date(Date d) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", int(d.year()), unsigned(d.month()), unsigned(d.day()));
    return buf;
}

std::optional<Date> parse_date(std::string_view s) {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    int y = 0; unsigned m = 0, d = 0;
    auto r1 = std::from_chars(s.data(), s.data() + 4, y);
    auto r2 = std::from_chars(s.data() + 5, s.data() + 7, m);
    auto r3 = std::from_chars(s.data() + 8, s.data() + 10, d);
    if (r1.ec != std::errc{} || r2.ec != std::errc{} || r3.ec != std::errc{}) return std::nullopt;
    Date ymd{year{y}, month{m}, day{d}};
    if (!ymd.ok()) return std::nullopt;
    return ymd;
}

Date parse_date_or_throw(std::string_view s) {
    auto d = parse_date(s);
    if (!d) throw std::invalid_argument("bad date: " + std::string(s));
    return *d;
}

std::string iso_utc(SysTime t) {
    auto dp = floor<days>(t);
    Date ymd{dp};
    auto tod = t - dp;
    auto h = duration_cast<hours>(tod); tod -= h;
    auto m = duration_cast<minutes>(tod); tod -= m;
    auto s = duration_cast<seconds>(tod); tod -= s;
    auto ms = duration_cast<milliseconds>(tod);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", int(ymd.year()), unsigned(ymd.month()),
                  unsigned(ymd.day()), int(h.count()), int(m.count()), int(s.count()), int(ms.count()));
    return buf;
}

std::optional<SysTime> parse_iso_utc(std::string_view s) {
    // YYYY-MM-DDTHH:MM:SS[.fff...][Z|+00:00|-HH:MM]
    if (s.size() < 19 || (s[10] != 'T' && s[10] != ' ')) return std::nullopt;
    auto d = parse_date(s.substr(0, 10));
    if (!d) return std::nullopt;
    int hh = 0, mm = 0, ss = 0;
    if (std::from_chars(s.data() + 11, s.data() + 13, hh).ec != std::errc{}) return std::nullopt;
    if (std::from_chars(s.data() + 14, s.data() + 16, mm).ec != std::errc{}) return std::nullopt;
    if (std::from_chars(s.data() + 17, s.data() + 19, ss).ec != std::errc{}) return std::nullopt;
    std::size_t i = 19;
    long ms = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        int digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (digits < 3) { ms = ms * 10 + (s[i] - '0'); ++digits; }
            ++i;
        }
        while (digits < 3) { ms *= 10; ++digits; }
    }
    long offset_s = 0;
    if (i < s.size()) {
        if (s[i] == 'Z') { ++i; }
        else if (s[i] == '+' || s[i] == '-') {
            int sign = s[i] == '-' ? -1 : 1;
            int oh = 0, om = 0;
            if (i + 6 > s.size()) return std::nullopt;
            if (std::from_chars(s.data() + i + 1, s.data() + i + 3, oh).ec != std::errc{}) return std::nullopt;
            if (std::from_chars(s.data() + i + 4, s.data() + i + 6, om).ec != std::errc{}) return std::nullopt;
            offset_s = sign * (oh * 3600 + om * 60);
            i += 6;
        } else return std::nullopt;
    }
    if (i != s.size()) return std::nullopt;
    sys_days sd{*d};
    SysTime t = sd + hours{hh} + minutes{mm} + seconds{ss} + milliseconds{ms} - seconds{offset_s};
    return t;
}

SysTime now_utc() { return time_point_cast<milliseconds>(Clock::now()); }
std::string now_utc_iso() { return iso_utc(now_utc()); }

namespace {
// Second Sunday in March, first Sunday in November, 02:00 local.
sys_days nth_sunday(int y, unsigned m, int n) {
    Date first{year{y}, month{m}, day{1}};
    sys_days sd{first};
    weekday wd{sd};
    int delta = (7 - int(wd.c_encoding())) % 7; // days to first Sunday
    return sd + days{delta + 7 * (n - 1)};
}
} // namespace

bool ny_is_dst(SysTime t) {
    auto dp = floor<days>(t);
    int y = int(Date{dp}.year());
    // DST starts 2:00 EST = 07:00 UTC on second Sunday of March; ends 2:00 EDT = 06:00 UTC first Sunday of November.
    SysTime start = nth_sunday(y, 3, 2) + hours{7};
    SysTime end = nth_sunday(y, 11, 1) + hours{6};
    return t >= start && t < end;
}

int ny_utc_offset_seconds(SysTime t) { return ny_is_dst(t) ? -4 * 3600 : -5 * 3600; }

NyLocal to_ny(SysTime t) {
    int off = ny_utc_offset_seconds(t);
    SysTime local = t + seconds{off};
    auto dp = floor<days>(local);
    auto tod = local - dp;
    NyLocal out;
    out.date = Date{dp};
    out.hour = int(duration_cast<hours>(tod).count());
    out.minute = int(duration_cast<minutes>(tod).count() % 60);
    out.second = int(duration_cast<seconds>(tod).count() % 60);
    out.dst = off == -4 * 3600;
    out.utc_offset_seconds = off;
    return out;
}

SysTime from_ny(Date d, int hour, int minute, int second) {
    // Guess with EST, then correct using the DST rule evaluated at the guess.
    SysTime guess = sys_days{d} + hours{hour} + minutes{minute} + seconds{second} + hours{5};
    int off = ny_utc_offset_seconds(guess);
    SysTime t = sys_days{d} + hours{hour} + minutes{minute} + seconds{second} - seconds{off};
    // Re-evaluate once in case the guess straddled a transition.
    int off2 = ny_utc_offset_seconds(t);
    if (off2 != off) t = sys_days{d} + hours{hour} + minutes{minute} + seconds{second} - seconds{off2};
    return t;
}

Date add_days(Date d, int n) { return Date{sys_days{d} + days{n}}; }
int days_between(Date a, Date b) { return int((sys_days{b} - sys_days{a}).count()); }
weekday weekday_of(Date d) { return weekday{sys_days{d}}; }
bool is_weekend(Date d) { auto wd = weekday_of(d); return wd == Saturday || wd == Sunday; }
int year_of(Date d) { return int(d.year()); }
int month_of(Date d) { return int(unsigned(d.month())); }
int day_of(Date d) { return int(unsigned(d.day())); }
Date make_date(int y, unsigned m, unsigned dd) { return Date{year{y}, month{m}, day{dd}}; }
std::int32_t date_key(Date d) { return year_of(d) * 10000 + month_of(d) * 100 + day_of(d); }

} // namespace at
