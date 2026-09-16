#include "common/indicators.hpp"
#include "helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace at;
using Catch::Approx;

namespace {
BarSeries flat(int n, Cents px) {
    BarSeries s;
    Date d = make_date(2024, 1, 2);
    for (int i = 0; i < n; ++i) {
        Bar b; b.date = d; b.open = b.high = b.low = b.close = px; b.volume = 1000;
        s.push_back(b);
        d = nyse().next_trading_day(d);
    }
    return s;
}
} // namespace

TEST_CASE("sma / atr / adv on simple series", "[indicators]") {
    BarSeries s = flat(30, 10000);
    CHECK(sma_close(s, 30, 20) == Approx(10000.0));
    CHECK(!sma_close(s, 10, 20));
    // ATR of a flat series is zero; make one bar range 100 cents.
    s[25].high = 10100; s[25].low = 10000;
    auto a = atr(s, 30, 20);
    REQUIRE(a);
    CHECK(*a == 5); // 100/20
    CHECK(adv_shares(s, 30, 20) == Approx(1000.0));
    auto advd = adv_dollars_cents(s, 30, 20);
    REQUIRE(advd);
    CHECK(*advd == 10000LL * 1000);
}

TEST_CASE("momentum and returns", "[indicators]") {
    BarSeries s = flat(300, 10000);
    // Make the price grow linearly: close = 10000 + i*10.
    for (std::size_t i = 0; i < s.size(); ++i) s[i].close = 10000 + static_cast<Cents>(i) * 10;
    auto m = momentum_skip(s, 300, 252, 21);
    REQUIRE(m);
    Cents recent = s[300 - 1 - 21].close, old = s[300 - 1 - 252].close;
    CHECK(*m == Approx(simple_return(old, recent)));
    CHECK(!momentum_skip(s, 100, 252, 21));
    CHECK(simple_return(100, 110) == Approx(0.10));
}

TEST_CASE("rsi extremes", "[indicators]") {
    BarSeries up = flat(40, 10000);
    for (std::size_t i = 0; i < up.size(); ++i) up[i].close = 10000 + static_cast<Cents>(i) * 5;
    auto r = rsi(up, 40, 5);
    REQUIRE(r);
    CHECK(*r == Approx(100.0));
    BarSeries down = up;
    for (std::size_t i = 0; i < down.size(); ++i) down[i].close = 20000 - static_cast<Cents>(i) * 5;
    auto r2 = rsi(down, 40, 5);
    REQUIRE(r2);
    CHECK(*r2 == Approx(0.0).margin(1e-9));
}

TEST_CASE("realized vol of a random walk is close to its generator", "[indicators]") {
    BarSeries s = test::synth_bars(make_date(2020, 1, 2), 1000, 10000, 0.02, 0.0, 7);
    auto v = realized_vol_annual(s, s.size(), 999);
    REQUIRE(v);
    CHECK(*v == Approx(0.02 * std::sqrt(252.0)).epsilon(0.15));
}

TEST_CASE("percentile rank, median, moments", "[indicators]") {
    std::vector<double> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(percentile_rank(v, 10) == 100.0);
    CHECK(percentile_rank(v, 4) == 40.0);
    CHECK(percentile_rank(v, 0) == 0.0);
    CHECK(median(v) == 5.5);
    CHECK(mean(v) == 5.5);
    CHECK(stdev(v) == Approx(3.0277).epsilon(1e-3));
    std::vector<double> sym{-1, 0, 1};
    CHECK(skewness(sym) == Approx(0.0).margin(1e-12));
    std::vector<double> flat_r(100, 0.001);
    CHECK(sharpe(flat_r, 252) == 0.0);
}

TEST_CASE("index lookup", "[indicators]") {
    BarSeries s = flat(5, 100);
    CHECK(index_of_date(s, s[2].date) == 2);
    CHECK(!index_of_date(s, make_date(2023, 1, 1)));
    CHECK(lower_bound_date(s, make_date(2030, 1, 1)) == 5);
}
