#include "common/envelope.hpp"
#include "common/schema.hpp"
#include "risk/limits.hpp"
#include "strategy/params.hpp"
#include "strategy/tsmom_params.hpp"
#include "strategy/types.hpp"
#include "strategy/ledger.hpp"
#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace at;

namespace {
std::filesystem::path root() { return std::filesystem::path(AT_PROJECT_ROOT); }
} // namespace

TEST_CASE("schema registry loads topics and validates envelopes", "[schema]") {
    SchemaRegistry reg(root() / "schemas");
    CHECK(reg.streams().size() == 8);
    CHECK(reg.schema_for_subject("market.data.bar.AAPL") == "v1/market.data.schema.json");
    CHECK(reg.schema_for_subject("control.halt") == "v1/control.schema.json");
    CHECK(reg.schema_for_subject("nope.subject").empty());

    Envelope good = make_envelope("watchdog", {{"command", "halt"}, {"reason", "drill"}, {"source", "drill"}, {"issued_at_utc", now_utc_iso()}, {"drill", true}});
    CHECK(reg.validate("control.halt", good.to_json()).empty());

    Envelope bad = make_envelope("watchdog", {{"command", "explode"}, {"reason", "x"}, {"source", "drill"}, {"issued_at_utc", "x"}});
    CHECK(!reg.validate("control.halt", bad.to_json()).empty());
    CHECK_THROWS(reg.validate_or_throw("control.halt", bad.to_json()));

    nlohmann::json broken_env = good.to_json();
    broken_env["msg_id"] = "not-a-uuid";
    CHECK(!reg.validate("control.halt", broken_env).empty());
}

TEST_CASE("every domain type serialises to a schema-valid payload", "[schema]") {
    SchemaRegistry reg(root() / "schemas");

    EarningsEvent ev = test::synth_event("AAPL", make_date(2026, 10, 29));
    CHECK(reg.validate("events.earnings", make_envelope("events-feed", ev.to_json()).to_json()).empty());
    EarningsEvent back = EarningsEvent::from_json(ev.to_json());
    CHECK(back.event_id == ev.event_id);
    CHECK(back.next_report_date == ev.next_report_date);

    Candidate c;
    c.symbol = "AAPL"; c.strategy_version = "v"; c.event_id = ev.event_id; c.session_date = make_date(2026, 9, 15); c.ear_pct = 4.2; c.vol_ratio = 2.5; c.mom_pct = 77;
    c.entry_px_ref_cents = 10000; c.atr20_cents = 150; c.adv20_shares = 100000; c.sector = "Technology"; c.spy_above_trend = true;
    c.entry_deadline_date = make_date(2026, 9, 17); c.universe_snapshot_id = "abc"; c.thesis_facts = {"EPS 1.00 vs 0.90"}; c.data_as_of_utc = now_utc_iso();
    auto cj = c.to_json();
    CHECK(reg.validate("signals.candidate", make_envelope("strategy", cj).to_json()).empty());
    CHECK(Candidate::from_json(cj).entry_deadline_date == c.entry_deadline_date);

    Ledger l(100000000);
    EntryMeta m; m.sector = "Technology"; m.atr20_cents = 150; m.stop_px_cents = 9550; m.exit_deadline = make_date(2026, 11, 10);
    l.apply_entry_fill("AAPL", 100, 10000, 50, make_date(2026, 9, 15), now_utc_iso(), m);
    l.mark("AAPL", 10100);
    l.end_of_session(make_date(2026, 9, 15));
    auto ps = l.portfolio_state_payload(now_utc_iso(), make_date(2026, 9, 15), true, nlohmann::json::array(), 1, 1, 0, true);
    auto errs = reg.validate("portfolio.state", make_envelope("portfolio", ps).to_json());
    for (auto& e : errs) INFO(e);
    CHECK(errs.empty());
    CHECK(ps["equity_cents"].get<Cents>() == 100000000 - 100 * 10000 - 50 + 100 * 10100);
    CHECK(ps["gross_exposure_cents"].get<Cents>() == 1010000);

    nlohmann::json hb = {{"seq", 1}, {"service", "portfolio"}, {"healthy", true}, {"reconciled", true}, {"equity_cents", 1}, {"drawdown_pct", 0.0}, {"validator_error_streak", 0}, {"sent_at_utc", now_utc_iso()}};
    CHECK(reg.validate("watchdog.heartbeat", make_envelope("portfolio", hb).to_json()).empty());
}

TEST_CASE("frozen configs load and round trip", "[config]") {
    StrategyParams p = StrategyParams::load(root() / "config" / "strategy.v1.json");
    CHECK(p.strategy_version == "EARNINGS_MOMENTUM_V1.0");
    CHECK(p.signal.ear_threshold_pct == 3.0);
    CHECK(p.signal.momentum_top_pct == 40.0);
    CHECK(p.exit.drift_window_sessions == 40);
    CHECK(p.exit.trailing_stop_atr_mult == 3.0);
    CHECK(p.sizing.max_positions == 15);
    CHECK(p.universe.exclude_over_optioned.count("NVDA") == 1);
    CHECK(p.order.order_type == "oto");
    StrategyParams p2 = StrategyParams::from_json(p.to_json());
    CHECK(p2.fingerprint() == p.fingerprint());
    RiskLimits r = RiskLimits::load(root() / "config" / "risk.v1.json");
    CHECK(r.max_open_positions == 15);
    CHECK(r.drawdown_flatten_pct == -12.0);
    CHECK(r.shadow_mode);
    nlohmann::json bad = r.to_json();
    bad["gross_exposure_cap_pct"] = 150.0;
    CHECK_THROWS(RiskLimits::from_json(bad));
    CHECK(r.asset_class_cap_pct == 40.0);

    TsmomParams tp = TsmomParams::load(root() / "config" / "strategy.v3.json");
    CHECK(tp.strategy_version == "TSMOM_ETF_V1.0");
    CHECK(tp.signal.momentum_lookback_sessions == 252);
    CHECK(tp.signal.vol_lookback_sessions == 60);
    CHECK(tp.sizing.gross_cap_pct == 100.0);
    TsmomParams tp2 = TsmomParams::from_json(tp.to_json());
    CHECK(tp2.fingerprint() == tp.fingerprint());
}

TEST_CASE("TSMOM candidate and REBALANCE_TO_WEIGHT order are schema-valid; earnings-only fields stay required for that signal type", "[schema][tsmom]") {
    SchemaRegistry reg(root() / "schemas");

    Candidate c;
    c.symbol = "SPY";
    c.signal_type = "TSMOM_ETF_V1";
    c.strategy_version = "TSMOM_ETF_V1.0";
    c.event_id = "u|SPY|2026-09";
    c.session_date = make_date(2026, 9, 30);
    c.entry_deadline_date = make_date(2026, 10, 7);
    c.universe_snapshot_id = "u";
    c.thesis_facts = {"12-month momentum sign 1"};
    c.data_as_of_utc = now_utc_iso();
    c.asset_class = "EQUITY";
    c.mom_sign = 1;
    c.vol_annual_pct = 15.0;
    c.target_weight_pct = 5.0;
    auto cj = c.to_json();
    CHECK(reg.validate("signals.candidate", make_envelope("tsmom", cj).to_json()).empty());
    Candidate back = Candidate::from_json(cj);
    CHECK(back.signal_type == "TSMOM_ETF_V1");
    CHECK(back.asset_class == "EQUITY");
    REQUIRE(back.target_weight_pct.has_value());
    CHECK(*back.target_weight_pct == 5.0);

    // Missing target_weight_pct (a TSMOM_ETF_V1-conditional required field) is rejected.
    nlohmann::json missing = cj;
    missing.erase("target_weight_pct");
    CHECK_FALSE(reg.validate("signals.candidate", make_envelope("tsmom", missing).to_json()).empty());

    // A valid earnings candidate still validates after the schema relaxation (regression).
    Candidate e;
    e.symbol = "AAPL"; e.strategy_version = "v"; e.event_id = "ev-1"; e.session_date = make_date(2026, 9, 15);
    e.ear_pct = 4.2; e.vol_ratio = 2.5; e.mom_pct = 77; e.entry_px_ref_cents = 10000; e.atr20_cents = 150; e.adv20_shares = 100000;
    e.sector = "Technology"; e.spy_above_trend = true; e.entry_deadline_date = make_date(2026, 9, 17);
    e.universe_snapshot_id = "abc"; e.thesis_facts = {"EPS 1.00 vs 0.90"}; e.data_as_of_utc = now_utc_iso();
    CHECK(reg.validate("signals.candidate", make_envelope("strategy", e.to_json()).to_json()).empty());
    // An earnings candidate missing one of its own conditionally-required fields is rejected.
    nlohmann::json e_missing = e.to_json();
    e_missing.erase("atr20_cents");
    CHECK_FALSE(reg.validate("signals.candidate", make_envelope("strategy", e_missing).to_json()).empty());

    nlohmann::json rebalance_order = {
        {"client_order_id", "atr-0000000000000000000000000000000000"},
        {"candidate_msg_id", "cid"}, {"validated_msg_id", nullptr},
        {"intent", "REBALANCE_TO_WEIGHT"}, {"symbol", "SPY"}, {"side", "buy"}, {"qty", 500},
        {"order_type", "limit"}, {"limit_px_cents", 10020}, {"stop_px_cents", nullptr}, {"take_profit_px_cents", nullptr},
        {"linked_broker_order_id", nullptr}, {"tif", "day"}, {"extended_hours", false},
        {"reason", "TSMOM-v1 monthly rebalance"}, {"risk_checks", nlohmann::json::array()},
        {"equity_at_approval_cents", 100000000}, {"target_qty", 500}, {"asset_class", "EQUITY"},
    };
    CHECK(reg.validate("orders.approved", make_envelope("risk", rebalance_order).to_json()).empty());
}
