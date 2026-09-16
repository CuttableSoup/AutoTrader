// Event-driven daily backtester. Drives the same StrategyEngine, RiskManager
// and Ledger the live services use; only the fill model and the data source
// differ (docs/DESIGN.md 7.1).
#pragma once
#include "backtester/metrics.hpp"
#include "backtester/mock_validator.hpp"
#include "risk/limits.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/engine.hpp"
#include "strategy/ledger.hpp"
#include "strategy/params.hpp"
#include "strategy/universe.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace at {

struct CostModel {
    double commission_bps_per_side = 5.0;
    Cents sec_fee_per_million_dollars_sold_cents = 2780;   // $27.80 per $1M sold
    double finra_taf_per_share_cents = 0.0166;
    Cents finra_taf_max_per_trade_cents = 833;
    bool half_spread_slippage = true;
    double default_spread_bps = 4.0;

    Cents buy_costs(std::int64_t qty, Cents px) const;
    Cents sell_costs(std::int64_t qty, Cents px) const;
    Cents buy_fill_px(Cents ref) const;    // ref + half spread
    Cents sell_fill_px(Cents ref) const;   // ref - half spread
    static CostModel from_json(const nlohmann::json& j);
    nlohmann::json to_json() const;
};

struct WalkForwardConfig {
    int folds = 3;
    int train_years = 2;
    int test_years = 1;
    int purge_sessions = 40;
    int embargo_sessions = 10;
    std::string objective = "sharpe_net";
};

struct ReportConfig {
    double haircut_pct = 50.0;
    double risk_free_annual_pct = 0.0;
    double min_gross_sharpe_after_haircut = 0.4;
};

struct BacktestConfig {
    std::string run_name;
    std::filesystem::path data_dir;
    std::filesystem::path out_dir;
    std::filesystem::path strategy_config;
    std::filesystem::path risk_config;
    Date start{};
    Date end{};
    Cents initial_equity_cents = 10000000;
    CostModel costs;
    std::string validator_mode = "mock";     // mock (enforced) | shadow (recorded) | none
    MockValidatorConfig mock;
    WalkForwardConfig wf;
    ReportConfig report;
    static BacktestConfig load(const std::filesystem::path& file);
};

struct BacktestData {
    MarketStore mkt;
    std::vector<SecurityInfo> securities;
    std::vector<EarningsEvent> events;
    static BacktestData load(const std::filesystem::path& data_dir);
    // earnings.csv: symbol,report_date,timing,fiscal_period,eps_actual,eps_consensus,eps_consensus_asof,revenue_actual,revenue_consensus,next_report_date,material_8k_dates
    static std::vector<EarningsEvent> load_earnings_csv(const std::filesystem::path& file);
};

struct CandidateOutcome {
    std::string symbol;
    std::string event_id;
    Date signal_date{};
    double ear_pct = 0;
    double mom_pct = 0;
    std::string verdict;                 // validator verdict
    bool entered = false;                // approved by risk and filled
    std::string risk_reject_reason;
    std::optional<double> fwd_return_pct;  // open(entry session) -> close(entry + drift window), for the counterfactual
    nlohmann::json to_json() const;
};

struct BacktestResult {
    StrategyParams params;
    Date start{};
    Date end{};
    std::vector<DailyRecord> daily;
    std::vector<double> daily_net;
    std::vector<double> daily_gross;
    std::vector<ClosedTrade> trades;
    PerformanceMetrics metrics;
    int sessions = 0;
    int candidates = 0;
    int vetoed = 0;
    int approved = 0;
    int risk_rejected = 0;
    std::map<std::string, int> reject_reasons;
    std::vector<CandidateOutcome> outcomes;
    std::optional<Date> halted_on;
    std::vector<std::string> control_log;
    nlohmann::json to_json(bool include_series = false) const;
};

class Backtester {
public:
    Backtester(const BacktestData& data, const BacktestConfig& cfg, RiskLimits limits);
    BacktestResult run(const StrategyParams& params, Date start, Date end, bool verbose = false);
    const BacktestConfig& config() const { return cfg_; }

private:
    const BacktestData& data_;
    BacktestConfig cfg_;
    RiskLimits limits_;
    const TradingCalendar& cal_;
};

} // namespace at
