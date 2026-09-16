#include "common/bus.hpp"
#include "common/envelope.hpp"
#include "common/ids.hpp"
#include "common/sha256.hpp"
#include "common/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace at;

TEST_CASE("sha256 known vectors", "[ids]") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("The quick brown fox jumps over the lazy dog") == "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST_CASE("uuid v4 format and uniqueness", "[ids]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        std::string u = uuid_v4();
        CHECK(is_uuid(u));
        CHECK(u[14] == '4');
        seen.insert(u);
    }
    CHECK(seen.size() == 1000);
    CHECK(!is_uuid("not-a-uuid"));
    CHECK(!is_uuid("123e4567-e89b-12d3-a456-42661417400G"));
}

TEST_CASE("client_order_id is deterministic and namespaced", "[ids]") {
    std::string a = client_order_id_for_entry("11111111-2222-4333-8444-555555555555");
    std::string b = client_order_id_for_entry("11111111-2222-4333-8444-555555555555");
    std::string c = client_order_id_for_entry("11111111-2222-4333-8444-555555555556");
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.size() == 36);
    CHECK(a.starts_with("ate-"));
    CHECK(is_our_client_order_id(a));
    std::string x = client_order_id_for_exit("AAPL", "EXIT_TIME", "2026-09-15");
    CHECK(x == client_order_id_for_exit("AAPL", "EXIT_TIME", "2026-09-15"));
    CHECK(x != client_order_id_for_exit("AAPL", "EXIT_TIME", "2026-09-15", 1));
    CHECK(x != client_order_id_for_exit("AAPL", "EXIT_EARNINGS", "2026-09-15"));
    CHECK(client_order_id_for_stop_replace("AAPL", "2026-09-15", 12345).starts_with("ats-"));
    CHECK(!is_our_client_order_id("manual-order-1"));
    CHECK(earnings_event_id("AAPL", "2026Q3", "2026-10-29").size() == 32);
}

TEST_CASE("envelope round trip and validation", "[envelope]") {
    Envelope e = make_envelope("strategy", {{"symbol", "AAPL"}});
    CHECK(is_uuid(e.msg_id));
    CHECK(e.schema_version == "1.0");
    Envelope back = Envelope::parse(e.dump());
    CHECK(back.msg_id == e.msg_id);
    CHECK(back.producer == "strategy");
    CHECK(back.payload["symbol"] == "AAPL");
    CHECK_THROWS(Envelope::parse("{}"));
    CHECK_THROWS(Envelope::parse(R"({"schema_version":"1.0","msg_id":"x","ts_utc":"2026-09-15T00:00:00Z","producer":"p","payload":{}})"));
    CHECK_THROWS(Envelope::parse(R"({"schema_version":"1.0","msg_id":"11111111-2222-4333-8444-555555555555","ts_utc":"bad","producer":"p","payload":{}})"));
}

TEST_CASE("subject wildcards", "[bus]") {
    CHECK(subject_matches("market.data.>", "market.data.bar.AAPL"));
    CHECK(subject_matches("market.data.bar.*", "market.data.bar.AAPL"));
    CHECK(!subject_matches("market.data.bar.*", "market.data.quote.AAPL"));
    CHECK(!subject_matches("market.data.bar.*", "market.data.bar.AAPL.x"));
    CHECK(subject_matches("control.*", "control.halt"));
    CHECK(!subject_matches("control.*", "control"));
    CHECK(subject_matches("orders.approved", "orders.approved"));
    CHECK(!subject_matches("orders.approved", "orders.approvedx"));
}

TEST_CASE("memory bus delivers, redelivers, and consumers dedupe", "[bus]") {
    MemoryBus bus;
    bus.set_redeliver_every_message(true);
    Dedupe dd(10);
    int handled = 0, unique = 0;
    bus.subscribe("signals.>", "test", [&](const Delivery& d) {
        ++handled;
        if (dd.first_time(d.env.msg_id)) ++unique;
    });
    bus.publish("signals.candidate", make_envelope("strategy", {{"a", 1}}));
    bus.publish("signals.validated", make_envelope("validator", {{"b", 2}}));
    bus.publish("orders.approved", make_envelope("risk", {{"c", 3}}));
    bus.drain();
    CHECK(handled == 4);   // 2 matching messages x 2 deliveries
    CHECK(unique == 2);
    CHECK(bus.published("signals.>").size() == 2);
    CHECK(bus.published("orders.approved").size() == 1);
}

TEST_CASE("memory bus naks on throw and redelivers", "[bus]") {
    MemoryBus bus;
    int attempts = 0;
    bus.subscribe("x", "t", [&](const Delivery&) { if (++attempts < 3) throw std::runtime_error("transient"); });
    bus.publish("x", make_envelope("p", {}));
    bus.drain();
    CHECK(attempts == 3);
}

TEST_CASE("dedupe evicts oldest", "[bus]") {
    Dedupe dd(3);
    CHECK(dd.first_time("a"));
    CHECK(dd.first_time("b"));
    CHECK(dd.first_time("c"));
    CHECK(!dd.first_time("a"));
    CHECK(dd.first_time("d"));   // evicts a
    CHECK(dd.first_time("a"));   // a forgotten
}
