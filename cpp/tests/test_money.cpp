#include "common/money.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace at;

TEST_CASE("parse_cents is exact and rounds half away from zero", "[money]") {
    CHECK(parse_cents("123.45") == 12345);
    CHECK(parse_cents("0.5") == 50);
    CHECK(parse_cents("100") == 10000);
    CHECK(parse_cents("-0.05") == -5);
    CHECK(parse_cents("1.005") == 101);
    CHECK(parse_cents("-1.005") == -101);
    CHECK(parse_cents("1.004") == 100);
    CHECK(parse_cents("189.9999") == 19000);
    CHECK_THROWS(parse_cents(""));
    CHECK_THROWS(parse_cents("abc"));
    CHECK_THROWS(parse_cents("1.2.3"));
}

TEST_CASE("cents_from_dollars avoids binary float artifacts", "[money]") {
    CHECK(cents_from_dollars(0.29) == 29);
    CHECK(cents_from_dollars(1.15) == 115);
    CHECK(cents_from_dollars(-2.675) == -268);
    CHECK(cents_from_dollars(123456789.12) == 12345678912LL);
}

TEST_CASE("formatting", "[money]") {
    CHECK(cents_to_decimal(12345) == "123.45");
    CHECK(cents_to_decimal(-5) == "-0.05");
    CHECK(cents_to_decimal(100) == "1.00");
    CHECK(format_money(123456789) == "$1,234,567.89");
    CHECK(format_money(-99) == "-$0.99");
}

TEST_CASE("percent and multiply helpers", "[money]") {
    CHECK(apply_pct(100000, 0.5) == 500);
    CHECK(apply_pct(100000, -2.0) == -2000);
    CHECK(mul(1000, 3.0) == 3000);
    CHECK(pct_of(50, 200) == 25.0);
    CHECK(pct_of(1, 0) == 0.0);
    CHECK(bps_between(10000, 10010) == 10.0);
    CHECK(round_to_tick(1234, 5) == 1235);
    CHECK(round_to_tick(1232, 5) == 1230);
    CHECK(round_to_tick(1234, 1) == 1234);
}
