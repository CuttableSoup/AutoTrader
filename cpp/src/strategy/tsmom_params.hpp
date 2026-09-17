// Frozen TSMOM-v1 parameters (config/strategy.v3.json, docs/prereg/TSMOM-v1.md).
// Zero tunable parameters -- this is a literature-cited spec, not a search;
// changing anything here bumps strategy_version and needs a new pre-registration,
// not a walk-forward grid. Deliberately a parallel struct to StrategyParams, not
// nested inside it: TSMOM has no signal.tunable.*, no exit.*, and a different
// sizing model, so folding it into StrategyParams would mean every
// earnings-only service carries dead TSMOM config and would conflate two
// unrelated strategies' fingerprint()s.
#pragma once
#include "common/config.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace at {

struct TsmomSignalParams {
    int momentum_lookback_sessions = 252;   // 12 months of daily sessions
    int vol_lookback_sessions = 60;         // trailing window for annualized vol
};

struct TsmomSizingParams {
    double gross_cap_pct = 100.0;               // sum(|w_i|) <= this; DOWN-scale only, never levers up
    double asset_class_cap_pct_default = 40.0;  // starting point for config/risk.v1.json's asset_class_cap_pct
};

struct TsmomOrderParams {
    std::string order_type = "limit";
    std::string tif = "day";
    double rebalance_limit_offset_bps = 20.0;
};

struct TsmomParams {
    std::string strategy_version = "TSMOM_ETF_V1.0";
    TsmomSignalParams signal;
    TsmomSizingParams sizing;
    TsmomOrderParams order;

    static TsmomParams from_json(const nlohmann::json& j);
    static TsmomParams load(const std::filesystem::path& file);
    nlohmann::json to_json() const;
    // Identifier of this exact parameter set (for reports and candidate metadata).
    std::string fingerprint() const;
};

} // namespace at
