#include "backtester/tsmom_backtester.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

using namespace at;
using Catch::Approx;

// Parity test: the C++ port's return-attribution math (the open/close intraday/overnight
// split across formation transitions -- what test_tsmom_signal.cpp's single-formation
// weight check does NOT exercise) checked against
// python/autotrader/research/tsmom.py::portfolio_returns on a real ~2.5-year window from
// the development panel (python/autotrader/research/export_tsmom_backtest_fixture.py).
// Gross only: no cost, no idle-capital risk-free credit, on either side (documented in the
// export script). This is what docs/prereg/TSMOM-v1-gates.md's proposed G1 parity check
// depends on.
TEST_CASE("run_tsmom_gross_backtest matches the golden Python gross-return fixture", "[tsmom][parity][backtest]") {
    std::filesystem::path fixture_path = std::filesystem::path(AT_PROJECT_ROOT) / "cpp" / "tests" / "fixtures" / "tsmom_backtest_golden.json";
    std::ifstream in(fixture_path);
    REQUIRE(in.good());
    nlohmann::json fx;
    in >> fx;

    const auto& cal = nyse();
    TsmomParams params;
    params.signal.momentum_lookback_sessions = fx.at("momentum_lookback").get<int>();
    params.signal.vol_lookback_sessions = fx.at("vol_lookback").get<int>();

    auto symbols = fx.at("symbols").get<std::vector<std::string>>();
    auto asset_classes = fx.at("asset_classes").get<std::vector<std::string>>();
    auto dates_str = fx.at("dates").get<std::vector<std::string>>();
    const auto& open_rows = fx.at("open_tr_cents");
    const auto& close_rows = fx.at("close_tr_cents");
    REQUIRE(dates_str.size() == open_rows.size());
    REQUIRE(dates_str.size() == close_rows.size());

    MarketStore mkt;
    UniverseSnapshot u;
    u.id = "backtest-fixture";
    for (std::size_t j = 0; j < symbols.size(); ++j) {
        u.symbols.push_back(symbols[j]);
        SecurityInfo info;
        info.symbol = symbols[j];
        info.asset_class = asset_classes[j];
        u.info[symbols[j]] = info;
    }
    for (std::size_t i = 0; i < dates_str.size(); ++i) {
        Date d = parse_date_or_throw(dates_str[i]);
        for (std::size_t j = 0; j < symbols.size(); ++j) {
            if (open_rows[i][j].is_null() || close_rows[i][j].is_null()) continue;
            Bar b;
            b.date = d;
            b.open = open_rows[i][j].get<Cents>();
            b.close = close_rows[i][j].get<Cents>();
            b.high = std::max(b.open, b.close);
            b.low = std::min(b.open, b.close);
            b.volume = 1000000;
            mkt.add_bar(symbols[j], b);
        }
    }

    Date compare_start = parse_date_or_throw(fx.at("compare_start_date").get<std::string>());
    Date compare_end = parse_date_or_throw(dates_str.back());
    auto result = run_tsmom_gross_backtest(mkt, u, params, compare_start, compare_end, cal);
    REQUIRE(!result.dates.empty());

    std::map<std::string, double> by_date;
    for (std::size_t i = 0; i < result.dates.size(); ++i) by_date[iso_date(result.dates[i])] = result.book_return[i];

    const auto& expected = fx.at("expected");
    REQUIRE(!expected.empty());
    double max_abs_diff = 0.0;
    int compared = 0;
    for (const auto& e : expected) {
        std::string d = e.at("date").get<std::string>();
        double exp = e.at("book_return").get<double>();
        auto it = by_date.find(d);
        REQUIRE(it != by_date.end());
        max_abs_diff = std::max(max_abs_diff, std::fabs(it->second - exp));
        ++compared;
    }
    INFO("compared " << compared << " of " << expected.size() << " expected days, max abs diff " << max_abs_diff);
    CHECK(compared == static_cast<int>(expected.size()));
    // Absolute tolerance, not relative: daily book returns are frequently near zero (idle
    // days, small weights), where a relative comparison is meaningless. Most days agree to
    // a few basis points (the same cents-quantization noise floor established in
    // test_tsmom_signal.cpp's weight parity test). A handful of days spike higher: a
    // formation's per-instrument weight can itself carry a few percent of relative noise
    // (test_tsmom_signal.cpp documents up to ~5% for an unusually low-vol instrument like
    // SHY), and that weight stays active for the whole month until the next formation -- on
    // a day when the affected instrument happens to make an unusually large move (observed:
    // a volatile week in instruments including USO/GLD/TLT around 2023-04-25..28), a few
    // percent of weight error times a large return surfaces as up to several tens of basis
    // points on that one day, even though the underlying weight error is the same small,
    // already-documented, already-accepted effect. 60bps covers the worst case observed
    // over this fixture's ~2.5-year, 600-day window (max ~53bps) with headroom, while still
    // being far below what a genuine formula bug would produce: a wrong open/close
    // attribution misattributes an entire day's return to the wrong side of a formation
    // transition, which shows up as a difference on the order of the instrument's whole
    // daily move (typically 1%+), not tens of basis points.
    for (const auto& e : expected) {
        std::string d = e.at("date").get<std::string>();
        double exp = e.at("book_return").get<double>();
        INFO("date " << d);
        CHECK(by_date.at(d) == Approx(exp).margin(6e-3));
    }
}
