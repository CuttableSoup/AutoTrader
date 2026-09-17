#include "risk/risk_manager.hpp"

#include "common/ids.hpp"
#include "strategy/exits.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace at {

RiskManager::RiskManager(StrategyParams sp, RiskLimits rl, const TradingCalendar& cal, std::optional<TsmomParams> tp)
    : sp_(std::move(sp)), tp_(std::move(tp)), limits_(std::move(rl)), cal_(cal) {}

// ---------------------------------------------------------------- inputs ----

void RiskManager::on_candidate(const Envelope& env) {
    if (!seen_candidates_.first_time(env.msg_id)) return; // at-least-once bus: ignore redeliveries
    PendingCandidate pc;
    pc.msg_id = env.msg_id;
    pc.cand = Candidate::from_json(env.payload);
    pc.received_utc = now_utc_iso();
    if (auto it = early_verdicts_.find(env.msg_id); it != early_verdicts_.end()) {
        // The verdict overtook the candidate on the bus (two consumers, no ordering guarantee between them).
        pc.verdict = it->second.first;
        pc.validated_msg_id = it->second.second;
        early_verdicts_.erase(it);
    }
    pending_.push_back(std::move(pc));
}

void RiskManager::on_validated(const Envelope& env) {
    const auto& p = env.payload;
    std::string cid = p.value("candidate_msg_id", "");
    std::string verdict = p.value("verdict", "ERROR");
    if (verdict == "ERROR") ++counters_.validator_error_streak; else counters_.validator_error_streak = 0;
    if (counters_.validator_error_streak >= limits_.validator_error_streak_pause && !paused_new_) {
        paused_new_ = true;
        pause_reason_ = "validator error streak";
        emit_control("pause_new", "VALIDATOR_ERROR_STREAK", "validator returned ERROR " + std::to_string(counters_.validator_error_streak) + " times in a row");
    }
    for (auto& pc : pending_) {
        if (pc.msg_id == cid) {
            pc.verdict = verdict;
            pc.validated_msg_id = env.msg_id;
            return;
        }
    }
    // No matching candidate yet: keep the verdict so it is applied when the candidate arrives (bounded).
    if (!cid.empty()) {
        early_verdicts_[cid] = {verdict, env.msg_id};
        if (early_verdicts_.size() > 1000) early_verdicts_.erase(early_verdicts_.begin());
    }
}

void RiskManager::on_portfolio_state(const nlohmann::json& p) {
    PortfolioView v;
    v.valid = true;
    v.reconciled = p.value("reconciled", false);
    v.as_of_utc = p.value("as_of_utc", "");
    if (auto d = parse_date(p.value("session_date", ""))) v.session = *d;
    v.equity_cents = p.value("equity_cents", 0LL);
    v.cash_cents = p.value("cash_cents", 0LL);
    v.buying_power_cents = p.value("buying_power_cents", 0LL);
    v.margin_buying_power_cents = p.value("margin_buying_power_cents", 0LL);
    v.gross_exposure_cents = p.value("gross_exposure_cents", 0LL);
    if (p.contains("sector_exposure_pct"))
        for (auto& [k, val] : p["sector_exposure_pct"].items()) v.sector_exposure_pct[k] = val.get<double>();
    if (p.contains("asset_class_exposure_pct"))
        for (auto& [k, val] : p["asset_class_exposure_pct"].items()) v.asset_class_exposure_pct[k] = val.get<double>();
    v.hwm_equity_cents = p.value("hwm_equity_cents", v.equity_cents);
    v.drawdown_pct = p.value("drawdown_pct", 0.0);
    v.daily_pnl_pct = p.value("daily_pnl_pct", 0.0);
    if (p.contains("counters")) {
        v.consecutive_losers = p["counters"].value("consecutive_losers", 0);
        v.closed_trades_total = p["counters"].value("closed_trades_total", 0);
    }
    if (p.contains("spy_above_trend") && p["spy_above_trend"].is_boolean()) v.spy_above_trend = p["spy_above_trend"].get<bool>();
    if (p.contains("realized_vol_annual_pct") && p["realized_vol_annual_pct"].is_number()) v.realized_vol_annual = p["realized_vol_annual_pct"].get<double>() / 100.0;
    if (p.contains("positions"))
        for (const auto& pj : p["positions"]) v.positions.push_back(PositionState::from_json(pj));
    if (p.contains("open_orders")) v.open_orders = static_cast<int>(p["open_orders"].size());
    pf_ = std::move(v);
    // Approved entries that now show up as positions are no longer "pending exposure".
    for (auto it = approved_by_symbol_.begin(); it != approved_by_symbol_.end();)
        if (position(it->first)) it = approved_by_symbol_.erase(it); else ++it;
    apply_drawdown_rules();

    if (limits_.consecutive_losers_pause > 0 && pf_.consecutive_losers >= limits_.consecutive_losers_pause && !paused_new_) {
        paused_new_ = true;
        pause_reason_ = "consecutive losers";
        emit_control("pause_new", "CONSECUTIVE_LOSERS", std::to_string(pf_.consecutive_losers) + " consecutive losing trades");
    }
    if (pf_.daily_pnl_pct <= limits_.daily_loss_pause_pct && !daily_loss_paused_today_) {
        daily_loss_paused_today_ = true;
        if (!paused_new_) { paused_new_ = true; pause_reason_ = "daily loss"; }
        emit_control("pause_new", "DAILY_LOSS", "daily P&L " + std::to_string(pf_.daily_pnl_pct) + "% breached " + std::to_string(limits_.daily_loss_pause_pct) + "%");
    }
}

void RiskManager::apply_drawdown_rules() {
    if (!pf_.valid) return;
    if (pf_.drawdown_pct <= limits_.drawdown_flatten_pct) {
        if (!flatten_issued_) {
            flatten_issued_ = true;
            halted_ = true;
            paused_new_ = true;
            pause_reason_ = "drawdown flatten";
            emit_control("flatten", "DRAWDOWN_FLATTEN", "drawdown " + std::to_string(pf_.drawdown_pct) + "% breached " + std::to_string(limits_.drawdown_flatten_pct) + "%");
            emit_control("halt", "DRAWDOWN_FLATTEN", "halted after flatten; operator review required");
        }
        return;
    }
    if (pf_.drawdown_pct <= limits_.drawdown_halve_pct) {
        if (!size_halved_) {
            size_halved_ = true;
            emit_control("pause_new", "DRAWDOWN_HALVE", "drawdown " + std::to_string(pf_.drawdown_pct) + "%: new positions sized at 50% (not paused)", {{"informational", true}});
            // informational: pause_new is NOT set; message records the regime change for the logs/pager.
        }
    } else if (size_halved_ && pf_.drawdown_pct > limits_.drawdown_halve_pct / 2.0) {
        size_halved_ = false; // recovered to half the halve threshold
    }
}

void RiskManager::on_reconcile(const nlohmann::json& p) {
    std::string status = p.value("status", "ERROR");
    reconcile_clean_ = status == "CLEAN";
    if (!reconcile_clean_ && !paused_new_) {
        paused_new_ = true;
        pause_reason_ = "reconcile " + status;
        emit_control("pause_new", "RECONCILE_MISMATCH", "broker reconcile status " + status, {{"diffs", p.value("diffs", nlohmann::json::array())}});
    }
}

void RiskManager::on_control(const std::string& subject, const nlohmann::json& p) {
    std::string cmd = p.value("command", "");
    if (subject.ends_with(".halt") || cmd == "halt") { halted_ = true; paused_new_ = true; pause_reason_ = "halt: " + p.value("reason", ""); }
    else if (subject.ends_with(".flatten") || cmd == "flatten") { paused_new_ = true; pause_reason_ = "flatten: " + p.value("reason", ""); }
    else if (subject.ends_with(".pause_new") || cmd == "pause_new") {
        if (!p.contains("details") || !p["details"].value("informational", false)) { paused_new_ = true; pause_reason_ = p.value("reason", ""); }
    } else if (subject.ends_with(".resume") || cmd == "resume") {
        bool clear_halt = p.value("source", "") == "operator" && p.value("details", nlohmann::json::object()).value("clear_halt", false);
        if (halted_ && clear_halt) { halted_ = false; flatten_issued_ = false; spdlog::warn("risk: halt cleared by operator ({})", p.value("reason", "")); }
        if (!halted_) { paused_new_ = false; pause_reason_.clear(); counters_.validator_error_streak = 0; counters_.validator_missing_streak = 0; }
        else spdlog::warn("risk: resume ignored while halted (send details.clear_halt=true from source=operator after the post-mortem)");
    }
}

void RiskManager::on_quote(const std::string& symbol, const Quote& q) { quotes_[symbol] = q; }

void RiskManager::on_shortable(const std::string& symbol, bool shortable) { shortable_[symbol] = shortable; }

void RiskManager::on_order_status(const nlohmann::json& p) {
    std::string ev = p.value("event", "");
    if (ev == "new" || ev == "accepted") ++counters_.submitted_today;
    if (ev == "rejected") {
        ++counters_.rejects_today;
        if (counters_.submitted_today + counters_.rejects_today >= 5) {
            double rate = 100.0 * counters_.rejects_today / static_cast<double>(counters_.submitted_today + counters_.rejects_today);
            if (rate > limits_.reject_rate_page_pct && !paused_new_) {
                paused_new_ = true;
                pause_reason_ = "order reject rate";
                emit_control("pause_new", "REJECT_RATE", "broker reject rate " + std::to_string(rate) + "% today");
            }
        }
    }
}

void RiskManager::on_order_filled(const nlohmann::json& p) {
    if (p.value("intent", "") == "ENTRY") approved_by_symbol_.erase(p.value("symbol", ""));
}

void RiskManager::start_session(Date session) {
    if (counters_.session == session) return;
    counters_.session = session;
    counters_.new_positions_today = 0;
    counters_.orders_today = 0;
    counters_.rejects_today = 0;
    counters_.submitted_today = 0;
    if (daily_loss_paused_today_) {
        daily_loss_paused_today_ = false;
        if (pause_reason_ == "daily loss" && !halted_) { paused_new_ = false; pause_reason_.clear(); }
    }
    // Drop candidates past their entry deadline.
    std::size_t before = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const PendingCandidate& pc) { return pc.cand.entry_deadline_date < session; }), pending_.end());
    if (pending_.size() != before) spdlog::info("risk: dropped {} expired candidates", before - pending_.size());
}

// ------------------------------------------------------------- decisions ----

std::optional<PositionState> RiskManager::position(const std::string& symbol) const {
    for (const auto& p : pf_.positions) if (p.symbol == symbol) return p;
    return std::nullopt;
}

Cents RiskManager::sector_exposure_cents(const std::string& sector) const {
    Cents c = 0;
    for (const auto& p : pf_.positions) if (p.sector == sector) c += p.market_value_cents();
    return c;
}

Cents RiskManager::asset_class_exposure_cents(const std::string& asset_class) const {
    Cents c = 0;
    // Gross, not net: a short still consumes asset-class risk budget.
    for (const auto& p : pf_.positions) if (p.asset_class == asset_class) c += std::llabs(p.market_value_cents());
    return c;
}

EntryDecision RiskManager::evaluate_entry(const PendingCandidate& pc, Date session, SysTime now) {
    EntryDecision d;
    d.candidate_msg_id = pc.msg_id;
    d.symbol = pc.cand.symbol;
    const Candidate& c = pc.cand;
    auto check = [&](std::string name, bool ok, nlohmann::json value = nullptr, nlohmann::json limit = nullptr) {
        d.checks.push_back(RiskCheck{std::move(name), ok, std::move(value), std::move(limit)});
        if (!ok && d.reject_reason.empty()) d.reject_reason = d.checks.back().name;
        return ok;
    };

    check("halted", !halted_, halted_);
    check("paused_new", !paused_new_, paused_new_, pause_reason_);
    check("portfolio_state_present", pf_.valid);
    check("reconciled", pf_.valid && pf_.reconciled && reconcile_clean_);
    // Candidate freshness: built on data no older than one session.
    int age = cal_.sessions_between(c.session_date, session);
    check("candidate_age_sessions", age <= limits_.max_candidate_age_sessions && age >= 0, age, limits_.max_candidate_age_sessions);
    check("entry_deadline", session <= c.entry_deadline_date, iso_date(c.entry_deadline_date));
    check("spy_above_trend_at_signal", c.spy_above_trend);
    if (pf_.spy_above_trend) check("spy_above_trend_now", *pf_.spy_above_trend);

    // Validator
    std::string verdict = pc.verdict.value_or("MISSING");
    if (limits_.shadow_mode) check("validator_verdict_shadow", true, verdict);
    else check("validator_verdict", verdict == "APPROVE", verdict, "APPROVE");

    // Earnings gate: never hold through the next scheduled earnings.
    if (limits_.earnings_gate && c.next_report_date) {
        Date first_exit = cal_.add_sessions(session, 1);
        check("earnings_gate", *c.next_report_date > first_exit, iso_date(*c.next_report_date));
    } else {
        check("earnings_gate", true, "next_report_date unknown; deadline = drift window, updated when scheduled");
    }

    // Position / order counters
    bool already = position(c.symbol).has_value() || approved_by_symbol_.count(c.symbol) > 0;
    check("not_already_held", !already);
    int open_positions = static_cast<int>(pf_.positions.size()) + static_cast<int>(approved_by_symbol_.size());
    check("max_open_positions", open_positions < limits_.max_open_positions, open_positions, limits_.max_open_positions);
    check("max_new_positions_per_day", counters_.new_positions_today < limits_.max_new_positions_per_day, counters_.new_positions_today, limits_.max_new_positions_per_day);
    check("max_orders_per_day", counters_.orders_today < limits_.max_orders_per_day, counters_.orders_today, limits_.max_orders_per_day);

    // Quote: freshness and spread
    auto qit = quotes_.find(c.symbol);
    Cents ref_px = c.entry_px_ref_cents;
    if (qit == quotes_.end()) {
        check("quote_present", false);
    } else {
        const Quote& q = qit->second;
        auto age_s = std::chrono::duration_cast<std::chrono::seconds>(now - q.ts).count();
        check("quote_age_s", age_s <= limits_.max_quote_age_s && age_s >= -5, static_cast<long long>(age_s), limits_.max_quote_age_s);
        check("not_halted", !q.halted);
        check("spread_bps", q.bid_cents > 0 && q.ask_cents > q.bid_cents && q.spread_bps() <= limits_.max_spread_bps, q.spread_bps(), limits_.max_spread_bps);
        if (q.ask_cents > 0) ref_px = q.ask_cents;
    }
    Cents limit_px = round_to_tick(ref_px + mul(ref_px, sp_.order.entry_limit_offset_bps / 10000.0));

    // Sizing
    SizingInput si;
    si.equity_cents = pf_.equity_cents;
    si.entry_px_cents = limit_px;
    si.atr20_cents = c.atr20_cents;
    si.book_vol_annual = pf_.realized_vol_annual;
    si.open_positions = open_positions;
    si.gross_exposure_cents = pf_.gross_exposure_cents;
    si.sector_exposure_cents = sector_exposure_cents(c.sector);
    si.per_position_risk_pct = limits_.per_position_risk_pct;
    si.gross_cap_pct = limits_.gross_exposure_cap_pct;
    si.size_halved = size_halved_;
    // Stock vol proxy from ATR: ATR/price * sqrt(252) approximates daily-range vol.
    if (c.entry_px_ref_cents > 0) si.stock_vol_annual = static_cast<double>(c.atr20_cents) / static_cast<double>(c.entry_px_ref_cents) * std::sqrt(252.0);
    d.sizing = size_position(si, sp_.sizing, sp_.exit);
    std::int64_t qty = d.sizing.qty;

    // Size guard: order <= 1% of 20-day ADV (shares).
    std::int64_t adv_cap = static_cast<std::int64_t>(static_cast<double>(c.adv20_shares) * limits_.max_order_pct_of_adv20 / 100.0);
    if (qty > adv_cap) { qty = adv_cap; d.sizing.binding = "adv_size_guard"; }
    check("size_guard_adv", qty <= adv_cap, qty, adv_cap);
    check("qty_positive", qty >= 1, qty, d.sizing.binding);
    Cents notional = qty * limit_px;
    check("single_name_cap_pct", pct_of(notional, pf_.equity_cents) <= limits_.single_name_cap_pct + 1e-9, pct_of(notional, pf_.equity_cents), limits_.single_name_cap_pct);
    check("gross_exposure_cap_pct", pct_of(pf_.gross_exposure_cents + notional, pf_.equity_cents) <= limits_.gross_exposure_cap_pct + 1e-9, pct_of(pf_.gross_exposure_cents + notional, pf_.equity_cents), limits_.gross_exposure_cap_pct);
    check("sector_cap_pct", pct_of(si.sector_exposure_cents + notional, pf_.equity_cents) <= limits_.sector_cap_pct + 1e-9, pct_of(si.sector_exposure_cents + notional, pf_.equity_cents), limits_.sector_cap_pct);
    check("buying_power", notional <= pf_.buying_power_cents || pf_.buying_power_cents == 0, notional, pf_.buying_power_cents);
    Cents stop_px = d.sizing.stop_px_cents;
    Cents risk_at_stop = qty * (limit_px - stop_px);
    check("per_position_risk_pct", pct_of(risk_at_stop, pf_.equity_cents) <= limits_.per_position_risk_pct + 1e-9, pct_of(risk_at_stop, pf_.equity_cents), limits_.per_position_risk_pct);

    bool all_ok = std::all_of(d.checks.begin(), d.checks.end(), [](const RiskCheck& r) { return r.ok; });
    if (!all_ok) return d;

    nlohmann::json checks = nlohmann::json::array();
    for (const auto& r : d.checks) checks.push_back(r.to_json());
    nlohmann::json order = {
        {"client_order_id", client_order_id_for_entry(pc.msg_id)},
        {"candidate_msg_id", pc.msg_id},
        {"validated_msg_id", pc.validated_msg_id ? nlohmann::json(*pc.validated_msg_id) : nlohmann::json(nullptr)},
        {"intent", "ENTRY"},
        {"symbol", c.symbol},
        {"side", "buy"},
        {"qty", qty},
        {"order_type", sp_.order.order_type},
        {"limit_px_cents", limit_px},
        {"stop_px_cents", stop_px},
        {"take_profit_px_cents", nullptr},
        {"linked_broker_order_id", nullptr},
        {"tif", sp_.order.tif},
        {"extended_hours", false},
        {"reason", "earnings-momentum v1 entry; binding=" + d.sizing.binding},
        {"risk_checks", checks},
        {"equity_at_approval_cents", pf_.equity_cents},
        {"atr20_cents", c.atr20_cents},
    };
    if (sp_.order.order_type == "bracket" && sp_.exit.take_profit_atr_mult)
        order["take_profit_px_cents"] = round_to_tick(limit_px + mul(c.atr20_cents, *sp_.exit.take_profit_atr_mult));
    d.order = std::move(order);
    d.approved = true;
    return d;
}

EntryDecision RiskManager::evaluate_rebalance(const PendingCandidate& pc, Date session, SysTime now) {
    EntryDecision d;
    d.candidate_msg_id = pc.msg_id;
    d.symbol = pc.cand.symbol;
    const Candidate& c = pc.cand;
    auto check = [&](std::string name, bool ok, nlohmann::json value = nullptr, nlohmann::json limit = nullptr) {
        d.checks.push_back(RiskCheck{std::move(name), ok, std::move(value), std::move(limit)});
        if (!ok && d.reject_reason.empty()) d.reject_reason = d.checks.back().name;
        return ok;
    };

    check("tsmom_configured", tp_.has_value());
    check("halted", !halted_, halted_);
    check("paused_new", !paused_new_, paused_new_, pause_reason_);
    check("portfolio_state_present", pf_.valid);
    check("reconciled", pf_.valid && pf_.reconciled && reconcile_clean_);
    int age = cal_.sessions_between(c.session_date, session);
    check("candidate_age_sessions", age <= limits_.max_candidate_age_sessions && age >= 0, age, limits_.max_candidate_age_sessions);
    check("entry_deadline", session <= c.entry_deadline_date, iso_date(c.entry_deadline_date));
    check("max_orders_per_day", counters_.orders_today < limits_.max_orders_per_day, counters_.orders_today, limits_.max_orders_per_day);

    // Quote: freshness and spread, direction-neutral (mid) -- unlike evaluate_entry's
    // always-buy ask price, a rebalance may end up buying or selling depending on the
    // sign of (target - current), which isn't known until sizing below.
    Cents ref_px = 0;
    auto qit = quotes_.find(c.symbol);
    if (qit == quotes_.end()) {
        check("quote_present", false);
    } else {
        const Quote& q = qit->second;
        auto age_s = std::chrono::duration_cast<std::chrono::seconds>(now - q.ts).count();
        check("quote_age_s", age_s <= limits_.max_quote_age_s && age_s >= -5, static_cast<long long>(age_s), limits_.max_quote_age_s);
        check("not_halted", !q.halted);
        check("spread_bps", q.bid_cents > 0 && q.ask_cents > q.bid_cents && q.spread_bps() <= limits_.max_spread_bps, q.spread_bps(), limits_.max_spread_bps);
        if (q.bid_cents > 0 && q.ask_cents > 0) ref_px = q.mid();
    }
    check("ref_px_present", ref_px > 0, ref_px);

    // Can't size without a valid reference price -- a missing quote must never be read as
    // "target is zero, flatten the position."
    if (!std::all_of(d.checks.begin(), d.checks.end(), [](const RiskCheck& r) { return r.ok; })) return d;

    TsmomPositionSizingInput si;
    si.equity_cents = pf_.equity_cents;
    si.ref_px_cents = ref_px;
    si.target_weight = c.target_weight_pct.value_or(0.0) / 100.0;
    auto sized = size_tsmom_target(si);

    auto cur = position(c.symbol);
    std::int64_t current_qty = cur ? cur->qty : 0;
    std::int64_t target_qty = sized.target_qty;
    std::int64_t delta = target_qty - current_qty;
    if (!check("no_op", delta != 0, target_qty, current_qty)) return d;

    std::int64_t qty_abs = std::llabs(delta);
    std::string side = delta > 0 ? "buy" : "sell";
    double offset = tp_->order.rebalance_limit_offset_bps / 10000.0;
    Cents limit_px = round_to_tick(side == "buy" ? ref_px + mul(ref_px, offset) : ref_px - mul(ref_px, offset));

    // Shortable pre-flight: only matters when this order makes the short bigger (opening a
    // fresh short, or adding to an existing one) -- reducing a short or going long never
    // needs to borrow more. An unconfirmed symbol (no market.data.shortable.* seen yet, or
    // the broker reported it false) blocks rather than assumes shortable: this is a broker-
    // enforced fact, not a modeling choice, and the ETFs least likely to be verified quickly
    // are exactly the thinner currency ones (FXY, FXB) where the assumption is weakest.
    bool increasing_short = target_qty < 0 && target_qty < current_qty;
    if (increasing_short) {
        auto sit = shortable_.find(c.symbol);
        check("shortable", sit != shortable_.end() && sit->second, sit != shortable_.end() ? nlohmann::json(sit->second) : nlohmann::json(nullptr));
    }

    Cents current_notional = cur ? cur->market_value_cents() : 0;
    Cents target_notional = static_cast<Cents>(target_qty) * limit_px;
    Cents delta_notional = qty_abs * limit_px;

    Cents gross_excl_this = pf_.gross_exposure_cents - std::llabs(current_notional);
    Cents new_gross = gross_excl_this + std::llabs(target_notional);
    check("gross_exposure_cap_pct", pct_of(new_gross, pf_.equity_cents) <= limits_.gross_exposure_cap_pct + 1e-9, pct_of(new_gross, pf_.equity_cents), limits_.gross_exposure_cap_pct);

    Cents asset_class_excl_this = asset_class_exposure_cents(c.asset_class) - std::llabs(current_notional);
    Cents new_asset_class = asset_class_excl_this + std::llabs(target_notional);
    check("asset_class_cap_pct", pct_of(new_asset_class, pf_.equity_cents) <= limits_.asset_class_cap_pct + 1e-9, pct_of(new_asset_class, pf_.equity_cents), limits_.asset_class_cap_pct);

    // Buying power / margin: gross_exposure_cap_pct=100% (no leverage, per the
    // pre-registration) is the real binding constraint for this cash-account book. This is
    // a secondary sanity check against the broker's actual Reg-T marginable buying power
    // (shorting needs a margin account, unlike the earnings path's non-marginable figure)
    // -- same "0 means not populated" escape hatch evaluate_entry's own buying_power check
    // already uses.
    check("margin_buying_power", delta_notional <= pf_.margin_buying_power_cents || pf_.margin_buying_power_cents == 0, delta_notional, pf_.margin_buying_power_cents);

    if (!std::all_of(d.checks.begin(), d.checks.end(), [](const RiskCheck& r) { return r.ok; })) return d;

    nlohmann::json checks = nlohmann::json::array();
    for (const auto& r : d.checks) checks.push_back(r.to_json());
    nlohmann::json order = {
        {"client_order_id", client_order_id_for_entry(pc.msg_id)},
        {"candidate_msg_id", pc.msg_id},
        {"validated_msg_id", nullptr},   // TSMOM bypasses the Claude validator sidecar entirely
        {"intent", "REBALANCE_TO_WEIGHT"},
        {"symbol", c.symbol},
        {"side", side},
        {"qty", qty_abs},
        {"order_type", tp_->order.order_type},
        {"limit_px_cents", limit_px},
        {"stop_px_cents", nullptr},
        {"take_profit_px_cents", nullptr},
        {"linked_broker_order_id", nullptr},
        {"tif", tp_->order.tif},
        {"extended_hours", false},
        {"reason", "TSMOM-v1 monthly rebalance to target_weight_pct=" + std::to_string(c.target_weight_pct.value_or(0.0))},
        {"risk_checks", checks},
        {"equity_at_approval_cents", pf_.equity_cents},
        {"target_qty", target_qty},
        {"asset_class", c.asset_class},
    };
    d.order = std::move(order);
    d.approved = true;
    return d;
}

std::vector<EntryDecision> RiskManager::process_pending_entries(Date session, SysTime now) {
    start_session(session);
    std::vector<EntryDecision> out;
    // Best candidates first: higher EAR then higher momentum.
    std::stable_sort(pending_.begin(), pending_.end(), [](const PendingCandidate& a, const PendingCandidate& b) {
        if (a.cand.ear_pct != b.cand.ear_pct) return a.cand.ear_pct > b.cand.ear_pct;
        return a.cand.mom_pct > b.cand.mom_pct;
    });
    std::deque<PendingCandidate> keep;
    for (auto& pc : pending_) {
        bool is_tsmom = pc.cand.signal_type == "TSMOM_ETF_V1";
        // TSMOM bypasses the Claude validator sidecar entirely -- never held pending a
        // verdict that will never arrive.
        if (!is_tsmom && !limits_.shadow_mode && !pc.verdict) {
            if (pc.cand.entry_deadline_date > session) { keep.push_back(pc); continue; }
            ++counters_.validator_missing_streak;
            if (counters_.validator_missing_streak >= limits_.validator_error_streak_pause && !paused_new_) {
                paused_new_ = true;
                pause_reason_ = "validator silent";
                emit_control("pause_new", "VALIDATOR_ERROR_STREAK", "no validator verdict for " + std::to_string(counters_.validator_missing_streak) + " candidates in a row");
            }
        } else if (!is_tsmom && !limits_.shadow_mode) {
            counters_.validator_missing_streak = 0;
        }
        EntryDecision d = is_tsmom ? evaluate_rebalance(pc, session, now) : evaluate_entry(pc, session, now);
        if (d.approved) {
            ++counters_.new_positions_today;
            ++counters_.orders_today;
            approved_by_symbol_[pc.cand.symbol] = d.order["client_order_id"].get<std::string>();
        } else if (d.reject_reason == "quote_present" || d.reject_reason == "quote_age_s" || (is_tsmom && d.reject_reason == "ref_px_present")) {
            // Transient: try again on the next call today if still within deadline.
            if (pc.cand.entry_deadline_date >= session) keep.push_back(pc);
        }
        out.push_back(std::move(d));
    }
    pending_.swap(keep);
    return out;
}

nlohmann::json RiskManager::make_exit_order(const PositionState& pos, const std::string& intent, std::int64_t qty, const std::string& reason, Date session, std::optional<Cents> new_stop) {
    nlohmann::json order = {
        {"client_order_id", intent == "STOP_REPLACE" ? client_order_id_for_stop_replace(pos.symbol, iso_date(session), *new_stop)
                                                      : client_order_id_for_exit(pos.symbol, intent, iso_date(session))},
        {"candidate_msg_id", nullptr},
        {"validated_msg_id", nullptr},
        {"intent", intent},
        {"symbol", pos.symbol},
        {"side", "sell"},
        {"qty", qty},
        {"order_type", intent == "STOP_REPLACE" ? "limit" : "market"},
        {"limit_px_cents", nullptr},
        {"stop_px_cents", new_stop ? nlohmann::json(*new_stop) : (pos.stop_px_cents ? nlohmann::json(*pos.stop_px_cents) : nlohmann::json(nullptr))},
        {"take_profit_px_cents", nullptr},
        {"linked_broker_order_id", pos.stop_broker_order_id ? nlohmann::json(*pos.stop_broker_order_id) : nlohmann::json(nullptr)},
        {"tif", intent == "STOP_REPLACE" ? "gtc" : "day"},
        {"extended_hours", false},
        {"reason", reason},
        {"risk_checks", nlohmann::json::array({{{"name", "exit_rule"}, {"ok", true}, {"value", reason}, {"limit", nullptr}}})},
        {"equity_at_approval_cents", pf_.equity_cents},
        {"atr20_cents", pos.atr20_cents ? nlohmann::json(*pos.atr20_cents) : nlohmann::json(nullptr)},
    };
    if (intent == "STOP_REPLACE") order["order_type"] = "limit"; // schema enum; execution treats STOP_REPLACE by intent
    return order;
}

std::vector<nlohmann::json> RiskManager::end_of_session_sweep(Date session) {
    std::vector<nlohmann::json> out;
    if (!pf_.valid) return out;
    if (halted_) return out; // flatten/halt already handles everything
    for (const auto& pos : pf_.positions) {
        // TSMOM-owned positions have no stop/earnings-deadline/trend-scale exit machinery --
        // they only change on the next monthly formation (evaluate_rebalance), never here.
        if (!pos.asset_class.empty()) continue;
        auto actions = evaluate_exits(pos, session, pf_.spy_above_trend, sp_.exit, cal_);
        for (const auto& a : actions) {
            if (counters_.orders_today >= limits_.max_orders_per_day) {
                spdlog::warn("risk: max orders/day reached during sweep; deferring {} {}", exit_intent_str(a.intent), pos.symbol);
                emit_control("pause_new", "MANUAL", "max orders/day reached during exit sweep; exits deferred to next session", {{"informational", true}});
                return out;
            }
            out.push_back(make_exit_order(pos, exit_intent_str(a.intent), a.qty, a.reason, session, a.new_stop_cents));
            ++counters_.orders_today;
        }
    }
    return out;
}

// -------------------------------------------------------------- control -----

void RiskManager::emit_control(const std::string& command, const std::string& trigger, const std::string& reason, nlohmann::json details) {
    nlohmann::json p = {
        {"command", command}, {"reason", reason}, {"source", "risk"}, {"issued_at_utc", now_utc_iso()},
        {"trigger", trigger}, {"details", std::move(details)}, {"drill", false},
    };
    spdlog::warn("risk: control.{} trigger={} reason={}", command, trigger, reason);
    control_out_.emplace_back("control." + command, std::move(p));
}

std::vector<std::pair<std::string, nlohmann::json>> RiskManager::take_control_messages() {
    std::vector<std::pair<std::string, nlohmann::json>> out;
    out.swap(control_out_);
    return out;
}

// ---------------------------------------------------------------- state -----

nlohmann::json RiskManager::state_json() const {
    nlohmann::json pend = nlohmann::json::array();
    for (const auto& pc : pending_)
        pend.push_back({{"msg_id", pc.msg_id}, {"candidate", pc.cand.to_json()}, {"verdict", pc.verdict ? nlohmann::json(*pc.verdict) : nlohmann::json(nullptr)},
                        {"validated_msg_id", pc.validated_msg_id ? nlohmann::json(*pc.validated_msg_id) : nlohmann::json(nullptr)}, {"received_utc", pc.received_utc}});
    return {
        {"session", iso_date(counters_.session)},
        {"new_positions_today", counters_.new_positions_today},
        {"orders_today", counters_.orders_today},
        {"rejects_today", counters_.rejects_today},
        {"submitted_today", counters_.submitted_today},
        {"validator_missing_streak", counters_.validator_missing_streak},
        {"validator_error_streak", counters_.validator_error_streak},
        {"paused_new", paused_new_},
        {"pause_reason", pause_reason_},
        {"halted", halted_},
        {"size_halved", size_halved_},
        {"flatten_issued", flatten_issued_},
        {"daily_loss_paused_today", daily_loss_paused_today_},
        {"pending", pend},
        {"approved_by_symbol", approved_by_symbol_},
        {"early_verdicts", [&] {
            nlohmann::json ev = nlohmann::json::object();
            for (const auto& [k, v] : early_verdicts_) ev[k] = {{"verdict", v.first}, {"validated_msg_id", v.second}};
            return ev;
        }()},
    };
}

void RiskManager::load_state(const nlohmann::json& j) {
    if (auto d = parse_date(j.value("session", ""))) counters_.session = *d;
    counters_.new_positions_today = j.value("new_positions_today", 0);
    counters_.orders_today = j.value("orders_today", 0);
    counters_.rejects_today = j.value("rejects_today", 0);
    counters_.submitted_today = j.value("submitted_today", 0);
    counters_.validator_missing_streak = j.value("validator_missing_streak", 0);
    counters_.validator_error_streak = j.value("validator_error_streak", 0);
    paused_new_ = j.value("paused_new", false);
    pause_reason_ = j.value("pause_reason", "");
    halted_ = j.value("halted", false);
    size_halved_ = j.value("size_halved", false);
    flatten_issued_ = j.value("flatten_issued", false);
    daily_loss_paused_today_ = j.value("daily_loss_paused_today", false);
    pending_.clear();
    if (j.contains("pending"))
        for (const auto& pj : j["pending"]) {
            PendingCandidate pc;
            pc.msg_id = pj.value("msg_id", "");
            pc.cand = Candidate::from_json(pj.at("candidate"));
            if (pj.contains("verdict") && pj["verdict"].is_string()) pc.verdict = pj["verdict"].get<std::string>();
            if (pj.contains("validated_msg_id") && pj["validated_msg_id"].is_string()) pc.validated_msg_id = pj["validated_msg_id"].get<std::string>();
            pc.received_utc = pj.value("received_utc", "");
            pending_.push_back(std::move(pc));
        }
    approved_by_symbol_.clear();
    if (j.contains("approved_by_symbol"))
        for (auto& [k, v] : j["approved_by_symbol"].items()) approved_by_symbol_[k] = v.get<std::string>();
    early_verdicts_.clear();
    if (j.contains("early_verdicts") && j["early_verdicts"].is_object())
        for (auto& [k, v] : j["early_verdicts"].items()) early_verdicts_[k] = {v.value("verdict", "ERROR"), v.value("validated_msg_id", "")};
}

} // namespace at
