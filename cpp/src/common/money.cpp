#include "money.hpp"

#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace at {

namespace {
Cents round_half_away(double v) {
    return static_cast<Cents>(v >= 0 ? std::floor(v + 0.5) : std::ceil(v - 0.5));
}
} // namespace

Cents cents_from_dollars(double dollars) { return round_half_away(dollars * 100.0); }

Cents parse_cents(std::string_view s) {
    if (s.empty()) throw std::invalid_argument("parse_cents: empty");
    std::size_t i = 0;
    bool neg = false;
    if (s[i] == '-' || s[i] == '+') { neg = s[i] == '-'; ++i; }
    Cents whole = 0;
    bool any_digit = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        any_digit = true;
        if (whole > (INT64_MAX - 9) / 10) throw std::invalid_argument("parse_cents: overflow");
        whole = whole * 10 + (s[i] - '0');
    }
    Cents frac = 0; // in cents
    if (i < s.size() && s[i] == '.') {
        ++i;
        int digits = 0;
        int third = -1;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            any_digit = true;
            if (digits < 2) { frac = frac * 10 + (s[i] - '0'); ++digits; }
            else if (third < 0) { third = s[i] - '0'; }
        }
        if (digits == 1) frac *= 10;
        if (third >= 5) frac += 1; // round half away from zero (sign applied below)
    }
    if (!any_digit || i != s.size()) throw std::invalid_argument("parse_cents: bad number '" + std::string(s) + "'");
    Cents total = whole * 100 + frac;
    return neg ? -total : total;
}

std::string cents_to_decimal(Cents c) {
    bool neg = c < 0;
    Cents a = neg ? -c : c;
    std::string out = std::to_string(a / 100);
    Cents f = a % 100;
    out += '.';
    out += static_cast<char>('0' + f / 10);
    out += static_cast<char>('0' + f % 10);
    return neg ? "-" + out : out;
}

std::string format_money(Cents c) {
    bool neg = c < 0;
    Cents a = neg ? -c : c;
    std::string whole = std::to_string(a / 100);
    std::string grouped;
    int n = 0;
    for (auto it = whole.rbegin(); it != whole.rend(); ++it) {
        if (n && n % 3 == 0) grouped.insert(grouped.begin(), ',');
        grouped.insert(grouped.begin(), *it);
        ++n;
    }
    Cents f = a % 100;
    std::string out = (neg ? "-$" : "$") + grouped + '.';
    out += static_cast<char>('0' + f / 10);
    out += static_cast<char>('0' + f % 10);
    return out;
}

double cents_to_dollars(Cents c) { return static_cast<double>(c) / 100.0; }

Cents apply_pct(Cents base, double pct) { return round_half_away(static_cast<double>(base) * pct / 100.0); }

Cents mul(Cents base, double factor) { return round_half_away(static_cast<double>(base) * factor); }

double pct_of(Cents a, Cents b) { return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b) * 100.0; }

double bps_between(Cents a, Cents b) {
    return a == 0 ? 0.0 : (static_cast<double>(b) - static_cast<double>(a)) / static_cast<double>(a) * 10000.0;
}

Cents round_to_tick(Cents px, Cents tick) {
    if (tick <= 1) return px;
    Cents r = px % tick;
    return r * 2 >= tick ? px - r + tick : px - r;
}

} // namespace at
