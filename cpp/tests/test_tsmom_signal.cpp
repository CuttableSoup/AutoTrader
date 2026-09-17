#include "helpers.hpp"
#include "strategy/tsmom_sizing.hpp"
#include "strategy/tsmom_signal.hpp"
#include "strategy/tsmom_universe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace at;
using Catch::Approx;

TEST_CASE("is_formation_session true only on the last session of a month", "[tsmom]") {
    const auto& cal = nyse();
    auto sessions = cal.sessions(make_date(2026, 9, 1), make_date(2026, 9, 30));
    REQUIRE(sessions.size() > 1);
    CHECK(is_formation_session(sessions.back(), cal));
    CHECK_FALSE(is_formation_session(sessions[sessions.size() - 2], cal));
}

namespace {
MarketStore build_store(const std::string& symbol, Date start, int n, unsigned seed) {
    MarketStore mkt;
    BarSeries bars = test::synth_bars(start, n, 10000, 0.012, 0.0003, seed);
    for (auto& b : bars) mkt.add_bar(symbol, b);
    return mkt;
}
} // namespace

TEST_CASE("evaluate_tsmom_signal matches independently-computed momentum and vol", "[tsmom]") {
    const auto& cal = nyse();
    Date start = make_date(2023, 1, 3);
    MarketStore mkt = build_store("SPY", start, 320, 7);
    UniverseSnapshot u = build_tsmom_universe(start);
    TsmomParams params;

    Date formation = *mkt.latest_date("SPY");
    TsmomSignalContext ctx{mkt, u, cal, formation};
    auto sig = evaluate_tsmom_signal("SPY", ctx, params);
    REQUIRE(sig.ok);
    CHECK(sig.symbol == "SPY");
    CHECK(sig.asset_class == "EQUITY");

    const BarSeries& s = *mkt.bars("SPY");
    std::size_t end = mkt.end_index("SPY", formation);
    double expected_mom_ret = simple_return(s[end - 1 - 252].close, s[end - 1].close);
    double expected_sign = (expected_mom_ret > 0) - (expected_mom_ret < 0);
    CHECK(sig.mom_sign == expected_sign);

    std::vector<double> rets;
    rets.reserve(60);
    for (std::size_t i = end - 60; i < end; ++i) rets.push_back(simple_return(s[i - 1].close, s[i].close));
    double expected_vol = stdev(rets) * std::sqrt(252.0);
    CHECK(sig.vol_annual == Approx(expected_vol).epsilon(1e-9));
    CHECK(sig.raw_weight == Approx(expected_sign / expected_vol).epsilon(1e-9));
}

TEST_CASE("evaluate_tsmom_signal is not ok with insufficient history", "[tsmom]") {
    const auto& cal = nyse();
    Date start = make_date(2023, 1, 3);
    MarketStore mkt = build_store("SPY", start, 100, 7);   // fewer than momentum_lookback_sessions+1
    UniverseSnapshot u = build_tsmom_universe(start);
    TsmomParams params;
    TsmomSignalContext ctx{mkt, u, cal, *mkt.latest_date("SPY")};
    auto sig = evaluate_tsmom_signal("SPY", ctx, params);
    CHECK_FALSE(sig.ok);
}

TEST_CASE("evaluate_tsmom_signal is not ok without an unknown symbol or off-session bar", "[tsmom]") {
    const auto& cal = nyse();
    Date start = make_date(2023, 1, 3);
    MarketStore mkt = build_store("SPY", start, 320, 7);
    UniverseSnapshot u = build_tsmom_universe(start);
    TsmomParams params;

    TsmomSignalContext missing_symbol{mkt, u, cal, *mkt.latest_date("SPY")};
    CHECK_FALSE(evaluate_tsmom_signal("QQQ", missing_symbol, params).ok);

    TsmomSignalContext off_session{mkt, u, cal, add_days(*mkt.latest_date("SPY"), 30)};
    CHECK_FALSE(evaluate_tsmom_signal("SPY", off_session, params).ok);
}

// Parity test: the C++ port of the primary spec's per-instrument signal, checked against
// python/autotrader/research/tsmom.py::compute_weights on real formation dates from the
// development panel (python/autotrader/research/export_tsmom_fixture.py). This is what
// proves the port didn't silently diverge from the strategy that actually passed the gate
// (docs/TSMOM-RESULT.md) -- the other tests in this file check internal consistency, not
// agreement with Python. Real calendar dates aren't preserved in the fixture (the math is
// purely index-based); the price series is replayed onto a synthetic date axis instead.
TEST_CASE("evaluate_tsmom_signal and compute_tsmom_target_weights match the golden Python fixture", "[tsmom][parity]") {
    std::filesystem::path fixture_path = std::filesystem::path(AT_PROJECT_ROOT) / "cpp" / "tests" / "fixtures" / "tsmom_weights_golden.json";
    std::ifstream in(fixture_path);
    REQUIRE(in.good());
    nlohmann::json fx;
    in >> fx;

    const auto& cal = nyse();
    TsmomParams params;
    params.signal.momentum_lookback_sessions = fx.at("momentum_lookback").get<int>();
    params.signal.vol_lookback_sessions = fx.at("vol_lookback").get<int>();
    Date anchor = make_date(2024, 1, 2);   // arbitrary; only relative session spacing matters

    for (const auto& formation : fx.at("formations")) {
        MarketStore mkt;
        UniverseSnapshot u;
        u.id = "fixture";
        int n = formation.at("n_prices_per_symbol").get<int>();
        for (const auto& inst : formation.at("instruments")) {
            std::string sym = inst.at("symbol").get<std::string>();
            u.symbols.push_back(sym);
            SecurityInfo info;
            info.symbol = sym;
            info.asset_class = inst.at("asset_class").get<std::string>();
            u.info[sym] = info;
            auto prices = inst.at("prices_cents").get<std::vector<Cents>>();
            REQUIRE(static_cast<int>(prices.size()) == n);
            Date d = cal.add_sessions(anchor, -(n - 1));
            for (int i = 0; i < n; ++i) {
                Bar b;
                b.date = d;
                b.open = b.high = b.low = b.close = prices[static_cast<std::size_t>(i)];
                b.volume = 1000000;
                mkt.add_bar(sym, b);
                d = cal.next_trading_day(d);
            }
        }

        TsmomSignalContext ctx{mkt, u, cal, anchor};
        std::vector<TsmomInstrumentSignal> signals;
        for (const auto& sym : u.symbols) signals.push_back(evaluate_tsmom_signal(sym, ctx, params));
        auto weights = compute_tsmom_target_weights(signals);

        const auto& expected_instruments = formation.at("instruments");
        REQUIRE(signals.size() == expected_instruments.size());
        for (std::size_t i = 0; i < signals.size(); ++i) {
            const auto& expected = expected_instruments[i];
            INFO("formation " << formation.at("formation_date").get<std::string>() << " symbol " << signals[i].symbol);
            REQUIRE(signals[i].ok);
            // mom_sign must match exactly: it is what actually decides capital allocation
            // direction. vol_annual/raw_weight/target_weight are checked to a looser
            // tolerance because the fixture's prices are quantized to integer cents (this
            // system's universal money representation, CLAUDE.md) while Python's closeadj
            // series carries continuous total-return-adjustment precision. Propagating a
            // +/-$0.005 rounding noise through 60 daily returns and a variance calculation
            // (noise adds to the true return variance roughly independently) predicts about
            // a 0.1-0.2% relative effect on vol_annual for a ~$100-130, ~7-10%-annual-vol
            // instrument -- exactly the size observed (worst case: FXB at ~0.13%). 1e-2
            // relative is comfortably above that noise floor while still catching a formula
            // error, which shows up as an order-of-magnitude difference or a wrong sign, not
            // a few percent. raw_weight = mom_sign/vol_annual amplifies vol's own noise
            // further for an unusually low-vol instrument (e.g. SHY, a short-duration bond
            // ETF with ~0.5% annualized vol): the same roughly-constant absolute noise floor
            // is a much larger fraction of a tiny true vol, so vol_annual/raw_weight get 3e-2
            // headroom rather than 1e-2. target_weight gets extra headroom on top of that
            // (5e-2): it divides by gross = sum(|raw_weight|) across all 18 instruments, so
            // each instrument's noise compounds into that one shared normalizer, shifting
            // every instrument's target_weight in the same formation together.
            CHECK(signals[i].mom_sign == static_cast<double>(expected.at("mom_sign").get<int>()));
            CHECK(signals[i].vol_annual == Approx(expected.at("vol_annual").get<double>()).epsilon(3e-2));
            CHECK(signals[i].raw_weight == Approx(expected.at("raw_weight").get<double>()).epsilon(3e-2));
            CHECK(weights[i].target_weight == Approx(expected.at("target_weight").get<double>()).epsilon(5e-2));
        }
    }
}
