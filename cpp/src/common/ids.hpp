// Deterministic identifiers. Retries of the same logical action must produce
// the same id so the broker and the bus dedupe them.
#pragma once
#include <string>
#include <string_view>

namespace at {

// Entry order for a candidate: "ate-" + sha256(candidate_msg_id)[0:32].
std::string client_order_id_for_entry(std::string_view candidate_msg_id);

// Exit / management order: "atx-" + sha256(symbol|intent|session_date|seq)[0:32].
// seq distinguishes multiple exits of the same intent on the same day (normally 0).
std::string client_order_id_for_exit(std::string_view symbol, std::string_view intent,
                                     std::string_view session_date, int seq = 0);

// Stop-leg replacement: "ats-" + sha256(symbol|session_date|new_stop_cents)[0:32].
std::string client_order_id_for_stop_replace(std::string_view symbol, std::string_view session_date,
                                             long long new_stop_cents);

// Earnings event id: sha256(symbol|fiscal_period|report_date)[0:32].
std::string earnings_event_id(std::string_view symbol, std::string_view fiscal_period, std::string_view report_date);

// Is this a client_order_id minted by this system?
bool is_our_client_order_id(std::string_view id);

} // namespace at
