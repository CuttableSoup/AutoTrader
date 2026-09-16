#include "ids.hpp"

#include "sha256.hpp"

namespace at {

std::string client_order_id_for_entry(std::string_view candidate_msg_id) {
    return "ate-" + sha256_hex(std::string("entry|") + std::string(candidate_msg_id)).substr(0, 32);
}

std::string client_order_id_for_exit(std::string_view symbol, std::string_view intent, std::string_view session_date,
                                     int seq) {
    std::string key = "exit|";
    key += symbol; key += '|'; key += intent; key += '|'; key += session_date; key += '|'; key += std::to_string(seq);
    return "atx-" + sha256_hex(key).substr(0, 32);
}

std::string client_order_id_for_stop_replace(std::string_view symbol, std::string_view session_date,
                                             long long new_stop_cents) {
    std::string key = "stop|";
    key += symbol; key += '|'; key += session_date; key += '|'; key += std::to_string(new_stop_cents);
    return "ats-" + sha256_hex(key).substr(0, 32);
}

std::string earnings_event_id(std::string_view symbol, std::string_view fiscal_period, std::string_view report_date) {
    std::string key(symbol);
    key += '|'; key += fiscal_period; key += '|'; key += report_date;
    return sha256_hex(key).substr(0, 32);
}

bool is_our_client_order_id(std::string_view id) {
    return id.size() == 36 && (id.starts_with("ate-") || id.starts_with("atx-") || id.starts_with("ats-"));
}

} // namespace at
