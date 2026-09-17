#include "strategy/tsmom_params.hpp"

#include "common/sha256.hpp"

#include <fstream>

namespace at {

namespace {
template <typename T>
void rd(const nlohmann::json& j, const char* k, T& out) {
    if (j.contains(k) && !j[k].is_null()) out = j[k].get<T>();
}
} // namespace

TsmomParams TsmomParams::from_json(const nlohmann::json& j) {
    TsmomParams p;
    rd(j, "strategy_version", p.strategy_version);
    if (j.contains("signal")) {
        const auto& s = j["signal"];
        rd(s, "momentum_lookback_sessions", p.signal.momentum_lookback_sessions);
        rd(s, "vol_lookback_sessions", p.signal.vol_lookback_sessions);
    }
    if (j.contains("sizing")) {
        const auto& s = j["sizing"];
        rd(s, "gross_cap_pct", p.sizing.gross_cap_pct);
        rd(s, "asset_class_cap_pct_default", p.sizing.asset_class_cap_pct_default);
    }
    if (j.contains("order")) {
        const auto& o = j["order"];
        rd(o, "order_type", p.order.order_type);
        rd(o, "tif", p.order.tif);
        rd(o, "rebalance_limit_offset_bps", p.order.rebalance_limit_offset_bps);
    }
    return p;
}

TsmomParams TsmomParams::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("tsmom params: cannot open " + file.string());
    nlohmann::json j;
    in >> j;
    return from_json(j);
}

nlohmann::json TsmomParams::to_json() const {
    return {
        {"strategy_version", strategy_version},
        {"signal", {
            {"momentum_lookback_sessions", signal.momentum_lookback_sessions},
            {"vol_lookback_sessions", signal.vol_lookback_sessions},
        }},
        {"sizing", {
            {"gross_cap_pct", sizing.gross_cap_pct},
            {"asset_class_cap_pct_default", sizing.asset_class_cap_pct_default},
        }},
        {"order", {
            {"order_type", order.order_type},
            {"tif", order.tif},
            {"rebalance_limit_offset_bps", order.rebalance_limit_offset_bps},
        }},
    };
}

std::string TsmomParams::fingerprint() const { return sha256_hex(to_json().dump()).substr(0, 16); }

} // namespace at
