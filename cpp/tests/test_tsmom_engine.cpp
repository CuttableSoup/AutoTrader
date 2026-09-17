#include "helpers.hpp"
#include "strategy/engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <stdexcept>

using namespace at;

namespace {

UniverseSnapshot make_tsmom_test_universe(const std::vector<std::pair<std::string, std::string>>& syms, Date as_of) {
    UniverseSnapshot u;
    u.id = "test-tsmom-universe";
    u.as_of = as_of;
    for (const auto& [sym, asset_class] : syms) {
        u.symbols.push_back(sym);
        SecurityInfo info;
        info.symbol = sym;
        info.category = "ETF";
        info.asset_class = asset_class;
        info.as_of = as_of;
        u.info[sym] = info;
    }
    return u;
}

// A date, well into the fixture's history, that is a formation session.
Date find_formation(const BarSeries& s, const TradingCalendar& cal, std::size_t min_index) {
    for (std::size_t i = min_index; i < s.size(); ++i)
        if (is_formation_session(s[i].date, cal)) return s[i].date;
    throw std::runtime_error("no formation session found with enough warm-up in the fixture");
}

struct Fixture {
    const TradingCalendar& cal = nyse();
    Date start = make_date(2023, 1, 3);
    StrategyEngine engine{StrategyParams{}, cal};
    Date formation{};

    Fixture() {
        // Drift is ~4-5 sigma above the 252-session noise floor (vol 0.010/day -> ~0.159
        // cumulative std over 252 sessions) so the momentum sign is robust to the seed.
        BarSeries spy = test::synth_bars(start, 400, 40000, 0.010, 0.003, 1);    // strong uptrend -> long
        BarSeries tlt = test::synth_bars(start, 400, 12000, 0.008, -0.003, 2);   // strong downtrend -> short
        for (auto& b : spy) engine.on_bar("SPY", b);
        for (auto& b : tlt) engine.on_bar("TLT", b);
        engine.set_universe(make_tsmom_test_universe({{"SPY", "EQUITY"}, {"TLT", "RATES_CREDIT"}}, start));
        formation = find_formation(spy, cal, 313);   // 252 momentum + 60 vol + 1 warm-up
    }
};

} // namespace

TEST_CASE("evaluate_rebalance_session no-ops outside a formation session", "[tsmom][engine]") {
    Fixture f;
    Date not_formation = f.cal.prev_trading_day(f.formation);
    REQUIRE_FALSE(is_formation_session(not_formation, f.cal));
    auto r = f.engine.evaluate_rebalance_session(not_formation, "x");
    CHECK(r.candidates.empty());
    CHECK(r.signals.empty());
}

TEST_CASE("evaluate_rebalance_session emits one candidate per symbol, is idempotent within a month, and re-fires next month", "[tsmom][engine]") {
    Fixture f;
    auto r = f.engine.evaluate_rebalance_session(f.formation, "2024-01-01T21:00:00Z");
    REQUIRE(r.signals.size() == 2);
    REQUIRE(r.candidates.size() == 2);

    const Candidate* spy = nullptr;
    const Candidate* tlt = nullptr;
    for (const auto& c : r.candidates) {
        CHECK(c.signal_type == "TSMOM_ETF_V1");
        CHECK(c.side == "BUY");
        CHECK(c.session_date == f.formation);
        CHECK(c.universe_snapshot_id == "test-tsmom-universe");
        CHECK(!c.event_id.empty());
        CHECK(!c.thesis_facts.empty());
        if (c.symbol == "SPY") spy = &c;
        if (c.symbol == "TLT") tlt = &c;
    }
    REQUIRE(spy);
    REQUIRE(tlt);
    CHECK(spy->asset_class == "EQUITY");
    CHECK(tlt->asset_class == "RATES_CREDIT");
    REQUIRE(spy->mom_sign.has_value());
    REQUIRE(tlt->mom_sign.has_value());
    CHECK(*spy->mom_sign == 1);    // strong uptrend
    CHECK(*tlt->mom_sign == -1);   // strong downtrend
    REQUIRE(spy->target_weight_pct.has_value());
    REQUIRE(tlt->target_weight_pct.has_value());
    CHECK(*spy->target_weight_pct > 0.0);   // long
    CHECK(*tlt->target_weight_pct < 0.0);   // short
    double gross = std::fabs(*spy->target_weight_pct) + std::fabs(*tlt->target_weight_pct);
    CHECK(gross <= 100.0 + 1e-6);

    // Idempotent: same formation session, same month -> no new candidates.
    auto again = f.engine.evaluate_rebalance_session(f.formation, "2024-01-01T21:00:00Z");
    CHECK(again.candidates.empty());

    // The next formation session (necessarily next month, since f.formation was the last
    // session of its own month) re-fires.
    Date latest = *f.engine.market().latest_date("SPY");
    Date next_formation = f.cal.next_trading_day(f.formation);
    while (next_formation <= latest && !is_formation_session(next_formation, f.cal)) next_formation = f.cal.next_trading_day(next_formation);
    REQUIRE(next_formation <= latest);
    CHECK(iso_date(next_formation).substr(0, 7) != iso_date(f.formation).substr(0, 7));
    auto later = f.engine.evaluate_rebalance_session(next_formation, "x");
    CHECK(later.candidates.size() == 2);
}
