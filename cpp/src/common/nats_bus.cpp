#include "nats_bus.hpp"

#if __has_include(<nats/nats.h>)
#include <nats/nats.h>   // vcpkg / installed layout
#else
#include <nats.h>        // nats.c source tree (FetchContent)
#endif
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace at {

namespace {
void check(natsStatus s, const char* what, jsErrCode jerr = (jsErrCode)0) {
    if (s == NATS_OK) return;
    std::string msg = std::string(what) + ": " + natsStatus_GetText(s);
    if (jerr) msg += " (js err " + std::to_string(static_cast<int>(jerr)) + ")";
    const char* last = nats_GetLastError(nullptr);
    if (last && *last) msg += " [" + std::string(last) + "]";
    throw std::runtime_error(msg);
}
constexpr std::int64_t kNsPerSec = 1000000000LL;
} // namespace

struct NatsBus::Impl {
    natsConnection* nc = nullptr;
    jsCtx* js = nullptr;
    std::string client_name;
    struct Sub {
        natsSubscription* sub = nullptr;
        std::string filter;
        std::string durable;
        Handler handler;
    };
    std::vector<Sub> subs;
    Dedupe dedupe{200000};
};

NatsBus::NatsBus(const std::string& url, const std::string& client_name) : impl_(std::make_unique<Impl>()) {
    impl_->client_name = client_name;
    natsOptions* opts = nullptr;
    check(natsOptions_Create(&opts), "natsOptions_Create");
    natsOptions_SetURL(opts, url.c_str());
    natsOptions_SetName(opts, client_name.c_str());
    natsOptions_SetMaxReconnect(opts, -1);
    natsOptions_SetReconnectWait(opts, 1000);
    natsOptions_SetTimeout(opts, 5000);
    natsStatus s = natsConnection_Connect(&impl_->nc, opts);
    natsOptions_Destroy(opts);
    check(s, "natsConnection_Connect");
    jsOptions jo;
    jsOptions_Init(&jo);
    jo.Wait = 5000;
    check(natsConnection_JetStream(&impl_->js, impl_->nc, &jo), "natsConnection_JetStream");
    spdlog::info("nats: connected to {} as {}", url, client_name);
}

NatsBus::~NatsBus() { close(); }

void NatsBus::close() {
    if (!impl_) return;
    for (auto& s : impl_->subs) {
        if (s.sub) { natsSubscription_Destroy(s.sub); s.sub = nullptr; }
    }
    impl_->subs.clear();
    if (impl_->js) { jsCtx_Destroy(impl_->js); impl_->js = nullptr; }
    if (impl_->nc) {
        natsConnection_Drain(impl_->nc);
        natsConnection_Destroy(impl_->nc);
        impl_->nc = nullptr;
    }
}

bool NatsBus::connected() const {
    return impl_ && impl_->nc && natsConnection_Status(impl_->nc) == NATS_CONN_STATUS_CONNECTED;
}

void NatsBus::ensure_streams(const SchemaRegistry& registry) {
    for (const auto& spec : registry.streams()) {
        jsStreamConfig cfg;
        jsStreamConfig_Init(&cfg);
        cfg.Name = spec.name.c_str();
        std::vector<const char*> subjects;
        for (const auto& s : spec.subjects) subjects.push_back(s.c_str());
        cfg.Subjects = subjects.data();
        cfg.SubjectsLen = static_cast<int>(subjects.size());
        cfg.MaxAge = static_cast<std::int64_t>(spec.max_age_days) * 86400 * kNsPerSec;
        cfg.MaxMsgs = spec.max_msgs;
        cfg.Duplicates = static_cast<std::int64_t>(spec.duplicate_window_s) * kNsPerSec;
        cfg.Storage = js_FileStorage;
        cfg.Retention = js_LimitsPolicy;
        cfg.Discard = js_DiscardOld;

        jsErrCode jerr = (jsErrCode)0;
        jsStreamInfo* si = nullptr;
        natsStatus s = js_GetStreamInfo(&si, impl_->js, spec.name.c_str(), nullptr, &jerr);
        if (s == NATS_NOT_FOUND) {
            s = js_AddStream(&si, impl_->js, &cfg, nullptr, &jerr);
            check(s, ("js_AddStream " + spec.name).c_str(), jerr);
            spdlog::info("nats: created stream {} subjects={} max_age={}d max_msgs={}", spec.name, spec.subjects.size(),
                         spec.max_age_days, spec.max_msgs);
        } else {
            check(s, ("js_GetStreamInfo " + spec.name).c_str(), jerr);
            jsStreamInfo_Destroy(si);
            si = nullptr;
            s = js_UpdateStream(&si, impl_->js, &cfg, nullptr, &jerr);
            check(s, ("js_UpdateStream " + spec.name).c_str(), jerr);
            spdlog::debug("nats: updated stream {}", spec.name);
        }
        jsStreamInfo_Destroy(si);
    }
}

void NatsBus::publish(const std::string& subject, const Envelope& env) {
    nlohmann::json j = env.to_json();
    if (registry_) registry_->validate_or_throw(subject, j);
    std::string data = j.dump();
    jsPubOptions po;
    jsPubOptions_Init(&po);
    po.MsgId = env.msg_id.c_str();
    po.MaxWait = 5000;
    jsPubAck* ack = nullptr;
    jsErrCode jerr = (jsErrCode)0;
    natsStatus s = js_Publish(&ack, impl_->js, subject.c_str(), data.data(), static_cast<int>(data.size()), &po, &jerr);
    if (s != NATS_OK) check(s, ("js_Publish " + subject).c_str(), jerr);
    if (ack) {
        if (ack->Duplicate) spdlog::debug("nats: duplicate publish deduped by JetStream msg_id={}", env.msg_id);
        jsPubAck_Destroy(ack);
    }
}

void NatsBus::subscribe(const std::string& subject_filter, const std::string& durable, Handler handler) {
    jsSubOptions so;
    jsSubOptions_Init(&so);
    so.Config.AckPolicy = js_AckExplicit;
    so.Config.DeliverPolicy = js_DeliverAll;
    so.Config.MaxAckPending = 1024;
    so.Config.AckWait = 30 * kNsPerSec;
    so.Config.MaxDeliver = 10;
    so.ManualAck = true;
    natsSubscription* sub = nullptr;
    jsErrCode jerr = (jsErrCode)0;
    natsStatus s = js_PullSubscribe(&sub, impl_->js, subject_filter.c_str(), durable.c_str(), nullptr, &so, &jerr);
    check(s, ("js_PullSubscribe " + subject_filter + " durable=" + durable).c_str(), jerr);
    impl_->subs.push_back(Impl::Sub{sub, subject_filter, durable, std::move(handler)});
    spdlog::info("nats: subscribed {} durable={}", subject_filter, durable);
}

std::size_t NatsBus::poll() {
    std::size_t handled = 0;
    for (auto& sub : impl_->subs) {
        natsMsgList list{};
        jsErrCode jerr = (jsErrCode)0;
        natsStatus s = natsSubscription_Fetch(&list, sub.sub, 64, 100, &jerr);
        if (s == NATS_TIMEOUT) continue;
        if (s != NATS_OK) {
            spdlog::warn("nats: fetch {} failed: {}", sub.filter, natsStatus_GetText(s));
            continue;
        }
        for (int i = 0; i < list.Count; ++i) {
            natsMsg* m = list.Msgs[i];
            const char* subj = natsMsg_GetSubject(m);
            std::string_view body(natsMsg_GetData(m), static_cast<std::size_t>(natsMsg_GetDataLength(m)));
            Delivery d;
            d.subject = subj ? subj : "";
            jsMsgMetaData* meta = nullptr;
            if (natsMsg_GetMetaData(&meta, m) == NATS_OK && meta) {
                d.seq = meta->Sequence.Stream;
                d.redelivery_count = static_cast<int>(meta->NumDelivered) - 1;
                jsMsgMetaData_Destroy(meta);
            }
            bool ok = true;
            try {
                d.env = Envelope::parse(body);
                if (!impl_->dedupe.first_time(d.env.msg_id)) {
                    spdlog::debug("nats: duplicate msg_id {} on {} skipped", d.env.msg_id, d.subject);
                } else {
                    sub.handler(d);
                }
            } catch (const std::exception& e) {
                ok = false;
                spdlog::error("nats: handler for {} threw: {}", d.subject, e.what());
            }
            if (ok) natsMsg_Ack(m, nullptr);
            else natsMsg_NakWithDelay(m, 2000, nullptr);
            ++handled;
        }
        natsMsgList_Destroy(&list);
    }
    return handled;
}

} // namespace at
