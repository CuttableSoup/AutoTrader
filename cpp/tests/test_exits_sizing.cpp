#include "strategy/exits.hpp"
#include "strategy/sizing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace at;
using Catch::Approx;

TEST_CASE("exit deadline is min(drift window, day before next earnings)", "[exits]") {
    const auto& cal = nyse();
    ExitParams p;
    Date entry = make_date(2026, 9, 15);
    Date time_limit = cal.add_sessions(entry, 40);
    CHECK(compute_exit_deadline(entry, std::nullopt, p, cal) == time_limit);
    CHECK(compute_exit_deadline(entry, make_date(2027, 1, 20), p, cal) == time_limit);
    Date next = make_date(2026, 10, 28); // Wednesday
    CHECK(compute_exit_deadline(entry, next, p, cal) == make_date(2026, 10, 27));
    // Next earnings on a Monday: exit Friday before.
    CHECK(compute_exit_deadline(entry, make_date(2026, 10, 26), p, cal) == make_date(2026, 10, 23));
    p.exit_sessions_before_earnings = 2;
    CHECK(compute_exit_deadline(entry, next, p, cal) == make_date(2026, 10, 26));
}

TEST_CASE("stops: initial and ratchet never move down", "[exits]") {
    ExitParams p;
    CHECK(initial_stop_px(10000, 100, p) == 9700);
    CHECK(initial_stop_px(100, 100, p) == 1);
    CHECK(ratchet_stop_px(std::nullopt, 10000, 100, p) == 9700);
    CHECK(ratchet_stop_px(9700, 10500, 100, p) == 10200);
    CHECK(ratchet_stop_px(10200, 10000, 100, p) == 10200);
}

TEST_CASE("end-of-session exit sweep", "[exits]") {
    const auto& cal = nyse();
    ExitParams p;
    PositionState pos;
    pos.symbol = "X";
    pos.qty = 100;
    pos.avg_px_cents = 10000;
    pos.last_px_cents = 10400;
    pos.entry_session = make_date(2026, 9, 15);
    pos.exit_deadline = cal.add_sessions(pos.entry_session, 40);
    pos.stop_px_cents = 9700;
    pos.hwm_px_cents = 10400;
    pos.atr20_cents = 100;

    SECTION("ratchet only when price makes a new high") {
        auto a = evaluate_exits(pos, make_date(2026, 9, 20), true, p, cal);
        REQUIRE(a.size() == 1);
        CHECK(a[0].intent == ExitIntent::StopReplace);
        CHECK(*a[0].new_stop_cents == 10100);
        pos.stop_px_cents = 10100;
        CHECK(evaluate_exits(pos, make_date(2026, 9, 21), true, p, cal).empty());
    }
    SECTION("time limit exits the whole position and dominates") {
        auto a = evaluate_exits(pos, pos.exit_deadline, false, p, cal);
        REQUIRE(a.size() == 1);
        CHECK(a[0].intent == ExitIntent::ExitTime);
        CHECK(a[0].qty == 100);
    }
    SECTION("earnings gate labels the exit") {
        pos.next_report_date = make_date(2026, 10, 1);
        pos.exit_deadline = make_date(2026, 9, 30);
        auto a = evaluate_exits(pos, make_date(2026, 9, 30), true, p, cal);
        REQUIRE(a.size() == 1);
        CHECK(a[0].intent == ExitIntent::ExitEarnings);
    }
    SECTION("trend break scales down once") {
        pos.stop_px_cents = 10100;
        auto a = evaluate_exits(pos, make_date(2026, 9, 21), false, p, cal);
        REQUIRE(a.size() == 1);
        CHECK(a[0].intent == ExitIntent::ExitTrendScale);
        CHECK(a[0].qty == 50);
        pos.scaled_down = true;
        CHECK(evaluate_exits(pos, make_date(2026, 9, 22), false, p, cal).empty());
    }
    SECTION("unknown trend state never scales down") {
        pos.stop_px_cents = 10100;
        CHECK(evaluate_exits(pos, make_date(2026, 9, 21), std::nullopt, p, cal).empty());
    }
}

TEST_CASE("sizing respects every cap and reports the binding one", "[sizing]") {
    SizingParams sp;
    ExitParams ep;
    SizingInput in;
    in.equity_cents = 100000000;   // $1,000,000
    in.entry_px_cents = 10000;     // $100
    in.atr20_cents = 200;          // $2 ATR -> stop $94, risk $6/share
    in.stock_vol_annual = 0.30;

    SECTION("single name cap binds for a normal name") {
        // inverse-vol target = (11%/sqrt(15))/30% = 9.5% of equity -> capped at 8% = $80k -> 800 shares.
        // per-position risk would allow 0.5% * $1M / $6 = 833, so the cap is the binding constraint.
        auto r = size_position(in, sp, ep);
        CHECK(r.qty == 800);
        CHECK(r.binding == "single_name_cap");
        CHECK(r.stop_px_cents == 9400);
        CHECK(r.weight_pct == Approx(8.0).epsilon(0.01));
    }
    SECTION("per-position risk binds for a high-ATR name") {
        in.atr20_cents = 300;   // stop $91, risk $9/share -> $5,000 / $9 = 555 shares = $55.5k < $80k cap
        auto r = size_position(in, sp, ep);
        CHECK(r.qty == 555);
        CHECK(r.binding == "per_position_risk");
        CHECK(r.stop_px_cents == 9100);
    }
    SECTION("gap budget binds when tighter than risk") {
        in.atr20_cents = 500;
        ExitParams tight = ep;
        tight.trailing_stop_atr_mult = 1.0;   // risk $5/share -> 1000 shares; gap 2xATR=$10 -> $5,000/$10 = 500
        auto r = size_position(in, sp, tight);
        CHECK(r.qty == 500);
        CHECK(r.binding == "gap_budget");
    }
    SECTION("gross exposure and sector caps") {
        in.gross_exposure_cents = 99000000;   // $990k of $1M used -> $10k left
        auto r = size_position(in, sp, ep);
        CHECK(r.qty == 100);
        CHECK(r.binding == "gross_exposure");
        in.gross_exposure_cents = 0;
        in.sector_exposure_cents = 29000000;  // 29% used, cap 30% -> $10k
        r = size_position(in, sp, ep);
        CHECK(r.qty == 100);
        CHECK(r.binding == "sector_cap");
    }
    SECTION("max positions yields zero") {
        in.open_positions = 15;
        auto r = size_position(in, sp, ep);
        CHECK(r.qty == 0);
        CHECK(r.binding == "max_positions");
    }
    SECTION("halved regime halves the target before caps") {
        auto full = size_position(in, sp, ep);
        in.size_halved = true;
        auto half = size_position(in, sp, ep);
        REQUIRE(full.caps[0].first == "target");
        CHECK(half.caps[0].second == Approx(static_cast<double>(full.caps[0].second) / 2).epsilon(1e-6));
        CHECK(half.qty < full.qty);
        CHECK(half.binding == "target");
    }
    SECTION("vol targeting scales down a hot book") {
        in.atr20_cents = 50;
        in.book_vol_annual = 0.22;   // twice the 11% target
        auto r = size_position(in, sp, ep);
        CHECK(r.qty < 800);
    }
    SECTION("invalid input") {
        in.equity_cents = 0;
        CHECK(size_position(in, sp, ep).qty == 0);
    }
}
