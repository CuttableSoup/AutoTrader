#include "backtester/backtester.hpp"

#include "common/ids.hpp"
#include "common/uuid.hpp"
#include "strategy/exits.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace at {

// ----------------------------------------------------------------- costs ----

Cents CostModel::buy_costs(std::int64_t qty, Cents px) const {
    Cents notional = qty * px;
    return mul(notional, commission_bps_per_side / 10000.0);
}

Cents CostModel::sell_costs(std::int64_t qty, Cents px) const {
    Cents notional = qty * px;
    Cents commission = mul(notional, commission_bps_per_side / 10000.0);
    // SEC fee: cents per million dollars sold -> notional_cents / 1e8 (dollars/1e6) * fee
    Cents sec = static_cast<Cents>(std::llround(static_cast<double>(notional) / 100000000.0 * static_cast<double>(sec_fee_per_million_dollars_sold_cents)));
    Cents taf = std::min(finra_taf_max_per_trade_cents, static_cast<Cents>(std::llround(static_cast<double>(qty) * finra_taf_per_share_cents)));
    return commission + sec + taf;
}

Cents CostModel::buy_fill_px(Cents ref) const { return half_spread_slippage ? ref + mul(ref, default_spread_bps / 2.0 / 10000.0) : ref; }
Cents CostModel::sell_fill_px(Cents ref) const { return half_spread_slippage ? std::max<Cents>(1, ref - mul(ref, default_spread_bps / 2.0 / 10000.0)) : ref; }

CostModel CostModel::from_json(const nlohmann::json& j) {
    CostModel c;
    if (j.contains("commission_bps_per_side")) c.commission_bps_per_side = j["commission_bps_per_side"].get<double>();
    if (j.contains("sec_fee_per_million_dollars_sold_cents")) c.sec_fee_per_million_dollars_sold_cents = j["sec_fee_per_million_dollars_sold_cents"].get<Cents>();
    if (j.contains("finra_taf_per_share_cents")) c.finra_taf_per_share_cents = j["finra_taf_per_share_cents"].get<double>();
    if (j.contains("finra_taf_max_per_trade_cents")) c.finra_taf_max_per_trade_cents = j["finra_taf_max_per_trade_cents"].get<Cents>();
    if (j.contains("half_spread_slippage")) c.half_spread_slippage = j["half_spread_slippage"].get<bool>();
    if (j.contains("default_spread_bps")) c.default_spread_bps = j["default_spread_bps"].get<double>();
    return c;
}

nlohmann::json CostModel::to_json() const {
    return {{"commission_bps_per_side", commission_bps_per_side}, {"sec_fee_per_million_dollars_sold_cents", sec_fee_per_million_dollars_sold_cents},
            {"finra_taf_per_share_cents", finra_taf_per_share_cents}, {"finra_taf_max_per_trade_cents", finra_taf_max_per_trade_cents},
            {"half_spread_slippage", half_spread_slippage}, {"default_spread_bps", default_spread_bps}};
}

// ---------------------------------------------------------------- config ----

BacktestConfig BacktestConfig::load(const std::filesystem::path& file) {
    Config c = Config::load(file);
    BacktestConfig b;
    b.run_name = c.get<std::string>("run_name", "run");
    b.data_dir = c.resolve(c.require<std::string>("data_dir"));
    b.out_dir = c.resolve(c.get<std::string>("out_dir", "var/backtest/" + b.run_name));
    b.strategy_config = c.resolve(c.get<std::string>("strategy_config", "config/strategy.v1.json"));
    b.risk_config = c.resolve(c.get<std::string>("risk_config", "config/risk.v1.json"));
    b.start = parse_date_or_throw(c.require<std::string>("start_date"));
    b.end = parse_date_or_throw(c.require<std::string>("end_date"));
    b.initial_equity_cents = c.get<Cents>("initial_equity_cents", 10000000);
    if (c.has("costs")) b.costs = CostModel::from_json(*c.find("costs"));
    b.validator_mode = c.get<std::string>("validator.mode", "mock");
    if (c.has("validator.mock")) b.mock = MockValidatorConfig::from_json(*c.find("validator.mock"));
    b.wf.folds = c.get<int>("walk_forward.folds", 3);
    b.wf.train_years = c.get<int>("walk_forward.train_years", 2);
    b.wf.test_years = c.get<int>("walk_forward.test_years", 1);
    b.wf.purge_sessions = c.get<int>("walk_forward.purge_sessions", 40);
    b.wf.embargo_sessions = c.get<int>("walk_forward.embargo_sessions", 10);
    b.wf.objective = c.get<std::string>("walk_forward.objective", "sharpe_net");
    b.report.haircut_pct = c.get<double>("report.haircut_pct", 50.0);
    b.report.risk_free_annual_pct = c.get<double>("report.risk_free_annual_pct", 0.0);
    b.report.min_gross_sharpe_after_haircut = c.get<double>("report.min_gross_sharpe_after_haircut", 0.4);
    return b;
}

// ------------------------------------------------------------------ data ----

std::vector<EarningsEvent> BacktestData::load_earnings_csv(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("backtest: cannot open " + file.string());
    std::string line;
    std::vector<EarningsEvent> out;
    if (!std::getline(in, line)) return out; if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> cols;
    { std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) cols.push_back(c); }
    std::map<std::string, int> idx;
    for (std::size_t i = 0; i < cols.size(); ++i) idx[cols[i]] = static_cast<int>(i);
    auto get = [&](const std::vector<std::string>& f, const char* n) -> std::string {
        auto it = idx.find(n);
        if (it == idx.end() || static_cast<std::size_t>(it->second) >= f.size()) return "";
        return f[static_cast<std::size_t>(it->second)];
    };
    std::vector<std::string> f;
    while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        f.clear();
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) f.push_back(c);
        EarningsEvent e;
        e.symbol = get(f, "symbol");
        e.report_date = parse_date_or_throw(get(f, "report_date"));
        e.timing = EarningsEvent::parse_timing(get(f, "timing"));
        e.fiscal_period = get(f, "fiscal_period");
        e.event_id = get(f, "event_id");
        if (e.event_id.empty()) e.event_id = earnings_event_id(e.symbol, e.fiscal_period, iso_date(e.report_date));
        auto num = [](const std::string& s) -> std::optional<double> { return s.empty() ? std::nullopt : std::optional<double>(std::stod(s)); };
        e.eps_actual = num(get(f, "eps_actual"));
        e.eps_consensus = num(get(f, "eps_consensus"));
        e.eps_consensus_asof = parse_date(get(f, "eps_consensus_asof"));
        if (auto s = get(f, "revenue_actual"); !s.empty()) e.revenue_actual_cents = parse_cents(s);
        if (auto s = get(f, "revenue_consensus"); !s.empty()) e.revenue_consensus_cents = parse_cents(s);
        e.next_report_date = parse_date(get(f, "next_report_date"));
        std::string k8 = get(f, "material_8k_dates");
        std::stringstream ks(k8);
        std::string d;
        while (std::getline(ks, d, ';')) if (auto dd = parse_date(d)) e.material_8k_dates.push_back(*dd);
        e.source = "replay";
        out.push_back(std::move(e));
    }
    return out;
}

BacktestData BacktestData::load(const std::filesystem::path& data_dir) {
    BacktestData d;
    std::size_t n = d.mkt.load_csv(data_dir / "bars.csv");
    d.securities = load_securities_csv(data_dir / "securities.csv");
    d.events = load_earnings_csv(data_dir / "earnings.csv");
    spdlog::info("backtest data: {} bars, {} symbols, {} security rows, {} earnings events", n, d.mkt.symbols().size(), d.securities.size(), d.events.size());
    return d;
}

nlohmann::json CandidateOutcome::to_json() const {
    return {{"symbol", symbol}, {"event_id", event_id}, {"signal_date", iso_date(signal_date)}, {"ear_pct", ear_pct}, {"mom_pct", mom_pct},
            {"verdict", verdict}, {"entered", entered}, {"risk_reject_reason", risk_reject_reason},
            {"fwd_return_pct", fwd_return_pct ? nlohmann::json(*fwd_return_pct) : nlohmann::json(nullptr)}};
}

nlohmann::json BacktestResult::to_json(bool include_series) const {
    nlohmann::json j = {
        {"params", params.to_json()}, {"start", iso_date(start)}, {"end", iso_date(end)}, {"sessions", sessions}, {"metrics", metrics.to_json()},
        {"candidates", candidates}, {"vetoed", vetoed}, {"approved", approved}, {"risk_rejected", risk_rejected}, {"reject_reasons", reject_reasons},
        {"halted_on", halted_on ? nlohmann::json(iso_date(*halted_on)) : nlohmann::json(nullptr)}, {"control_log", control_log},
    };
    if (include_series) {
        j["daily"] = nlohmann::json::array();
        for (const auto& d : daily) j["daily"].push_back({{"date", iso_date(d.date)}, {"equity_cents", d.equity_cents}, {"ret", d.ret}, {"n_positions", d.n_positions}, {"drawdown_pct", d.drawdown_pct}});
        j["trades"] = nlohmann::json::array();
        for (const auto& t : trades) j["trades"].push_back(t.to_json());
        j["outcomes"] = nlohmann::json::array();
        for (const auto& o : outcomes) j["outcomes"].push_back(o.to_json());
    }
    return j;
}

// ------------------------------------------------------------- simulation ---

Backtester::Backtester(const BacktestData& data, const BacktestConfig& cfg, RiskLimits limits)
    : data_(data), cfg_(cfg), limits_(std::move(limits)), cal_(nyse()) {}

namespace {

struct PendingExit {
    nlohmann::json order;   // orders.approved payload
};

} // namespace

BacktestResult Backtester::run(const StrategyParams& params, Date start, Date end, bool verbose) {
    BacktestResult res;
    res.params = params;
    res.start = start;
    res.end = end;

    RiskLimits limits = limits_;
    limits.shadow_mode = cfg_.validator_mode != "mock";   // mock: veto enforced; shadow/none: recorded only

    StrategyEngine engine(params, cal_);
    // Share bars by copying the store once per run (the engine owns its MarketStore).
    engine.market() = data_.mkt;
    for (const auto& ev : data_.events) engine.on_earnings_event(ev);
    std::map<std::string, const EarningsEvent*> events_by_id;
    for (const auto& [id, ev] : engine.events()) events_by_id[id] = &ev;

    RiskManager risk(params, limits, cal_);
    // The simulated broker is always in sync with the ledger: report a clean startup reconcile.
    risk.on_reconcile({{"status", "CLEAN"}, {"trigger", "STARTUP"}, {"checked_at_utc", iso_utc(from_ny(start, 9, 0))}, {"broker_positions", nlohmann::json::array()},
                       {"internal_positions", nlohmann::json::array()}, {"diffs", nlohmann::json::array()}, {"open_orders_count", 0}, {"orphan_orders", nlohmann::json::array()}});
    Ledger ledger(cfg_.initial_equity_cents);
    MockValidator mock(cfg_.mock, params.universe.exclude_over_optioned, cal_);
    const MarketStore& mkt = engine.market();

    std::vector<Date> sessions = cal_.sessions(start, end);
    res.sessions = static_cast<int>(sessions.size());
    std::vector<PendingExit> pending_exits;
    std::map<std::string, std::size_t> outcome_index;   // candidate msg_id -> outcomes index
    std::map<std::string, EntryMeta> entry_meta;        // client_order_id -> meta for fills
    std::map<std::string, std::string> cand_by_order;   // client_order_id -> candidate msg_id
    std::optional<Date> flatten_next_open;
    int last_universe_month = -1;
    bool halted = false;
    Cents prev_equity = cfg_.initial_equity_cents;

    auto record_control = [&](const std::string& subject, const nlohmann::json& p, Date d) {
        res.control_log.push_back(iso_date(d) + " " + subject + " " + p.value("trigger", "") + ": " + p.value("reason", ""));
        if (subject == "control.flatten") flatten_next_open = cal_.next_trading_day(d);
        if (subject == "control.halt") { halted = true; if (!res.halted_on) res.halted_on = d; }
    };

    for (Date d : sessions) {
        // ---- monthly universe rebuild (point-in-time) ----
        if (month_of(d) != last_universe_month) {
            last_universe_month = month_of(d);
            auto secs = securities_as_of(data_.securities, cal_.prev_trading_day(d));
            engine.set_universe(build_universe(secs, mkt, cal_.prev_trading_day(d), params.universe));
            if (verbose) spdlog::info("{} universe rebuilt: {} members", iso_date(d), engine.universe().symbols.size());
        }
        ledger.start_session(d);
        Cents day_costs = 0;
        const std::string ts_open = iso_utc(from_ny(d, 9, 30));

        // ---- open: flatten if ordered ----
        if (flatten_next_open && *flatten_next_open <= d) {
            for (auto pos : ledger.positions()) {
                auto b = mkt.bar_on(pos.symbol, d);
                Cents ref = b ? b->open : pos.last_px_cents;
                Cents px = cfg_.costs.sell_fill_px(ref);
                Cents c = cfg_.costs.sell_costs(pos.qty, px);
                day_costs += c;
                ledger.apply_exit_fill(pos.symbol, pos.qty, px, c, d, "FLATTEN");
            }
            flatten_next_open.reset();
            pending_exits.clear();
        }

        // ---- open: pending exits from last close's sweep ----
        for (const auto& pe : pending_exits) {
            const auto& o = pe.order;
            std::string sym = o["symbol"].get<std::string>();
            auto pos = ledger.position(sym);
            if (!pos) continue;
            auto b = mkt.bar_on(sym, d);
            if (!b) continue;
            std::int64_t qty = std::min<std::int64_t>(o["qty"].get<std::int64_t>(), pos->qty);
            Cents px = cfg_.costs.sell_fill_px(b->open);
            Cents c = cfg_.costs.sell_costs(qty, px);
            day_costs += c;
            std::string intent = o["intent"].get<std::string>();
            ledger.apply_exit_fill(sym, qty, px, c, d, intent);
            if (intent == "EXIT_TREND_SCALE") ledger.mark_scaled_down(sym);
        }
        pending_exits.clear();

        // ---- open: entries ----
        if (!halted) {
            // Synthesize fresh quotes from the open for every pending candidate.
            for (const auto& pc : risk.pending()) {
                auto b = mkt.bar_on(pc.cand.symbol, d);
                if (!b) continue;
                Quote q;
                q.bid_cents = cfg_.costs.sell_fill_px(b->open);
                q.ask_cents = cfg_.costs.buy_fill_px(b->open);
                if (q.ask_cents <= q.bid_cents) q.ask_cents = q.bid_cents + 1;
                q.bid_size = q.ask_size = 100;
                q.ts = from_ny(d, 9, 30);
                risk.on_quote(pc.cand.symbol, q);
            }
            auto decisions = risk.process_pending_entries(d, from_ny(d, 9, 30) + std::chrono::seconds(5));
            for (const auto& dec : decisions) {
                auto oi = outcome_index.find(dec.candidate_msg_id);
                if (!dec.approved) {
                    ++res.risk_rejected;
                    ++res.reject_reasons[dec.reject_reason];
                    if (oi != outcome_index.end()) res.outcomes[oi->second].risk_reject_reason = dec.reject_reason;
                    continue;
                }
                ++res.approved;
                const auto& o = dec.order;
                std::string sym = o["symbol"].get<std::string>();
                std::int64_t qty = o["qty"].get<std::int64_t>();
                auto b = mkt.bar_on(sym, d);
                if (!b) continue;
                Cents px = cfg_.costs.buy_fill_px(b->open);
                Cents limit = o["limit_px_cents"].get<Cents>();
                if (px > limit) { ++res.reject_reasons["limit_not_marketable"]; continue; }
                Cents c = cfg_.costs.buy_costs(qty, px);
                day_costs += c;
                EntryMeta m;
                m.atr20_cents = o["atr20_cents"].get<Cents>();
                m.stop_px_cents = o["stop_px_cents"].get<Cents>();
                m.candidate_msg_id = dec.candidate_msg_id;
                if (oi != outcome_index.end()) {
                    const auto& oc = res.outcomes[oi->second];
                    m.event_id = oc.event_id;
                    m.ear_pct = oc.ear_pct;
                    m.mom_pct = oc.mom_pct;
                    m.verdict = oc.verdict;
                    res.outcomes[oi->second].entered = true;
                }
                auto evit = events_by_id.find(m.event_id);
                std::optional<Date> next_rep = evit != events_by_id.end() ? evit->second->next_report_date : std::nullopt;
                if (!next_rep) next_rep = engine.next_report_date(sym, d);
                m.next_report_date = next_rep;
                m.exit_deadline = compute_exit_deadline(d, next_rep, params.exit, cal_);
                m.sector = engine.universe().sector_of(sym);
                ledger.apply_entry_fill(sym, qty, px, c, d, ts_open, m);
                risk.on_order_filled({{"intent", "ENTRY"}, {"symbol", sym}});
            }
        }

        // ---- intraday: resting stops ----
        for (auto pos : ledger.positions()) {
            if (!pos.stop_px_cents) continue;
            auto b = mkt.bar_on(pos.symbol, d);
            if (!b) continue;
            Cents stop = *pos.stop_px_cents;
            bool entered_today = pos.entry_session == d;
            Cents fill = 0;
            if (!entered_today && b->open <= stop) fill = b->open;      // gapped through the stop
            else if (b->low <= stop) fill = stop;
            if (fill == 0) continue;
            Cents px = cfg_.costs.sell_fill_px(fill);
            Cents c = cfg_.costs.sell_costs(pos.qty, px);
            day_costs += c;
            ledger.apply_exit_fill(pos.symbol, pos.qty, px, c, d, "STOP_LEG");
        }

        // ---- close: marks, portfolio state, signals, sweep ----
        ledger.mark_all_from(mkt, d);
        ledger.end_of_session(d);
        auto sr = engine.evaluate_session(d, iso_utc(from_ny(d, 16, 0)));
        nlohmann::json pstate = ledger.portfolio_state_payload(iso_utc(from_ny(d, 16, 1)), d, true, nlohmann::json::array(),
                                                               risk.counters().new_positions_today, risk.counters().orders_today, 0, sr.spy_above_trend);
        risk.on_portfolio_state(pstate);

        for (const auto& c : sr.candidates) {
            ++res.candidates;
            Envelope env = make_envelope("strategy", c.to_json());
            risk.on_candidate(env);
            CandidateOutcome oc;
            oc.symbol = c.symbol;
            oc.event_id = c.event_id;
            oc.signal_date = d;
            oc.ear_pct = c.ear_pct;
            oc.mom_pct = c.mom_pct;
            // Forward return for the counterfactual: next open -> close after drift window.
            Date e0 = cal_.next_trading_day(d);
            Date e1 = cal_.add_sessions(e0, params.exit.drift_window_sessions);
            auto b0 = mkt.bar_on(c.symbol, e0);
            auto b1 = mkt.last_bar_on_or_before(c.symbol, e1);
            if (b0 && b1 && b1->date > e0) oc.fwd_return_pct = simple_return(b0->open, b1->close) * 100.0;
            if (cfg_.validator_mode != "none") {
                auto evit = events_by_id.find(c.event_id);
                MockVerdict mv = mock.validate(c, evit != events_by_id.end() ? evit->second : nullptr, mkt, e0, params.exit.drift_window_sessions);
                oc.verdict = mv.verdict;
                if (mv.verdict != "APPROVE") ++res.vetoed;
                Envelope venv = make_envelope("validator", mv.to_payload(env.msg_id, c.symbol, cfg_.validator_mode == "mock" ? "MOCK" : "SHADOW"));
                risk.on_validated(venv);
            } else {
                oc.verdict = "NONE";
            }
            outcome_index[env.msg_id] = res.outcomes.size();
            res.outcomes.push_back(std::move(oc));
        }

        auto sweep = risk.end_of_session_sweep(d);
        for (auto& o : sweep) {
            std::string intent = o["intent"].get<std::string>();
            if (intent == "STOP_REPLACE") ledger.set_stop(o["symbol"].get<std::string>(), o["stop_px_cents"].get<Cents>());
            else pending_exits.push_back(PendingExit{std::move(o)});
        }
        for (auto& [subj, p] : risk.take_control_messages()) {
            if (p.contains("details") && p["details"].value("informational", false)) { res.control_log.push_back(iso_date(d) + " " + subj + " (info) " + p.value("reason", "")); continue; }
            record_control(subj, p, d);
            risk.on_control(subj, p);
            // Live, CONSECUTIVE_LOSERS and REJECT_RATE pauses wait for an operator. The backtest emulates an
            // operator who reviews and resumes at the next session so the statistics are not truncated;
            // the pause itself stays in control_log so the frequency is visible in the report.
            std::string trig = p.value("trigger", "");
            if (subj == "control.pause_new" && (trig == "CONSECUTIVE_LOSERS" || trig == "REJECT_RATE"))
                risk.on_control("control.resume", {{"command", "resume"}, {"reason", "backtest: emulated operator review"}, {"source", "operator"}, {"issued_at_utc", iso_utc(from_ny(d, 17, 0))}});
        }

        // ---- daily returns (net / gross) ----
        const auto& rec = ledger.daily().back();
        res.daily_net.push_back(rec.ret);
        res.daily_gross.push_back(prev_equity > 0 ? rec.ret + static_cast<double>(day_costs) / static_cast<double>(prev_equity) : rec.ret);
        prev_equity = rec.equity_cents;
    }

    res.daily = ledger.daily();
    res.trades = ledger.trades();
    res.metrics = compute_metrics(res.daily_net, res.daily_gross, res.trades, 252.0, cfg_.report.risk_free_annual_pct);
    return res;
}

} // namespace at
