// at_backtester --config config/backtest.synthetic.json [--single] [--verbose]
//   default: walk-forward per config (tunes only EAR threshold + momentum percentile), DSR, G1 verdict
//   --single: one pass with the frozen parameters over [start_date, end_date]
#include "backtester/backtester.hpp"
#include "backtester/walk_forward.hpp"
#include "common/logging.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>
#include <iostream>

using namespace at;

int main(int argc, char** argv) {
    std::string config_path = "config/backtest.synthetic.json";
    bool single = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!std::strcmp(argv[i], "--single")) single = true;
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
        else if (!std::strcmp(argv[i], "--help")) { std::cout << "usage: at_backtester --config <file> [--single] [--verbose]\n"; return 0; }
    }
    init_logging("backtester", "", verbose ? "debug" : "info");
    try {
        BacktestConfig cfg = BacktestConfig::load(config_path);
        StrategyParams params = StrategyParams::load(cfg.strategy_config);
        RiskLimits limits = RiskLimits::load(cfg.risk_config);
        BacktestData data = BacktestData::load(cfg.data_dir);
        std::filesystem::create_directories(cfg.out_dir);
        Backtester bt(data, cfg, limits);

        if (single) {
            BacktestResult r = bt.run(params, cfg.start, cfg.end, verbose);
            std::ofstream(cfg.out_dir / "single.json") << r.to_json(true).dump(2);
            std::ofstream tr(cfg.out_dir / "trades.csv");
            tr << ClosedTrade::csv_header() << "\n";
            for (const auto& t : r.trades) tr << t.to_csv() << "\n";
            std::ofstream dl(cfg.out_dir / "daily.csv");
            dl << "date,equity,cash,gross_exposure,n_positions,ret,drawdown_pct\n";
            for (const auto& d : r.daily) dl << iso_date(d.date) << ',' << cents_to_decimal(d.equity_cents) << ',' << cents_to_decimal(d.cash_cents) << ',' << cents_to_decimal(d.gross_exposure_cents) << ',' << d.n_positions << ',' << d.ret << ',' << d.drawdown_pct << "\n";
            std::cout << r.to_json(false).dump(2) << "\n";
            std::cout << "counterfactual: " << validator_counterfactual(r.outcomes).dump(2) << "\n";
            std::cout << "wrote " << (cfg.out_dir / "single.json").string() << "\n";
            return 0;
        }

        WalkForwardResult wf = run_walk_forward(bt, params, cfg, verbose);
        std::ofstream(cfg.out_dir / "walk_forward.json") << wf.to_json().dump(2);
        std::ofstream tr(cfg.out_dir / "oos_trades.csv");
        tr << ClosedTrade::csv_header() << "\n";
        for (const auto& t : wf.oos_trades) tr << t.to_csv() << "\n";
        std::string md = render_report_md(wf, cfg, params);
        std::ofstream(cfg.out_dir / "report.md") << md;
        std::cout << md << "\nwrote " << (cfg.out_dir / "report.md").string() << "\n";
        return wf.g1_pass ? 0 : 2;
    } catch (const std::exception& e) {
        spdlog::error("backtester failed: {}", e.what());
        return 1;
    }
}
