#include "strategy/ledger.hpp"

#include "strategy/exits.hpp"

#include "common/json_util.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace at {

nlohmann::json ClosedTrade::to_json() const {
    return {
        {"symbol", symbol}, {"candidate_msg_id", candidate_msg_id ? nlohmann::json(*candidate_msg_id) : nlohmann::json(nullptr)},
        {"event_id", event_id}, {"entry_date", iso_date(entry_date)}, {"entry_px_cents", entry_px_cents}, {"qty", qty},
        {"exit_date", iso_date(exit_date)}, {"exit_px_cents", exit_px_cents}, {"exit_reason", exit_reason},
        {"gross_pnl_cents", gross_pnl_cents}, {"costs_cents", costs_cents}, {"net_pnl_cents", net_pnl_cents},
        {"holding_sessions", holding_sessions}, {"ear_pct", ear_pct}, {"mom_pct", mom_pct}, {"verdict", verdict},
    };
}

std::string ClosedTrade::csv_header() {
    return "symbol,candidate_msg_id,event_id,entry_date,entry_px,qty,exit_date,exit_px,exit_reason,gross_pnl,costs,net_pnl,holding_sessions,ear_pct,mom_pct,verdict";
}

std::string ClosedTrade::to_csv() const {
    std::ostringstream ss;
    ss << symbol << ',' << candidate_msg_id.value_or("") << ',' << event_id << ',' << iso_date(entry_date) << ',' << cents_to_decimal(entry_px_cents) << ',' << qty << ','
       << iso_date(exit_date) << ',' << cents_to_decimal(exit_px_cents) << ',' << exit_reason << ',' << cents_to_decimal(gross_pnl_cents) << ','
       << cents_to_decimal(costs_cents) << ',' << cents_to_decimal(net_pnl_cents) << ',' << holding_sessions << ',' << ear_pct << ',' << mom_pct << ',' << verdict;
    return ss.str();
}

Ledger::Ledger(Cents initial_cash_cents)
    : initial_cash_(initial_cash_cents), cash_(initial_cash_cents), hwm_equity_(initial_cash_cents), day_start_equity_(initial_cash_cents) {}

PositionState* Ledger::find(const std::string& symbol) {
    for (auto& p : positions_) if (p.symbol == symbol) return &p;
    return nullptr;
}

std::optional<PositionState> Ledger::position(const std::string& symbol) const {
    for (const auto& p : positions_) if (p.symbol == symbol) return p;
    return std::nullopt;
}

void Ledger::apply_entry_fill(const std::string& symbol, std::int64_t qty, Cents px, Cents costs, Date session, const std::string& ts_utc, const EntryMeta& m) {
    cash_ -= qty * px + costs;
    total_costs_ += costs;
    if (PositionState* p = find(symbol)) {
        Cents total = p->qty * p->avg_px_cents + qty * px;
        p->qty += qty;
        p->avg_px_cents = total / p->qty;
        entry_costs_[symbol] += costs;
        return;
    }
    PositionState p;
    p.symbol = symbol;
    p.qty = qty;
    p.avg_px_cents = px;
    p.last_px_cents = px;
    p.entry_ts_utc = ts_utc;
    p.entry_session = session;
    p.sessions_held = 0;
    p.exit_deadline = m.exit_deadline;
    p.next_report_date = m.next_report_date;
    p.stop_px_cents = m.stop_px_cents;
    p.stop_broker_order_id = m.stop_broker_order_id;
    p.hwm_px_cents = px;
    p.atr20_cents = m.atr20_cents;
    p.sector = m.sector;
    p.candidate_msg_id = m.candidate_msg_id;
    positions_.push_back(p);
    entry_costs_[symbol] = costs;
    // Stash trade metadata in a shadow record keyed by symbol until exit.
    ClosedTrade t;
    t.symbol = symbol;
    t.candidate_msg_id = m.candidate_msg_id;
    t.event_id = m.event_id;
    t.entry_date = session;
    t.entry_px_cents = px;
    t.ear_pct = m.ear_pct;
    t.mom_pct = m.mom_pct;
    t.verdict = m.verdict;
    // store in a map via trades_ prefix: we keep open metadata separately
    open_meta_[symbol] = t;
}

void Ledger::open_rebalance_position(const std::string& symbol, std::int64_t qty, Cents px, Cents costs, Date session, const std::string& ts_utc, const EntryMeta& m) {
    PositionState p;
    p.symbol = symbol;
    p.qty = qty;
    p.avg_px_cents = px;
    p.last_px_cents = px;
    p.entry_ts_utc = ts_utc;
    p.entry_session = session;
    p.sessions_held = 0;
    p.hwm_px_cents = px;
    p.sector = m.sector;
    p.asset_class = m.asset_class;
    p.candidate_msg_id = m.candidate_msg_id;
    positions_.push_back(p);
    entry_costs_[symbol] = costs;
    ClosedTrade t;
    t.symbol = symbol;
    t.candidate_msg_id = m.candidate_msg_id;
    t.event_id = m.event_id;
    t.entry_date = session;
    t.entry_px_cents = px;
    t.ear_pct = m.ear_pct;
    t.mom_pct = m.mom_pct;
    t.verdict = m.verdict;
    open_meta_[symbol] = t;
}

void Ledger::apply_rebalance_fill(const std::string& symbol, std::int64_t target_qty, Cents px, Cents costs, Date session, const std::string& ts_utc, const EntryMeta& m) {
    PositionState* p = find(symbol);
    std::int64_t current_qty = p ? p->qty : 0;
    std::int64_t delta = target_qty - current_qty;
    if (delta == 0) return;

    // Cash moves opposite the signed share delta regardless of direction: buying-to-open,
    // selling-to-open (short), buying-to-cover and selling-to-reduce are all the same formula.
    cash_ -= delta * px + costs;
    total_costs_ += costs;

    bool crosses_zero = current_qty != 0 && target_qty != 0 && (current_qty > 0) != (target_qty > 0);

    if (current_qty == 0) {
        open_rebalance_position(symbol, target_qty, px, costs, session, ts_utc, m);
        return;
    }

    if (crosses_zero) {
        // Full close of the old side (realizes all its P&L), then a fresh open of the new
        // side at px -- there is no gain/loss on the "opening" half by construction, since
        // its avg_px is px itself.
        Cents entry_share = entry_costs_.count(symbol) ? entry_costs_[symbol] : 0;
        entry_costs_.erase(symbol);
        ClosedTrade t = open_meta_.count(symbol) ? open_meta_[symbol] : ClosedTrade{};
        t.symbol = symbol;
        t.qty = current_qty;
        t.entry_px_cents = p->avg_px_cents;
        if (t.entry_date == Date{}) t.entry_date = p->entry_session;
        t.exit_date = session;
        t.exit_px_cents = px;
        t.exit_reason = "rebalance_flip";
        t.gross_pnl_cents = current_qty * (px - p->avg_px_cents);
        t.costs_cents = entry_share;   // the fill's own cost is charged to the new leg below
        t.net_pnl_cents = t.gross_pnl_cents - t.costs_cents;
        t.holding_sessions = p->sessions_held;
        trades_.push_back(t);
        if (t.net_pnl_cents < 0) ++consecutive_losers_; else consecutive_losers_ = 0;
        open_meta_.erase(symbol);
        positions_.erase(std::remove_if(positions_.begin(), positions_.end(), [&](const PositionState& x) { return x.symbol == symbol; }), positions_.end());
        open_rebalance_position(symbol, target_qty, px, costs, session, ts_utc, m);
        return;
    }

    if (target_qty == 0) {
        Cents entry_share = entry_costs_.count(symbol) ? entry_costs_[symbol] : 0;
        ClosedTrade t = open_meta_.count(symbol) ? open_meta_[symbol] : ClosedTrade{};
        t.symbol = symbol;
        t.qty = current_qty;
        t.entry_px_cents = p->avg_px_cents;
        if (t.entry_date == Date{}) t.entry_date = p->entry_session;
        t.exit_date = session;
        t.exit_px_cents = px;
        t.exit_reason = "rebalance_close";
        t.gross_pnl_cents = current_qty * (px - p->avg_px_cents);
        t.costs_cents = costs + entry_share;
        t.net_pnl_cents = t.gross_pnl_cents - t.costs_cents;
        t.holding_sessions = p->sessions_held;
        trades_.push_back(t);
        if (t.net_pnl_cents < 0) ++consecutive_losers_; else consecutive_losers_ = 0;
        positions_.erase(std::remove_if(positions_.begin(), positions_.end(), [&](const PositionState& x) { return x.symbol == symbol; }), positions_.end());
        entry_costs_.erase(symbol);
        open_meta_.erase(symbol);
        return;
    }

    // Same-direction resize (no crossing): increasing adds at px with a weighted-average
    // cost basis, like apply_entry_fill; decreasing realizes P&L on the reduced portion,
    // like apply_exit_fill, leaving avg_px_cents unchanged.
    if (std::llabs(target_qty) > std::llabs(current_qty)) {
        Cents total_notional = current_qty * p->avg_px_cents + delta * px;
        p->qty = target_qty;
        p->avg_px_cents = total_notional / target_qty;
        entry_costs_[symbol] += costs;
    } else {
        std::int64_t reduced_qty = current_qty - target_qty;   // same sign as current_qty
        Cents entry_share = entry_costs_.count(symbol) ? entry_costs_[symbol] * std::llabs(reduced_qty) / std::llabs(current_qty) : 0;
        if (entry_costs_.count(symbol)) entry_costs_[symbol] -= entry_share;
        ClosedTrade t = open_meta_.count(symbol) ? open_meta_[symbol] : ClosedTrade{};
        t.symbol = symbol;
        t.qty = reduced_qty;
        t.entry_px_cents = p->avg_px_cents;
        if (t.entry_date == Date{}) t.entry_date = p->entry_session;
        t.exit_date = session;
        t.exit_px_cents = px;
        t.exit_reason = "rebalance_reduce";
        t.gross_pnl_cents = reduced_qty * (px - p->avg_px_cents);
        t.costs_cents = costs + entry_share;
        t.net_pnl_cents = t.gross_pnl_cents - t.costs_cents;
        t.holding_sessions = p->sessions_held;
        trades_.push_back(t);
        if (t.net_pnl_cents < 0) ++consecutive_losers_; else consecutive_losers_ = 0;
        p->qty = target_qty;
    }
}

void Ledger::apply_exit_fill(const std::string& symbol, std::int64_t qty, Cents px, Cents costs, Date session, const std::string& reason) {
    PositionState* p = find(symbol);
    if (!p || qty <= 0) return;
    if (qty > p->qty) qty = p->qty;
    cash_ += qty * px - costs;
    total_costs_ += costs;
    // Proportional share of entry costs.
    Cents entry_share = entry_costs_[symbol] * qty / p->qty;
    entry_costs_[symbol] -= entry_share;
    ClosedTrade t = open_meta_.count(symbol) ? open_meta_[symbol] : ClosedTrade{};
    t.symbol = symbol;
    t.qty = qty;
    t.entry_px_cents = p->avg_px_cents;
    if (t.entry_date == Date{}) t.entry_date = p->entry_session;
    t.exit_date = session;
    t.exit_px_cents = px;
    t.exit_reason = reason;
    t.gross_pnl_cents = qty * (px - p->avg_px_cents);
    t.costs_cents = costs + entry_share;
    t.net_pnl_cents = t.gross_pnl_cents - t.costs_cents;
    t.holding_sessions = p->sessions_held;
    trades_.push_back(t);
    if (t.net_pnl_cents < 0) ++consecutive_losers_; else consecutive_losers_ = 0;
    p->qty -= qty;
    if (p->qty == 0) {
        positions_.erase(std::remove_if(positions_.begin(), positions_.end(), [&](const PositionState& x) { return x.symbol == symbol; }), positions_.end());
        entry_costs_.erase(symbol);
        open_meta_.erase(symbol);
    }
}

void Ledger::mark(const std::string& symbol, Cents last_px_cents) {
    if (PositionState* p = find(symbol)) p->last_px_cents = last_px_cents;
}

void Ledger::mark_all_from(const MarketStore& mkt, Date session) {
    for (auto& p : positions_) {
        if (auto b = mkt.last_bar_on_or_before(p.symbol, session)) p.last_px_cents = b->close;
    }
}

void Ledger::start_session(Date session) {
    if (day_start_session_ == session) return;
    day_start_session_ = session;
    day_start_equity_ = equity();
}

void Ledger::end_of_session(Date session) {
    for (auto& p : positions_) {
        if (p.entry_session < session) ++p.sessions_held;
        if (!p.hwm_px_cents || p.last_px_cents > *p.hwm_px_cents) p.hwm_px_cents = p.last_px_cents;
    }
    Cents eq = equity();
    DailyRecord d;
    d.date = session;
    d.equity_cents = eq;
    d.cash_cents = cash_;
    d.gross_exposure_cents = gross_exposure();
    d.n_positions = static_cast<int>(positions_.size());
    Cents prev = daily_.empty() ? initial_cash_ : daily_.back().equity_cents;
    d.ret = prev > 0 ? static_cast<double>(eq - prev) / static_cast<double>(prev) : 0.0;
    if (eq > hwm_equity_) hwm_equity_ = eq;
    d.drawdown_pct = drawdown_pct();
    daily_.push_back(d);
}

void Ledger::set_stop(const std::string& symbol, Cents stop_px_cents, std::optional<std::string> broker_order_id) {
    if (PositionState* p = find(symbol)) {
        p->stop_px_cents = stop_px_cents;
        if (broker_order_id) p->stop_broker_order_id = broker_order_id;
    }
}

void Ledger::mark_scaled_down(const std::string& symbol) {
    if (PositionState* p = find(symbol)) p->scaled_down = true;
}

void Ledger::update_next_report_date(const std::string& symbol, Date next_report, const TradingCalendar& cal, int drift_window, int exit_sessions_before) {
    PositionState* p = find(symbol);
    if (!p) return;
    if (p->next_report_date && *p->next_report_date <= next_report) return;
    p->next_report_date = next_report;
    ExitParams ep;
    ep.drift_window_sessions = drift_window;
    ep.exit_sessions_before_earnings = exit_sessions_before;
    p->exit_deadline = compute_exit_deadline(p->entry_session, next_report, ep, cal);
}

void Ledger::sync_from_broker(const std::vector<PositionState>& broker, Date session, const std::string& ts_utc) {
    std::vector<PositionState> merged;
    for (const auto& b : broker) {
        if (PositionState* p = find(b.symbol)) {
            p->qty = b.qty;
            p->avg_px_cents = b.avg_px_cents;
            if (b.last_px_cents > 0) p->last_px_cents = b.last_px_cents;
            merged.push_back(*p);
        } else {
            PositionState np = b;
            np.entry_ts_utc = ts_utc;
            np.entry_session = session;
            np.exit_deadline = session; // unknown provenance: exit at next sweep unless the reconciler adopts it
            merged.push_back(np);
        }
    }
    positions_ = std::move(merged);
}

Cents Ledger::equity() const {
    Cents e = cash_;
    for (const auto& p : positions_) e += p.market_value_cents();
    return e;
}

Cents Ledger::gross_exposure() const {
    Cents g = 0;
    for (const auto& p : positions_) g += std::llabs(p.market_value_cents());
    return g;
}

double Ledger::drawdown_pct() const {
    if (hwm_equity_ <= 0) return 0.0;
    double dd = (static_cast<double>(equity()) / static_cast<double>(hwm_equity_) - 1.0) * 100.0;
    return std::min(dd, 0.0);
}

double Ledger::daily_pnl_pct() const {
    if (day_start_equity_ <= 0) return 0.0;
    return (static_cast<double>(equity()) / static_cast<double>(day_start_equity_) - 1.0) * 100.0;
}

std::map<std::string, double> Ledger::sector_exposure_pct() const {
    std::map<std::string, double> out;
    Cents eq = equity();
    if (eq <= 0) return out;
    for (const auto& p : positions_) out[p.sector.empty() ? "UNKNOWN" : p.sector] += pct_of(p.market_value_cents(), eq);
    return out;
}

std::map<std::string, double> Ledger::asset_class_exposure_pct() const {
    std::map<std::string, double> out;
    Cents eq = equity();
    if (eq <= 0) return out;
    // Gross (not net) per bucket -- consistent with how gross_exposure() already treats
    // shorts, and the natural reading of an "exposure cap" for a book that can be short.
    for (const auto& p : positions_) if (!p.asset_class.empty()) out[p.asset_class] += pct_of(std::llabs(p.market_value_cents()), eq);
    return out;
}

std::optional<double> Ledger::realized_vol_annual(int lookback) const {
    if (static_cast<int>(daily_.size()) < lookback + 1) return std::nullopt;
    std::vector<double> r;
    for (std::size_t i = daily_.size() - static_cast<std::size_t>(lookback); i < daily_.size(); ++i) r.push_back(daily_[i].ret);
    return stdev(r) * std::sqrt(252.0);
}

nlohmann::json Ledger::portfolio_state_payload(const std::string& as_of_utc, Date session, bool reconciled, const nlohmann::json& open_orders,
                                               int new_positions_today, int orders_today, int rejects_today, std::optional<bool> spy_above_trend) const {
    nlohmann::json pos = nlohmann::json::array();
    for (const auto& p : positions_) pos.push_back(p.to_json());
    Cents eq = equity();
    auto rv = realized_vol_annual();
    return {
        {"as_of_utc", as_of_utc},
        {"session_date", iso_date(session)},
        {"reconciled", reconciled},
        {"equity_cents", eq},
        {"cash_cents", cash_},
        {"buying_power_cents", std::max<Cents>(cash_, 0)},
        {"margin_buying_power_cents", margin_buying_power_},
        {"positions", pos},
        {"gross_exposure_cents", gross_exposure()},
        {"gross_exposure_pct", pct_of(gross_exposure(), eq)},
        {"sector_exposure_pct", sector_exposure_pct()},
        {"asset_class_exposure_pct", asset_class_exposure_pct()},
        {"hwm_equity_cents", std::max(hwm_equity_, eq)},
        {"drawdown_pct", drawdown_pct()},
        {"day_start_equity_cents", day_start_equity_},
        {"daily_pnl_pct", daily_pnl_pct()},
        {"open_orders", open_orders.is_array() ? open_orders : nlohmann::json::array()},
        {"counters", {{"new_positions_today", new_positions_today}, {"orders_today", orders_today}, {"consecutive_losers", consecutive_losers_},
                      {"closed_trades_total", static_cast<int>(trades_.size())}, {"rejects_today", rejects_today}}},
        {"spy_above_trend", spy_above_trend ? nlohmann::json(*spy_above_trend) : nlohmann::json(nullptr)},
        {"realized_vol_annual_pct", rv ? nlohmann::json(*rv * 100.0) : nlohmann::json(nullptr)},
    };
}

nlohmann::json Ledger::state_json() const {
    nlohmann::json pos = nlohmann::json::array();
    for (const auto& p : positions_) pos.push_back(p.to_json());
    nlohmann::json tr = nlohmann::json::array();
    for (const auto& t : trades_) tr.push_back(t.to_json());
    nlohmann::json om = nlohmann::json::object();
    for (const auto& [k, v] : open_meta_) om[k] = v.to_json();
    nlohmann::json ec = nlohmann::json::object();
    for (const auto& [k, v] : entry_costs_) ec[k] = v;
    nlohmann::json dl = nlohmann::json::array();
    for (const auto& d : daily_) dl.push_back({{"date", iso_date(d.date)}, {"equity_cents", d.equity_cents}, {"cash_cents", d.cash_cents}, {"gross_exposure_cents", d.gross_exposure_cents}, {"n_positions", d.n_positions}, {"ret", d.ret}, {"drawdown_pct", d.drawdown_pct}});
    return {
        {"initial_cash_cents", initial_cash_}, {"cash_cents", cash_}, {"positions", pos}, {"trades", tr}, {"open_meta", om}, {"entry_costs", ec},
        {"daily", dl}, {"hwm_equity_cents", hwm_equity_}, {"day_start_equity_cents", day_start_equity_}, {"day_start_session", iso_date(day_start_session_)},
        {"total_costs_cents", total_costs_}, {"consecutive_losers", consecutive_losers_},
        {"margin_buying_power_cents", margin_buying_power_},
    };
}

void Ledger::load_state(const nlohmann::json& j) {
    initial_cash_ = j.value("initial_cash_cents", initial_cash_);
    cash_ = j.value("cash_cents", cash_);
    positions_.clear();
    for (const auto& p : j.value("positions", nlohmann::json::array())) positions_.push_back(PositionState::from_json(p));
    trades_.clear();
    for (const auto& t : j.value("trades", nlohmann::json::array())) {
        ClosedTrade c;
        c.symbol = t.value("symbol", "");
        if (t.contains("candidate_msg_id") && t["candidate_msg_id"].is_string()) c.candidate_msg_id = t["candidate_msg_id"].get<std::string>();
        c.event_id = t.value("event_id", "");
        if (auto d = parse_date(t.value("entry_date", ""))) c.entry_date = *d;
        c.entry_px_cents = t.value("entry_px_cents", 0LL);
        c.qty = t.value("qty", 0LL);
        if (auto d = parse_date(t.value("exit_date", ""))) c.exit_date = *d;
        c.exit_px_cents = t.value("exit_px_cents", 0LL);
        c.exit_reason = t.value("exit_reason", "");
        c.gross_pnl_cents = t.value("gross_pnl_cents", 0LL);
        c.costs_cents = t.value("costs_cents", 0LL);
        c.net_pnl_cents = t.value("net_pnl_cents", 0LL);
        c.holding_sessions = t.value("holding_sessions", 0);
        c.ear_pct = t.value("ear_pct", 0.0);
        c.mom_pct = t.value("mom_pct", 0.0);
        c.verdict = t.value("verdict", "");
        trades_.push_back(c);
    }
    open_meta_.clear();
    for (auto& [k, v] : json_obj(j, "open_meta").items()) {
        ClosedTrade c;
        c.symbol = k;
        if (v.contains("candidate_msg_id") && v["candidate_msg_id"].is_string()) c.candidate_msg_id = v["candidate_msg_id"].get<std::string>();
        c.event_id = v.value("event_id", "");
        if (auto d = parse_date(v.value("entry_date", ""))) c.entry_date = *d;
        c.entry_px_cents = v.value("entry_px_cents", 0LL);
        c.ear_pct = v.value("ear_pct", 0.0);
        c.mom_pct = v.value("mom_pct", 0.0);
        c.verdict = v.value("verdict", "");
        open_meta_[k] = c;
    }
    entry_costs_.clear();
    for (auto& [k, v] : json_obj(j, "entry_costs").items()) entry_costs_[k] = v.get<Cents>();
    daily_.clear();
    for (const auto& d : j.value("daily", nlohmann::json::array())) {
        DailyRecord r;
        if (auto dd = parse_date(d.value("date", ""))) r.date = *dd;
        r.equity_cents = d.value("equity_cents", 0LL);
        r.cash_cents = d.value("cash_cents", 0LL);
        r.gross_exposure_cents = d.value("gross_exposure_cents", 0LL);
        r.n_positions = d.value("n_positions", 0);
        r.ret = d.value("ret", 0.0);
        r.drawdown_pct = d.value("drawdown_pct", 0.0);
        daily_.push_back(r);
    }
    hwm_equity_ = j.value("hwm_equity_cents", hwm_equity_);
    day_start_equity_ = j.value("day_start_equity_cents", day_start_equity_);
    if (auto d = parse_date(j.value("day_start_session", ""))) day_start_session_ = *d;
    total_costs_ = j.value("total_costs_cents", 0LL);
    consecutive_losers_ = j.value("consecutive_losers", 0);
    margin_buying_power_ = j.value("margin_buying_power_cents", 0LL);
}

} // namespace at
