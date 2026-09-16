// Fixed-point money. Every monetary value in the system is an int64 number of
// cents. Doubles appear only at the boundary (vendor JSON with decimal strings,
// percentages) and are converted here with explicit rounding.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace at {

using Cents = std::int64_t;
constexpr Cents kCentsPerDollar = 100;

// dollars -> cents, round half away from zero.
Cents cents_from_dollars(double dollars);

// Parse a decimal string ("123.45", "-0.5", "100") exactly, no floating point.
// More than two decimals are rounded half away from zero. Throws std::invalid_argument.
Cents parse_cents(std::string_view decimal);

// "123.45" style, always two decimals, sign preserved.
std::string cents_to_decimal(Cents c);

// "$1,234.56" for logs and reports.
std::string format_money(Cents c);

double cents_to_dollars(Cents c);

// base * pct / 100, rounded half away from zero. pct may be negative.
Cents apply_pct(Cents base, double pct);

// base * factor, rounded half away from zero.
Cents mul(Cents base, double factor);

// (a / b) * 100 as a double; 0 if b == 0.
double pct_of(Cents a, Cents b);

// Basis points between two prices: (b - a) / a * 10000.
double bps_between(Cents a, Cents b);

// Round to the nearest tick. NMS stocks >= $1.00 trade in $0.01 ticks.
Cents round_to_tick(Cents px, Cents tick = 1);

} // namespace at
