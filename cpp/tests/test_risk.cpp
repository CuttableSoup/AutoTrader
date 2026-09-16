#include "common/ids.hpp"
#include "helpers.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/ledger.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace at;

namespace {

struct RiskFixture {
    StrategyParams sp;
    RiskLimits rl;
    Date session = make_date(2026, 9, 16);
    SysTime open = from_ny(session, 9, 31);

    RiskFixture() { rl.shadow_mode = true; }

    RiskManager make() { return RiskManager(sp, rl, nyse()); }

    nlohmann::json portfolio(Cents equity = 100000000, bool reconciled = true, double drawdown = 0.0, double daily = 0.0, int losers = 0) {
        Ledger l(equity);
        nlohmann::json p = l.portfolio_state_payload("2026-09-15T20:01:00.000Z", make_date(2026, 9, 15), reconciled, nlohmann::json::array(), 0, 0, 0, true);
        p["drawdown_pct"] = drawdown;
        p["daily_pnl_pct"] = daily;
        p["counters"]["consecutive_losers"] = losers;
        p["hwm_equity_cents"] = equity;
        return p;
    }

    Candidate candidate(const std::string& sym = "ABC") {
        Candidate c;
        c.symbol = sym;
        c.strategy_version = sp.strategy_version;
        c.event_id = "ev-" + sym;
        c.session_date = make_date(2026, 9, 15);
        c.ear_pct = 5.0;
        c.vol_ratio = 3.0;
        c.mom_pct = 80.0;
        c.entry_px_ref_cents = 10000;
        c.atr20_cents = 200;
        c.adv20_shares = 5000000;
        c.sector = "Technology";
        c.spy_above_trend = true;
        c.entry_deadline_date = make_date(2026, 9, 17);
        c.next_report_date = make_date(2026, 12, 1);
        c.universe_snapshot_id = "u";
        c.thesis_facts = {"fact"};
        c.data_as_of_utc = "2026-09-15T20:00:00.000Z";
        return c;
    }

    Quote quote(Cents bid = 10000, Cents ask = 10005, SysTime ts = SysTime{}) {
        Quote q;
        q.bid_cents = bid;
        q.ask_cents = ask;
        q.bid_size = q.ask_size = 100;
        q.ts = ts == SysTime{} ? open : ts;
        return q;
    }

    void reconcile(RiskManager& r) {
        r.on_reconcile({{"status", "CLEAN"}, {"trigger", "STARTUP"}, {"checked_at_utc", "x"}, {"broker_positions", nlohmann::json::array()},
                        {"internal_positions", nlohmann::json::array()}, {"diffs", nlohmann::json::array()}, {"open_orders_count", 0}, {"orphan_orders", nlohmann::json::array()}});
    }
};

} // namespace

TEST_CASE("happy path: approved entry carries deterministic id, stop and checks", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio());
    Envelope env = make_envelope("strategy", f.candidate().to_json());
    r.on_candidate(env);
    r.on_quote("ABC", f.quote());
    auto ds = r.process_pending_entries(f.session, f.open + std::chrono::seconds(3));
    REQUIRE(ds.size() == 1);
    const auto& d = ds[0];
    INFO(d.reject_reason);
    REQUIRE(d.approved);
    CHECK(d.order["client_order_id"] == client_order_id_for_entry(env.msg_id));
    CHECK(d.order["intent"] == "ENTRY");
    CHECK(d.order["order_type"] == "oto");
    CHECK(d.order["tif"] == "gtc");
    Cents limit = d.order["limit_px_cents"].get<Cents>();
    CHECK(limit == 10025); // ask 100.05 + 20 bps
    CHECK(d.order["stop_px_cents"].get<Cents>() == limit - 600);
    CHECK(d.order["qty"].get<int>() >= 1);
    CHECK(d.order["risk_checks"].size() > 10);
    CHECK(r.counters().new_positions_today == 1);
    CHECK(r.counters().orders_today == 1);
    CHECK(r.pending_count() == 0);
    // Same candidate again is deduped.
    r.on_candidate(env);
    CHECK(r.pending_count() == 0);
}

TEST_CASE("nothing trades before a clean reconcile", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    r.on_portfolio_state(f.portfolio(100000000, false));
    r.on_candidate(make_envelope("strategy", f.candidate().to_json()));
    r.on_quote("ABC", f.quote());
    auto ds = r.process_pending_entries(f.session, f.open);
    REQUIRE(ds.size() == 1);
    CHECK(!ds[0].approved);
    CHECK(ds[0].reject_reason == "reconciled");
}

TEST_CASE("reconcile mismatch pauses new entries and emits control", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    r.on_reconcile({{"status", "MISMATCH"}, {"diffs", nlohmann::json::array({{{"symbol", "ABC"}, {"field", "qty"}, {"broker", 10}, {"internal", 0}}})}});
    CHECK(r.paused_new());
    auto ctl = r.take_control_messages();
    REQUIRE(ctl.size() == 1);
    CHECK(ctl[0].first == "control.pause_new");
    CHECK(ctl[0].second["trigger"] == "RECONCILE_MISMATCH");
}

TEST_CASE("stale or wide quotes are rejected", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio());
    r.on_candidate(make_envelope("strategy", f.candidate().to_json()));
    SECTION("stale") {
        r.on_quote("ABC", f.quote(10000, 10005, f.open - std::chrono::seconds(45)));
        auto ds = r.process_pending_entries(f.session, f.open);
        CHECK(!ds[0].approved);
        CHECK(ds[0].reject_reason == "quote_age_s");
        CHECK(r.pending_count() == 1); // transient: retried later today
    }
    SECTION("wide spread") {
        r.on_quote("ABC", f.quote(10000, 10020)); // 20 bps
        auto ds = r.process_pending_entries(f.session, f.open);
        CHECK(!ds[0].approved);
        CHECK(ds[0].reject_reason == "spread_bps");
    }
    SECTION("no quote") {
        auto ds = r.process_pending_entries(f.session, f.open);
        CHECK(ds[0].reject_reason == "quote_present");
    }
}

TEST_CASE("daily caps: 3 new positions, 30 orders", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio());
    for (int i = 0; i < 5; ++i) {
        std::string sym = "S" + std::to_string(i);
        r.on_candidate(make_envelope("strategy", f.candidate(sym).to_json()));
        r.on_quote(sym, f.quote());
    }
    auto ds = r.process_pending_entries(f.session, f.open);
    int approved = 0;
    for (const auto& d : ds) approved += d.approved;
    CHECK(approved == 3);
    CHECK(ds[3].reject_reason == "max_new_positions_per_day");
}

TEST_CASE("live mode requires APPROVE; ERROR streak pauses", "[risk]") {
    RiskFixture f;
    f.rl.shadow_mode = false;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio());
    Envelope c1 = make_envelope("strategy", f.candidate("A").to_json());
    Envelope c2 = make_envelope("strategy", f.candidate("B").to_json());
    Envelope c3 = make_envelope("strategy", f.candidate("C").to_json());
    r.on_candidate(c1); r.on_candidate(c2); r.on_candidate(c3);
    auto verdict = [&](const Envelope& c, const char* v) {
        return make_envelope("validator", {{"candidate_msg_id", c.msg_id}, {"symbol", c.payload["symbol"]}, {"verdict", v}, {"confidence", 0.5}, {"reasons", nlohmann::json::array()},
                                           {"flags", nlohmann::json::array()}, {"model", "mock"}, {"latency_ms", 1}, {"cost_usd", 0.0}, {"citations", nlohmann::json::array()}, {"mode", "LIVE"}});
    };
    r.on_validated(verdict(c1, "APPROVE"));
    r.on_validated(verdict(c2, "REJECT"));
    for (const auto& s : {"A", "B", "C"}) r.on_quote(s, f.quote());
    auto ds = r.process_pending_entries(f.session, f.open);
    REQUIRE(ds.size() == 2);   // C has no verdict yet and stays pending
    CHECK(ds[0].approved);
    CHECK(!ds[1].approved);
    CHECK(ds[1].reject_reason == "validator_verdict");
    CHECK(r.pending_count() == 1);
    // Three ERRORs in a row -> pause + control
    Envelope c4 = make_envelope("strategy", f.candidate("D").to_json());
    Envelope c5 = make_envelope("strategy", f.candidate("E").to_json());
    r.on_candidate(c4); r.on_candidate(c5);
    r.on_validated(verdict(c3, "ERROR"));
    r.on_validated(verdict(c4, "ERROR"));
    CHECK(!r.paused_new());
    r.on_validated(verdict(c5, "ERROR"));
    CHECK(r.paused_new());
    auto ctl = r.take_control_messages();
    REQUIRE(!ctl.empty());
    CHECK(ctl.back().second["trigger"] == "VALIDATOR_ERROR_STREAK");
}

TEST_CASE("drawdown regimes: halve at -8%, flatten+halt at -12%", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(92000000, true, -8.5));
    CHECK(r.size_halved());
    CHECK(!r.halted());
    r.take_control_messages();
    r.on_portfolio_state(f.portfolio(87000000, true, -13.0));
    CHECK(r.halted());
    auto ctl = r.take_control_messages();
    REQUIRE(ctl.size() == 2);
    CHECK(ctl[0].first == "control.flatten");
    CHECK(ctl[1].first == "control.halt");
    // A plain resume does not clear a halt; an explicit operator clear_halt does.
    r.on_control("control.resume", {{"command", "resume"}, {"reason", "op"}, {"source", "operator"}, {"issued_at_utc", "x"}});
    CHECK(r.halted());
    CHECK(r.paused_new());
    r.on_control("control.resume", {{"command", "resume"}, {"reason", "post-mortem done"}, {"source", "watchdog"}, {"issued_at_utc", "x"}, {"details", {{"clear_halt", true}}}});
    CHECK(r.halted());   // wrong source
    r.on_control("control.resume", {{"command", "resume"}, {"reason", "post-mortem done"}, {"source", "operator"}, {"issued_at_utc", "x"}, {"details", {{"clear_halt", true}}}});
    CHECK(!r.halted());
    CHECK(!r.paused_new());
}

TEST_CASE("daily loss pauses for the day and clears next session", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(98000000, true, -2.0, -2.1));
    CHECK(r.paused_new());
    r.start_session(nyse().next_trading_day(f.session));
    CHECK(!r.paused_new());
}

TEST_CASE("consecutive losers pause requires operator resume", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio(100000000, true, 0, 0, 8));
    CHECK(r.paused_new());
    r.start_session(nyse().next_trading_day(f.session));
    CHECK(r.paused_new());
    r.on_control("control.resume", {{"command", "resume"}, {"reason", "reviewed"}, {"source", "operator"}, {"issued_at_utc", "x"}});
    CHECK(!r.paused_new());
}

TEST_CASE("exit sweep produces exit and stop-replace orders with deterministic ids", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    f.reconcile(r);
    nlohmann::json p = f.portfolio();
    PositionState pos;
    pos.symbol = "ABC"; pos.qty = 100; pos.avg_px_cents = 10000; pos.last_px_cents = 10500; pos.entry_session = make_date(2026, 8, 1);
    pos.exit_deadline = make_date(2026, 9, 16); pos.stop_px_cents = 9400; pos.hwm_px_cents = 10500; pos.atr20_cents = 200; pos.sector = "Technology";
    pos.stop_broker_order_id = "stop-leg-1";
    p["positions"] = nlohmann::json::array({pos.to_json()});
    r.on_portfolio_state(p);
    auto orders = r.end_of_session_sweep(make_date(2026, 9, 15));
    REQUIRE(orders.size() == 1);
    CHECK(orders[0]["intent"] == "STOP_REPLACE");
    CHECK(orders[0]["stop_px_cents"].get<Cents>() == 9900);
    CHECK(orders[0]["linked_broker_order_id"] == "stop-leg-1");
    auto orders2 = r.end_of_session_sweep(make_date(2026, 9, 16));
    REQUIRE(orders2.size() == 1);
    CHECK(orders2[0]["intent"] == "EXIT_TIME");
    CHECK(orders2[0]["side"] == "sell");
    CHECK(orders2[0]["qty"].get<int>() == 100);
    CHECK(orders2[0]["client_order_id"] == client_order_id_for_exit("ABC", "EXIT_TIME", "2026-09-16"));
}

TEST_CASE("a verdict that overtakes its candidate is retained and applied", "[risk]") {
    RiskFixture f;
    f.rl.shadow_mode = false;
    RiskManager r = f.make();
    f.reconcile(r);
    r.on_portfolio_state(f.portfolio());
    Envelope c = make_envelope("strategy", f.candidate("Z").to_json());
    Envelope v = make_envelope("validator", {{"candidate_msg_id", c.msg_id}, {"symbol", "Z"}, {"verdict", "APPROVE"}, {"confidence", 0.7}, {"reasons", nlohmann::json::array()},
                                             {"flags", nlohmann::json::array()}, {"model", "mock"}, {"latency_ms", 1}, {"cost_usd", 0.0}, {"citations", nlohmann::json::array()}, {"mode", "LIVE"}});
    r.on_validated(v);                    // arrives first
    nlohmann::json persisted = r.state_json();
    RiskManager r2 = f.make();
    r2.load_state(persisted);             // survives a restart in between
    f.reconcile(r2);
    r2.on_portfolio_state(f.portfolio());
    r2.on_candidate(c);
    REQUIRE(r2.pending_count() == 1);
    CHECK(r2.pending().front().verdict == std::optional<std::string>("APPROVE"));
    r2.on_quote("Z", f.quote());
    auto ds = r2.process_pending_entries(f.session, f.open);
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].approved);
}

TEST_CASE("state round trip", "[risk]") {
    RiskFixture f;
    RiskManager r = f.make();
    r.on_candidate(make_envelope("strategy", f.candidate("Q").to_json()));
    r.on_reconcile({{"status", "MISMATCH"}});
    nlohmann::json s = r.state_json();
    RiskManager r2 = f.make();
    r2.load_state(s);
    CHECK(r2.pending_count() == 1);
    CHECK(r2.paused_new());
    CHECK(r2.state_json() == s);
}
