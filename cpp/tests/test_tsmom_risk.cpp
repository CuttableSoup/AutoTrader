#include "common/ids.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/ledger.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace at;

namespace {

struct TsmomRiskFixture {
    RiskLimits rl;
    TsmomParams tp;
    Date session = make_date(2026, 9, 30);
    SysTime now = from_ny(session, 16, 25);

    TsmomRiskFixture() { rl.shadow_mode = true; }

    RiskManager make() { return RiskManager(StrategyParams{}, rl, nyse(), tp); }

    // Builds a portfolio.state payload via the Ledger (so the fixture exercises the same
    // apply_rebalance_fill path milestone 3 tests) with at most one pre-existing position.
    nlohmann::json portfolio(Cents equity, const std::string& symbol = "", std::int64_t qty = 0, Cents px = 0, const std::string& asset_class = "") {
        Ledger l(equity);
        if (qty != 0) {
            EntryMeta m;
            m.asset_class = asset_class;
            l.apply_rebalance_fill(symbol, qty, px, 0, session, "t0", m);
        }
        return l.portfolio_state_payload("2026-09-30T21:25:00.000Z", session, true, nlohmann::json::array(), 0, 0, 0, std::nullopt);
    }

    Candidate candidate(const std::string& sym, const std::string& asset_class, double target_weight_pct) {
        Candidate c;
        c.symbol = sym;
        c.signal_type = "TSMOM_ETF_V1";
        c.strategy_version = tp.strategy_version;
        c.event_id = "u|" + sym + "|2026-09";
        c.session_date = session;
        c.entry_deadline_date = nyse().add_sessions(session, 5);
        c.universe_snapshot_id = "u";
        c.data_as_of_utc = "2026-09-30T21:25:00.000Z";
        c.asset_class = asset_class;
        c.target_weight_pct = target_weight_pct;
        c.mom_sign = target_weight_pct >= 0 ? 1 : -1;
        c.vol_annual_pct = 15.0;
        c.thesis_facts = {"fact"};
        return c;
    }

    // bid/ask symmetric around $100.00, spread exactly at the 10bps default limit.
    Quote quote(Cents bid = 9995, Cents ask = 10005) {
        Quote q;
        q.bid_cents = bid;
        q.ask_cents = ask;
        q.bid_size = q.ask_size = 100;
        q.ts = now;
        return q;
    }

    void reconcile(RiskManager& r) {
        r.on_reconcile({{"status", "CLEAN"}, {"trigger", "STARTUP"}, {"checked_at_utc", "x"}, {"broker_positions", nlohmann::json::array()},
                        {"internal_positions", nlohmann::json::array()}, {"diffs", nlohmann::json::array()}, {"open_orders_count", 0}, {"orphan_orders", nlohmann::json::array()}});
    }
};

} // namespace

TEST_CASE("evaluate_rebalance opens a fresh long, sized off the mid quote", "[tsmom][risk]") {
    TsmomRiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(100000000));
    Envelope env = make_envelope("tsmom", f.candidate("SPY", "EQUITY", 5.0).to_json());
    r.on_candidate(env);
    r.on_quote("SPY", f.quote());
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    const auto& d = ds[0];
    INFO(d.reject_reason);
    REQUIRE(d.approved);
    CHECK(d.order["intent"] == "REBALANCE_TO_WEIGHT");
    CHECK(d.order["side"] == "buy");
    CHECK(d.order["qty"].get<int>() == 500);
    CHECK(d.order["target_qty"].get<std::int64_t>() == 500);
    CHECK(d.order["limit_px_cents"].get<Cents>() == 10020);   // mid 10000 + 20bps, rounded to tick
    CHECK(d.order["asset_class"] == "EQUITY");
    CHECK(d.order["validated_msg_id"].is_null());
    CHECK(d.order["client_order_id"] == client_order_id_for_entry(env.msg_id));
}

TEST_CASE("evaluate_rebalance flips a long into a short in one order", "[tsmom][risk]") {
    TsmomRiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(100000000, "SPY", 500, 10000, "EQUITY"));   // existing 5% long
    r.on_candidate(make_envelope("tsmom", f.candidate("SPY", "EQUITY", -5.0).to_json()));
    r.on_quote("SPY", f.quote());
    r.on_shortable("SPY", true);   // increasing a short requires broker-confirmed shortability
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    const auto& d = ds[0];
    INFO(d.reject_reason);
    REQUIRE(d.approved);
    CHECK(d.order["side"] == "sell");
    CHECK(d.order["qty"].get<int>() == 1000);          // 500 (close the long) + 500 (open the short)
    CHECK(d.order["target_qty"].get<std::int64_t>() == -500);
}

TEST_CASE("evaluate_rebalance rejects opening a short without a confirmed shortable flag", "[tsmom][risk]") {
    TsmomRiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(100000000));   // flat
    r.on_candidate(make_envelope("tsmom", f.candidate("FXY", "CURRENCIES", -5.0).to_json()));
    r.on_quote("FXY", f.quote());
    // No on_shortable("FXY", ...) call at all -- unconfirmed, not "confirmed false".
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    CHECK_FALSE(ds[0].approved);
    CHECK(ds[0].reject_reason == "shortable");

    // The broker confirming it isn't shortable is rejected the same way.
    RiskManager r2 = f.make();
    f.reconcile(r2);
    r2.on_portfolio_state(f.portfolio(100000000));
    r2.on_candidate(make_envelope("tsmom", f.candidate("FXY", "CURRENCIES", -5.0).to_json()));
    r2.on_quote("FXY", f.quote());
    r2.on_shortable("FXY", false);
    auto ds2 = r2.process_pending_entries(f.session, f.now);
    REQUIRE(ds2.size() == 1);
    CHECK_FALSE(ds2[0].approved);
    CHECK(ds2[0].reject_reason == "shortable");
}

TEST_CASE("evaluate_rebalance does not need a shortable flag to go long or to reduce an existing short", "[tsmom][risk]") {
    TsmomRiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    // Going long from flat: never touches shortability.
    r.on_portfolio_state(f.portfolio(100000000));
    r.on_candidate(make_envelope("tsmom", f.candidate("SPY", "EQUITY", 5.0).to_json()));
    r.on_quote("SPY", f.quote());
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    INFO(ds[0].reject_reason);
    CHECK(ds[0].approved);

    // Reducing an existing short toward flat: also never touches shortability.
    RiskManager r2 = f.make();
    f.reconcile(r2);
    r2.on_portfolio_state(f.portfolio(100000000, "FXY", -500, 10000, "CURRENCIES"));   // existing 5% short
    r2.on_candidate(make_envelope("tsmom", f.candidate("FXY", "CURRENCIES", -2.0).to_json()));   // smaller short
    r2.on_quote("FXY", f.quote());
    auto ds2 = r2.process_pending_entries(f.session, f.now);
    REQUIRE(ds2.size() == 1);
    INFO(ds2[0].reject_reason);
    CHECK(ds2[0].approved);
}

TEST_CASE("evaluate_rebalance is a no-op when the sized target equals the current position", "[tsmom][risk]") {
    TsmomRiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(100000000, "SPY", 500, 10000, "EQUITY"));   // already at the 5% target
    r.on_candidate(make_envelope("tsmom", f.candidate("SPY", "EQUITY", 5.0).to_json()));
    r.on_quote("SPY", f.quote());
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    CHECK_FALSE(ds[0].approved);
    CHECK(ds[0].reject_reason == "no_op");
}

TEST_CASE("evaluate_rebalance rejects when the asset-class bucket would exceed its cap", "[tsmom][risk]") {
    TsmomRiskFixture f;
    f.rl.asset_class_cap_pct = 40.0;
    RiskManager r = f.make();
    f.reconcile(r);
    // Existing EQUITY exposure at 39% via a different symbol; QQQ wants a fresh 10% ->
    // 49% > 40% cap.
    r.on_portfolio_state(f.portfolio(100000000, "SPY", 3900, 10000, "EQUITY"));
    r.on_candidate(make_envelope("tsmom", f.candidate("QQQ", "EQUITY", 10.0).to_json()));
    r.on_quote("QQQ", f.quote());
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    CHECK_FALSE(ds[0].approved);
    CHECK(ds[0].reject_reason == "asset_class_cap_pct");
}

TEST_CASE("evaluate_rebalance rejects when total gross exposure would exceed 100%", "[tsmom][risk]") {
    TsmomRiskFixture f;
    f.rl.asset_class_cap_pct = 100.0;   // isolate the gross check from the asset-class check
    RiskManager r = f.make();
    f.reconcile(r);
    // 95% gross already used by a RATES_CREDIT position; a fresh 10% EQUITY position pushes
    // total gross to 105% > 100%.
    r.on_portfolio_state(f.portfolio(100000000, "TLT", 9500, 10000, "RATES_CREDIT"));
    r.on_candidate(make_envelope("tsmom", f.candidate("SPY", "EQUITY", 10.0).to_json()));
    r.on_quote("SPY", f.quote());
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    CHECK_FALSE(ds[0].approved);
    CHECK(ds[0].reject_reason == "gross_exposure_cap_pct");
}

TEST_CASE("TSMOM candidates never wait on a validator verdict, even outside shadow mode", "[tsmom][risk]") {
    TsmomRiskFixture f;
    f.rl.shadow_mode = false;   // live mode: earnings candidates would stay pending without a verdict
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(100000000));
    r.on_candidate(make_envelope("tsmom", f.candidate("SPY", "EQUITY", 5.0).to_json()));
    r.on_quote("SPY", f.quote());
    auto ds = r.process_pending_entries(f.session, f.now);
    REQUIRE(ds.size() == 1);
    INFO(ds[0].reject_reason);
    CHECK(ds[0].approved);   // resolved immediately despite pc.verdict being unset
    CHECK(r.pending_count() == 0);
}
