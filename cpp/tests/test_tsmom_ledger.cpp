#include "strategy/ledger.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace at;
using Catch::Approx;

namespace {
EntryMeta equity_meta() { EntryMeta m; m.asset_class = "EQUITY"; return m; }
} // namespace

TEST_CASE("apply_rebalance_fill opens a long position from flat", "[tsmom][ledger]") {
    Ledger l(100000000);   // $1,000,000
    l.apply_rebalance_fill("SPY", 500, 10000, 1000, make_date(2024, 1, 2), "t0", equity_meta());
    CHECK(l.cash() == 100000000 - (500 * 10000 + 1000));
    auto p = l.position("SPY");
    REQUIRE(p);
    CHECK(p->qty == 500);
    CHECK(p->avg_px_cents == 10000);
    CHECK(p->asset_class == "EQUITY");
}

TEST_CASE("apply_rebalance_fill opening a short increases cash by the proceeds minus costs", "[tsmom][ledger]") {
    Ledger l(100000000);
    l.apply_rebalance_fill("TLT", -400, 5000, 0, make_date(2024, 1, 2), "t0", equity_meta());
    CHECK(l.cash() == 100000000 + 400 * 5000);   // short-sale proceeds, no costs
    auto p = l.position("TLT");
    REQUIRE(p);
    CHECK(p->qty == -400);
    CHECK(p->avg_px_cents == 5000);
}

TEST_CASE("apply_rebalance_fill increasing a long uses a weighted-average cost basis", "[tsmom][ledger]") {
    Ledger l(100000000);
    l.apply_rebalance_fill("SPY", 500, 10000, 0, make_date(2024, 1, 2), "t0", equity_meta());
    l.apply_rebalance_fill("SPY", 800, 12000, 500, make_date(2024, 2, 1), "t1", equity_meta());
    auto p = l.position("SPY");
    REQUIRE(p);
    CHECK(p->qty == 800);
    CHECK(p->avg_px_cents == (500 * 10000 + 300 * 12000) / 800);
    CHECK(l.cash() == 100000000 - 500 * 10000 - (300 * 12000 + 500));
}

TEST_CASE("apply_rebalance_fill reducing a long realizes P&L on the reduced portion and keeps avg_px", "[tsmom][ledger]") {
    Ledger l(100000000);
    l.apply_rebalance_fill("SPY", 800, 10000, 0, make_date(2024, 1, 2), "t0", equity_meta());
    l.apply_rebalance_fill("SPY", 300, 12000, 100, make_date(2024, 2, 1), "t1", equity_meta());
    auto p = l.position("SPY");
    REQUIRE(p);
    CHECK(p->qty == 300);
    CHECK(p->avg_px_cents == 10000);   // unchanged on a pure reduction
    REQUIRE(l.trades().size() == 1);
    const auto& t = l.trades().back();
    CHECK(t.qty == 500);
    CHECK(t.exit_reason == "rebalance_reduce");
    CHECK(t.gross_pnl_cents == 500 * (12000 - 10000));
    CHECK(t.costs_cents == 100);
    CHECK(t.net_pnl_cents == 500 * (12000 - 10000) - 100);
    CHECK(l.cash() == 100000000 - 800 * 10000 + (500 * 12000 - 100));
}

TEST_CASE("apply_rebalance_fill flips a long into a short in one fill: closes the old side, opens the new", "[tsmom][ledger]") {
    Ledger l(100000000);
    l.apply_rebalance_fill("SPY", 500, 10000, 0, make_date(2024, 1, 2), "t0", equity_meta());
    l.apply_rebalance_fill("SPY", -300, 9000, 50, make_date(2024, 2, 1), "t1", equity_meta());

    auto p = l.position("SPY");
    REQUIRE(p);
    CHECK(p->qty == -300);
    CHECK(p->avg_px_cents == 9000);   // the new leg's own fill price, no carryover from the old side

    REQUIRE(l.trades().size() == 1);
    const auto& t = l.trades().back();
    CHECK(t.qty == 500);
    CHECK(t.exit_reason == "rebalance_flip");
    CHECK(t.gross_pnl_cents == 500 * (9000 - 10000));   // the long lost money as price fell
    CHECK(t.net_pnl_cents < 0);
    CHECK(l.consecutive_losers() == 1);

    Cents cash_after_open = 100000000 - 500 * 10000;
    Cents delta = -300 - 500;   // -800
    CHECK(l.cash() == cash_after_open - (delta * 9000 + 50));
}

TEST_CASE("apply_rebalance_fill fully closes a short and realizes the covering P&L", "[tsmom][ledger]") {
    Ledger l(100000000);
    l.apply_rebalance_fill("TLT", -400, 5000, 0, make_date(2024, 1, 2), "t0", equity_meta());
    l.apply_rebalance_fill("TLT", 0, 4500, 100, make_date(2024, 2, 1), "t1", equity_meta());

    CHECK_FALSE(l.position("TLT").has_value());
    REQUIRE(l.trades().size() == 1);
    const auto& t = l.trades().back();
    CHECK(t.qty == -400);
    CHECK(t.exit_reason == "rebalance_close");
    CHECK(t.gross_pnl_cents == -400 * (4500 - 5000));   // covering cheaper than the short entry -> profit
    CHECK(t.gross_pnl_cents == 200000);
    CHECK(t.net_pnl_cents == 200000 - 100);
}

TEST_CASE("apply_rebalance_fill is a no-op when the target equals the current position", "[tsmom][ledger]") {
    Ledger l(100000000);
    l.apply_rebalance_fill("SPY", 500, 10000, 0, make_date(2024, 1, 2), "t0", equity_meta());
    Cents cash_before = l.cash();
    l.apply_rebalance_fill("SPY", 500, 11000, 999, make_date(2024, 2, 1), "t1", equity_meta());
    CHECK(l.cash() == cash_before);
    CHECK(l.trades().empty());
    CHECK(l.position("SPY")->avg_px_cents == 10000);
}

TEST_CASE("asset_class_exposure_pct sums gross exposure per bucket, long and short alike", "[tsmom][ledger]") {
    Ledger l(100000000);
    EntryMeta eq = equity_meta();
    EntryMeta rates;
    rates.asset_class = "RATES_CREDIT";
    l.apply_rebalance_fill("SPY", 500, 10000, 0, make_date(2024, 1, 2), "t0", eq);     // +$5,000,000
    l.apply_rebalance_fill("TLT", -300, 10000, 0, make_date(2024, 1, 2), "t0", rates); // -$3,000,000

    auto pct = l.asset_class_exposure_pct();
    REQUIRE(pct.count("EQUITY"));
    REQUIRE(pct.count("RATES_CREDIT"));
    CHECK(pct["EQUITY"] == Approx(5.0));
    CHECK(pct["RATES_CREDIT"] == Approx(3.0));   // gross, not net -- the short still counts
}
