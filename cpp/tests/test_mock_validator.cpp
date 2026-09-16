#include "backtester/mock_validator.hpp"
#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace at;

namespace {
Candidate cand(const std::string& sym, double ear) {
    Candidate c;
    c.symbol = sym; c.ear_pct = ear; c.atr20_cents = 100; c.entry_px_ref_cents = 10000; c.session_date = make_date(2023, 8, 3);
    return c;
}
} // namespace

TEST_CASE("mock validator rules", "[mock]") {
    MockValidatorConfig cfg;
    MockValidator mv(cfg, {"NVDA"}, nyse());
    MarketStore mkt;
    BarSeries bars = test::synth_bars(make_date(2023, 1, 3), 200, 10000, 0.0, 0.0, 5);
    for (auto& b : bars) mkt.add_bar("X", b);
    for (auto& b : bars) mkt.add_bar("NVDA", b);
    Date entry = make_date(2023, 8, 4);

    SECTION("approves a clean candidate") {
        EarningsEvent ev = test::synth_event("X", make_date(2023, 8, 1));
        auto v = mv.validate(cand("X", 5.0), &ev, mkt, entry, 40);
        CHECK(v.verdict == "APPROVE");
        CHECK(!v.used_lookahead);
        auto p = v.to_payload("cid", "X", "MOCK");
        CHECK(p["model"] == "mock");
        CHECK(p["mode"] == "MOCK");
    }
    SECTION("over-optioned") {
        EarningsEvent ev = test::synth_event("NVDA", make_date(2023, 8, 1));
        auto v = mv.validate(cand("NVDA", 5.0), &ev, mkt, entry, 40);
        CHECK(v.verdict == "REJECT");
        CHECK(v.flags[0] == "OVER_OPTIONED");
    }
    SECTION("revenue miss with large EAR") {
        EarningsEvent ev = test::synth_event("X", make_date(2023, 8, 1));
        ev.revenue_actual_cents = 100; ev.revenue_consensus_cents = 200;
        CHECK(mv.validate(cand("X", 9.0), &ev, mkt, entry, 40).verdict == "REJECT");
        CHECK(mv.validate(cand("X", 4.0), &ev, mkt, entry, 40).verdict == "APPROVE");
    }
    SECTION("second 8-K inside the drift window (lookahead proxy)") {
        EarningsEvent ev = test::synth_event("X", make_date(2023, 8, 1));
        ev.material_8k_dates = {make_date(2023, 8, 20)};
        auto v = mv.validate(cand("X", 5.0), &ev, mkt, entry, 40);
        CHECK(v.verdict == "REJECT");
        CHECK(v.used_lookahead);
        ev.material_8k_dates = {make_date(2024, 1, 20)};
        CHECK(mv.validate(cand("X", 5.0), &ev, mkt, entry, 40).verdict == "APPROVE");
    }
    SECTION("counter gap after entry (lookahead proxy)") {
        BarSeries g = bars;
        auto i = index_of_date(g, nyse().add_sessions(entry, 1));
        REQUIRE(i);
        g[*i].open = g[*i - 1].close - 500;   // 5x ATR gap down
        for (auto& b : g) mkt.add_bar("X", b);
        EarningsEvent ev = test::synth_event("X", make_date(2023, 8, 1));
        auto v = mv.validate(cand("X", 5.0), &ev, mkt, entry, 40);
        CHECK(v.verdict == "REJECT");
        CHECK(v.flags[0] == "COUNTER_GAP");
        CHECK(v.used_lookahead);
    }
    SECTION("counter gap disabled") {
        MockValidatorConfig c2 = cfg;
        c2.reject_counter_gap_atr_mult = 0;
        MockValidator mv2(c2, {}, nyse());
        BarSeries g = bars;
        auto i = index_of_date(g, nyse().add_sessions(entry, 1));
        g[*i].open = g[*i - 1].close - 500;
        for (auto& b : g) mkt.add_bar("X", b);
        EarningsEvent ev = test::synth_event("X", make_date(2023, 8, 1));
        CHECK(mv2.validate(cand("X", 5.0), &ev, mkt, entry, 40).verdict == "APPROVE");
    }
}
