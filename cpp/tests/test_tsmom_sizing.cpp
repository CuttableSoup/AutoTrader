#include "strategy/tsmom_sizing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace at;
using Catch::Approx;

namespace {
TsmomInstrumentSignal sig(std::string symbol, std::string asset_class, double raw_weight) {
    TsmomInstrumentSignal s;
    s.symbol = std::move(symbol);
    s.asset_class = std::move(asset_class);
    s.ok = true;
    s.raw_weight = raw_weight;
    return s;
}
} // namespace

TEST_CASE("compute_tsmom_target_weights leaves weights unscaled under the gross cap", "[tsmom]") {
    std::vector<TsmomInstrumentSignal> signals = {sig("SPY", "EQUITY", 0.2), sig("TLT", "RATES_CREDIT", -0.3)};
    auto w = compute_tsmom_target_weights(signals);
    REQUIRE(w.size() == 2);
    CHECK(w[0].symbol == "SPY");
    CHECK(w[0].target_weight == Approx(0.2));
    CHECK(w[1].target_weight == Approx(-0.3));
}

TEST_CASE("compute_tsmom_target_weights down-scales when gross exceeds the cap, never levers up", "[tsmom]") {
    std::vector<TsmomInstrumentSignal> signals = {sig("SPY", "EQUITY", 3.0), sig("TLT", "RATES_CREDIT", -0.5)};
    auto w = compute_tsmom_target_weights(signals);   // gross = 3.5, cap 100%
    double gross_out = std::fabs(w[0].target_weight) + std::fabs(w[1].target_weight);
    CHECK(gross_out == Approx(1.0));
    CHECK(w[0].target_weight == Approx(3.0 / 3.5));
    CHECK(w[1].target_weight == Approx(-0.5 / 3.5));
}

TEST_CASE("compute_tsmom_target_weights: all-zero momentum yields all-zero weights", "[tsmom]") {
    std::vector<TsmomInstrumentSignal> signals = {sig("SPY", "EQUITY", 0.0), sig("TLT", "RATES_CREDIT", 0.0)};
    auto w = compute_tsmom_target_weights(signals);
    for (const auto& x : w) CHECK(x.target_weight == 0.0);
}

TEST_CASE("compute_tsmom_target_weights: empty universe yields empty result", "[tsmom]") {
    CHECK(compute_tsmom_target_weights({}).empty());
}

TEST_CASE("compute_tsmom_target_weights respects a non-default gross cap", "[tsmom]") {
    std::vector<TsmomInstrumentSignal> signals = {sig("SPY", "EQUITY", 1.0), sig("TLT", "RATES_CREDIT", -1.0)};
    auto w = compute_tsmom_target_weights(signals, 50.0);   // 50% gross cap
    double gross_out = std::fabs(w[0].target_weight) + std::fabs(w[1].target_weight);
    CHECK(gross_out == Approx(0.5));
}

TEST_CASE("size_tsmom_target sizes long and short targets, and guards invalid input", "[tsmom]") {
    TsmomPositionSizingInput in;
    in.equity_cents = 100000000;   // $1,000,000
    in.ref_px_cents = 10000;       // $100
    in.target_weight = 0.05;       // 5% long

    auto r = size_tsmom_target(in);
    CHECK(r.target_qty == 500);
    CHECK(r.target_notional_cents == 5000000);
    CHECK(r.target_weight_pct == Approx(5.0));

    in.target_weight = -0.05;      // 5% short
    r = size_tsmom_target(in);
    CHECK(r.target_qty == -500);
    CHECK(r.target_notional_cents == -5000000);
    CHECK(r.target_weight_pct == Approx(-5.0));

    in.equity_cents = 0;
    CHECK(size_tsmom_target(in).target_qty == 0);
}
