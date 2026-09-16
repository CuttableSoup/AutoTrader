#include "helpers.hpp"
#include "strategy/engine.hpp"
#include "strategy/signal.hpp"
#include "strategy/universe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace at;
using Catch::Approx;

namespace {

struct Fixture {
    StrategyParams params;
    StrategyEngine engine{StrategyParams{}, nyse()};
    Date start = make_date(2022, 1, 3);
    Date report = make_date(2023, 8, 1);   // Tuesday, AMC -> day0 = Aug 2, signal session = Aug 3
    Date day0 = make_date(2023, 8, 2);
    Date signal = make_date(2023, 8, 3);

    Fixture() {
        params.signal.ear_top_tercile_fallback = false;
        engine.set_params(params);
        // SPY: near-deterministic uptrend so it sits above its 200-day SMA on every session.
        BarSeries spy = test::synth_bars(start, 500, 40000, 0.001, 0.0005, 1);
        for (auto& b : spy) engine.on_bar("SPY", b);
        // Universe of 10 names; S0 gets the reaction and overwhelming 12-1 momentum.
        std::vector<std::string> syms;
        for (int i = 0; i < 10; ++i) {
            std::string s = "S" + std::to_string(i);
            syms.push_back(s);
            double drift = i == 0 ? 0.0025 : 0.0;   // S0 ~ +80% over the lookback vs ~0% +/- 15% for the rest
            BarSeries bars = test::synth_bars(start, 500, 5000 + i * 100, 0.010, drift, 100 + static_cast<unsigned>(i));
            if (i == 0) test::inject_reaction(bars, day0, 6.0, 3.0, 1.0);
            for (auto& b : bars) engine.on_bar(s, b);
        }
        engine.set_universe(test::synth_universe(syms, signal));
    }
};

} // namespace

TEST_CASE("day0 depends on BMO/AMC and calendar", "[signal]") {
    EarningsEvent e = test::synth_event("X", make_date(2023, 8, 1), EarningsEvent::Timing::BMO);
    CHECK(e.day0(nyse()) == make_date(2023, 8, 1));
    e.timing = EarningsEvent::Timing::AMC;
    CHECK(e.day0(nyse()) == make_date(2023, 8, 2));
    e.timing = EarningsEvent::Timing::UNKNOWN;
    CHECK(e.day0(nyse()) == make_date(2023, 8, 2));
    // Friday AMC -> Monday
    EarningsEvent f = test::synth_event("X", make_date(2023, 8, 4), EarningsEvent::Timing::AMC);
    CHECK(f.day0(nyse()) == make_date(2023, 8, 7));
    // BMO on a holiday -> next session
    EarningsEvent h = test::synth_event("X", make_date(2023, 9, 4), EarningsEvent::Timing::BMO);
    CHECK(h.day0(nyse()) == make_date(2023, 9, 5));
}

TEST_CASE("EAR computation is close(day-1)->close(day+1) minus benchmark", "[signal]") {
    Fixture f;
    EarningsEvent ev = test::synth_event("S0", f.report);
    EarComputation ear = compute_ear(ev, f.engine.market(), nyse(), f.params.signal);
    REQUIRE(ear.ok);
    CHECK(ear.day0 == f.day0);
    CHECK(ear.day_plus1 == f.signal);
    CHECK(ear.ear_pct > 4.0);           // 6% jump + 1% drift minus market noise
    CHECK(ear.vol_ratio > 2.0);
    const auto& mkt = f.engine.market();
    double raw = simple_return(*mkt.close_on("S0", ear.day_minus1), *mkt.close_on("S0", ear.day_plus1)) * 100.0;
    double bm = simple_return(*mkt.close_on("SPY", ear.day_minus1), *mkt.close_on("SPY", ear.day_plus1)) * 100.0;
    CHECK(ear.ear_pct == Approx(raw - bm));
}

TEST_CASE("signal passes for the reacting, high-momentum name and fails otherwise", "[signal]") {
    Fixture f;
    f.engine.on_earnings_event(test::synth_event("S0", f.report));
    f.engine.on_earnings_event(test::synth_event("S1", f.report));   // no reaction, weak momentum
    auto r = f.engine.evaluate_session(f.signal, "2023-08-03T20:00:00Z");
    REQUIRE(r.evaluations.size() == 2);
    for (const auto& e : r.evaluations) {
        std::string why;
        for (const auto& x : e.failed) why += x + " ";
        for (const auto& x : e.checked) why += x + " ";
        INFO("evaluation: " << why);
    }
    REQUIRE(r.candidates.size() == 1);
    const Candidate& c = r.candidates[0];
    CHECK(c.symbol == "S0");
    CHECK(c.spy_above_trend);
    CHECK(c.mom_pct >= 60.0);
    CHECK(c.entry_deadline_date == nyse().add_sessions(f.signal, 2));
    CHECK(c.atr20_cents > 0);
    CHECK(c.entry_px_ref_cents == *f.engine.market().close_on("S0", f.signal));
    CHECK(!c.thesis_facts.empty());
    for (const auto& fact : c.thesis_facts) {
        CHECK(fact.find("strong") == std::string::npos);
        CHECK(fact.find("great") == std::string::npos);
    }
    // Idempotent: the same event never yields a second candidate.
    auto again = f.engine.evaluate_session(f.signal, "2023-08-03T20:00:00Z");
    CHECK(again.candidates.empty());
    // Wrong session -> not evaluated as a signal
    auto other = f.engine.evaluate_session(nyse().next_trading_day(f.signal), "x");
    CHECK(other.candidates.empty());
}

TEST_CASE("trend filter blocks new longs", "[signal]") {
    Fixture f;
    // Crash SPY below its 200-day SMA at the signal session.
    BarSeries spy = *f.engine.market().bars("SPY");
    for (auto& b : spy) if (b.date >= add_days(f.signal, -30)) { b.close = b.close / 2; b.open = b.open / 2; b.high = b.high / 2; b.low = b.low / 2; f.engine.on_bar("SPY", b); }
    f.engine.on_earnings_event(test::synth_event("S0", f.report));
    auto r = f.engine.evaluate_session(f.signal, "x");
    CHECK(r.candidates.empty());
    REQUIRE(r.evaluations.size() == 1);
    bool trend_failed = false;
    for (const auto& x : r.evaluations[0].failed) if (x == "trend") trend_failed = true;
    CHECK(trend_failed);
    CHECK(r.spy_above_trend.has_value());
    CHECK(!*r.spy_above_trend);
}

TEST_CASE("universe rules exclude what the plan says", "[universe]") {
    MarketStore mkt;
    BarSeries bars = test::synth_bars(make_date(2023, 1, 3), 60, 10000, 0.01, 0, 3, 2000000);
    for (auto& b : bars) mkt.add_bar("OK", b);
    for (auto& b : bars) mkt.add_bar("ETF1", b);
    for (auto& b : bars) mkt.add_bar("SMALL", b);
    for (auto& b : bars) mkt.add_bar("NVDA", b);
    for (auto& b : bars) mkt.add_bar("THIN", b);
    Date as_of = bars.back().date;
    UniverseParams p;
    p.exclude_over_optioned = {"NVDA"};
    std::vector<SecurityInfo> secs;
    secs.push_back(test::synth_security("OK"));
    auto etf = test::synth_security("ETF1"); etf.category = "ETF"; secs.push_back(etf);
    auto small = test::synth_security("SMALL"); small.market_cap_cents = 100000000000LL; secs.push_back(small);
    secs.push_back(test::synth_security("NVDA"));
    auto thin = test::synth_security("THIN"); thin.median_spread_bps = 9.0; secs.push_back(thin);
    auto ipo = test::synth_security("IPO"); ipo.first_listed = add_days(as_of, -100); secs.push_back(ipo);
    auto nocov = test::synth_security("NOCOV"); nocov.analyst_coverage = 2; secs.push_back(nocov);
    auto adr = test::synth_security("ADR1"); adr.category = "ADR Common Stock"; secs.push_back(adr);
    UniverseSnapshot u = build_universe(secs, mkt, as_of, p);
    CHECK(u.symbols == std::vector<std::string>{"OK"});
    CHECK(u.exclusion_reasons["ETF1"] == "etf");
    CHECK(u.exclusion_reasons["SMALL"] == "market_cap");
    CHECK(u.exclusion_reasons["NVDA"] == "over_optioned");
    CHECK(u.exclusion_reasons["THIN"] == "spread");
    CHECK(u.exclusion_reasons["IPO"] == "ipo_age");
    CHECK(u.exclusion_reasons["NOCOV"] == "analyst_coverage");
    CHECK(u.exclusion_reasons["ADR1"] == "adr");
    CHECK(u.id.size() == 16);
}

TEST_CASE("securities_as_of is point-in-time", "[universe]") {
    std::vector<SecurityInfo> all;
    auto a1 = test::synth_security("A"); a1.as_of = make_date(2023, 1, 31); a1.market_cap_cents = 1; all.push_back(a1);
    auto a2 = test::synth_security("A"); a2.as_of = make_date(2023, 6, 30); a2.market_cap_cents = 2; all.push_back(a2);
    auto v1 = securities_as_of(all, make_date(2023, 3, 1));
    REQUIRE(v1.size() == 1);
    CHECK(v1[0].market_cap_cents == 1);
    auto v2 = securities_as_of(all, make_date(2023, 12, 1));
    CHECK(v2[0].market_cap_cents == 2);
    CHECK(securities_as_of(all, make_date(2022, 1, 1)).empty());
}
