#include "strategy/types.hpp"

#include <algorithm>
#include <cctype>

namespace at {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::optional<Date> opt_date(const nlohmann::json& j, const char* k) {
    if (!j.contains(k) || j[k].is_null()) return std::nullopt;
    return parse_date(j[k].get<std::string>());
}
template <typename T>
std::optional<T> opt(const nlohmann::json& j, const char* k) {
    if (!j.contains(k) || j[k].is_null()) return std::nullopt;
    return j[k].get<T>();
}
nlohmann::json j_opt_date(const std::optional<Date>& d) { return d ? nlohmann::json(iso_date(*d)) : nlohmann::json(nullptr); }
template <typename T>
nlohmann::json j_opt(const std::optional<T>& v) { return v ? nlohmann::json(*v) : nlohmann::json(nullptr); }
} // namespace

bool SecurityInfo::is_etf() const { return lower(category).find("etf") != std::string::npos || lower(category).find("etn") != std::string::npos; }
bool SecurityInfo::is_adr() const { return lower(category).find("adr") != std::string::npos; }
bool SecurityInfo::is_common_stock() const {
    auto c = lower(category);
    return c.find("common stock") != std::string::npos && !is_adr();
}

std::string UniverseSnapshot::sector_of(const std::string& s) const {
    auto it = info.find(s);
    return it == info.end() ? "" : it->second.sector;
}

Date EarningsEvent::day0(const TradingCalendar& cal) const {
    if (timing == Timing::BMO && cal.is_trading_day(report_date)) return report_date;
    return cal.next_trading_day(report_date);
}

EarningsEvent::Timing EarningsEvent::parse_timing(std::string_view s) {
    if (s == "BMO") return Timing::BMO;
    if (s == "AMC") return Timing::AMC;
    return Timing::UNKNOWN;
}

const char* EarningsEvent::timing_str(Timing t) {
    switch (t) { case Timing::BMO: return "BMO"; case Timing::AMC: return "AMC"; default: return "UNKNOWN"; }
}

EarningsEvent EarningsEvent::from_json(const nlohmann::json& p) {
    EarningsEvent e;
    e.event_id = p.at("event_id").get<std::string>();
    e.symbol = p.at("symbol").get<std::string>();
    e.report_date = parse_date_or_throw(p.at("report_date").get<std::string>());
    e.timing = parse_timing(p.value("timing", "UNKNOWN"));
    e.fiscal_period = p.value("fiscal_period", "");
    e.eps_actual = opt<double>(p, "eps_actual");
    e.eps_consensus = opt<double>(p, "eps_consensus");
    e.eps_consensus_asof = opt_date(p, "eps_consensus_asof");
    e.revenue_actual_cents = opt<Cents>(p, "revenue_actual_cents");
    e.revenue_consensus_cents = opt<Cents>(p, "revenue_consensus_cents");
    e.next_report_date = opt_date(p, "next_report_date");
    if (p.contains("material_8k_dates"))
        for (const auto& d : p["material_8k_dates"])
            if (auto dd = parse_date(d.get<std::string>())) e.material_8k_dates.push_back(*dd);
    e.source = p.value("source", "");
    return e;
}

nlohmann::json EarningsEvent::to_json() const {
    nlohmann::json j = {
        {"event_id", event_id},
        {"symbol", symbol},
        {"report_date", iso_date(report_date)},
        {"timing", timing_str(timing)},
        {"timing_sources", nlohmann::json::array({{{"vendor", source.empty() ? "synthetic" : source}, {"timing", timing_str(timing)}}})},
        {"fiscal_period", fiscal_period},
        {"eps_actual", j_opt(eps_actual)},
        {"eps_consensus", j_opt(eps_consensus)},
        {"eps_consensus_asof", j_opt_date(eps_consensus_asof)},
        {"revenue_actual_cents", j_opt(revenue_actual_cents)},
        {"revenue_consensus_cents", j_opt(revenue_consensus_cents)},
        {"next_report_date", j_opt_date(next_report_date)},
        {"material_8k_dates", nlohmann::json::array()},
        {"source", source.empty() ? "synthetic" : source},
        {"as_of_utc", now_utc_iso()},
    };
    for (auto d : material_8k_dates) j["material_8k_dates"].push_back(iso_date(d));
    return j;
}

nlohmann::json Candidate::to_json() const {
    nlohmann::json j = {
        {"symbol", symbol},
        {"side", side},
        {"signal_type", signal_type},
        {"strategy_version", strategy_version},
        {"event_id", event_id},
        {"session_date", iso_date(session_date)},
        {"ear_pct", ear_pct},
        {"vol_ratio", vol_ratio},
        {"mom_pct", mom_pct},
        {"rsi5", j_opt(rsi5)},
        {"revision_breadth", j_opt(revision_breadth)},
        {"entry_px_ref_cents", entry_px_ref_cents},
        {"atr20_cents", atr20_cents},
        {"adv20_shares", adv20_shares},
        {"sector", sector},
        {"spy_above_trend", spy_above_trend},
        {"entry_deadline_date", iso_date(entry_deadline_date)},
        {"next_report_date", j_opt_date(next_report_date)},
        {"universe_snapshot_id", universe_snapshot_id},
        {"thesis_facts", thesis_facts},
        {"data_as_of_utc", data_as_of_utc},
    };
    return j;
}

Candidate Candidate::from_json(const nlohmann::json& p) {
    Candidate c;
    c.symbol = p.at("symbol").get<std::string>();
    c.side = p.value("side", "BUY");
    c.signal_type = p.value("signal_type", "EARNINGS_MOMENTUM_V1");
    c.strategy_version = p.value("strategy_version", "");
    c.event_id = p.value("event_id", "");
    c.session_date = parse_date_or_throw(p.at("session_date").get<std::string>());
    c.ear_pct = p.at("ear_pct").get<double>();
    c.vol_ratio = p.at("vol_ratio").get<double>();
    c.mom_pct = p.at("mom_pct").get<double>();
    c.rsi5 = opt<double>(p, "rsi5");
    c.revision_breadth = opt<double>(p, "revision_breadth");
    c.entry_px_ref_cents = p.at("entry_px_ref_cents").get<Cents>();
    c.atr20_cents = p.at("atr20_cents").get<Cents>();
    c.adv20_shares = p.value("adv20_shares", 0LL);
    c.sector = p.value("sector", "");
    c.spy_above_trend = p.value("spy_above_trend", false);
    if (auto d = opt_date(p, "entry_deadline_date")) c.entry_deadline_date = *d;
    c.next_report_date = opt_date(p, "next_report_date");
    c.universe_snapshot_id = p.value("universe_snapshot_id", "");
    if (p.contains("thesis_facts")) c.thesis_facts = p["thesis_facts"].get<std::vector<std::string>>();
    c.data_as_of_utc = p.value("data_as_of_utc", "");
    return c;
}

nlohmann::json PositionState::to_json() const {
    return {
        {"symbol", symbol},
        {"qty", qty},
        {"avg_px_cents", avg_px_cents},
        {"last_px_cents", last_px_cents},
        {"market_value_cents", market_value_cents()},
        {"unrealized_cents", unrealized_cents()},
        {"entry_ts_utc", entry_ts_utc},
        {"entry_session_date", iso_date(entry_session)},
        {"sessions_held", sessions_held},
        {"exit_deadline_date", iso_date(exit_deadline)},
        {"next_report_date", j_opt_date(next_report_date)},
        {"stop_px_cents", j_opt(stop_px_cents)},
        {"stop_broker_order_id", j_opt(stop_broker_order_id)},
        {"hwm_px_cents", j_opt(hwm_px_cents)},
        {"atr20_cents", j_opt(atr20_cents)},
        {"sector", sector.empty() ? nlohmann::json(nullptr) : nlohmann::json(sector)},
        {"candidate_msg_id", j_opt(candidate_msg_id)},
        {"scaled_down", scaled_down},
    };
}

PositionState PositionState::from_json(const nlohmann::json& j) {
    PositionState p;
    p.symbol = j.at("symbol").get<std::string>();
    p.qty = j.at("qty").get<std::int64_t>();
    p.avg_px_cents = j.at("avg_px_cents").get<Cents>();
    p.last_px_cents = j.value("last_px_cents", p.avg_px_cents);
    p.entry_ts_utc = j.value("entry_ts_utc", "");
    if (auto d = opt_date(j, "entry_session_date")) p.entry_session = *d;
    p.sessions_held = j.value("sessions_held", 0);
    if (auto d = opt_date(j, "exit_deadline_date")) p.exit_deadline = *d;
    p.next_report_date = opt_date(j, "next_report_date");
    p.stop_px_cents = opt<Cents>(j, "stop_px_cents");
    p.stop_broker_order_id = opt<std::string>(j, "stop_broker_order_id");
    p.hwm_px_cents = opt<Cents>(j, "hwm_px_cents");
    p.atr20_cents = opt<Cents>(j, "atr20_cents");
    if (j.contains("sector") && j["sector"].is_string()) p.sector = j["sector"].get<std::string>();
    p.candidate_msg_id = opt<std::string>(j, "candidate_msg_id");
    p.scaled_down = j.value("scaled_down", false);
    return p;
}

} // namespace at
