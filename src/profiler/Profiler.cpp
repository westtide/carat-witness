#include "Profiler.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RingBuffer.h"

namespace car {
namespace prof {

namespace {

constexpr size_t kRingRecords = 16 * 1024;  // 1 MiB per thread
constexpr size_t kBatchRecords = 256;       // 16 KiB per frame payload
constexpr int kPollIntervalUs = 2000;       // sender poll when idle
constexpr auto kHeartbeatInterval = std::chrono::milliseconds(100);
constexpr auto kAggregateInterval = std::chrono::milliseconds(500);
constexpr auto kReconnectInterval = std::chrono::milliseconds(1000);
constexpr size_t kMaxPendingBytes = 4 * 1024 * 1024;  // cap unsent bytes

uint64_t SteadyNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string NowStamp() {
    const auto t = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

void MakeNoPipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

}  // namespace

struct Profiler::ThreadState {
    explicit ThreadState(RingBuffer *r, uint32_t id)
        : ring(r), thread_id(id) {}
    RingBuffer *ring;
    const uint32_t thread_id;
    uint64_t seq = 0;
    // Open spans (span_id, stage); innermost last. cur_stage mirrors the top.
    std::vector<std::pair<uint64_t, uint16_t>> span_stack;
    uint16_t cur_stage = 0;
    uint64_t dropped = 0;
    // Active SAT query (read by the sender thread for heartbeats).
    std::atomic<uint64_t> sat_span{0};
    std::atomic<uint64_t> sat_start_ns{0};
    std::atomic<uint64_t> sat_meta{0};  // cat<<32 | instance_id
    // Per-thread SAT query context (begin/end pair on the same thread).
    uint64_t cur_sat_span = 0;
    uint64_t cur_sat_cat = 0;
    uint64_t cur_sat_assumptions = 0;
    // Summary aggregates by category, atomics: the sender reads them.
    static constexpr size_t kCats = 16;
    std::atomic<uint64_t> agg_count[kCats]{};
    std::atomic<uint64_t> agg_ns[kCats]{};
    std::atomic<uint64_t> agg_max_ns[kCats]{};
    std::atomic<uint64_t> agg_unsat[kCats]{};
};

namespace {

// Sender-thread writer: batches ring records into frames and pushes them to
// the Unix socket and/or binary file. Never blocks the producer for long:
// sends are timeout-bounded, overflow is dropped and counted.
class TraceWriter {
public:
    TraceWriter(const std::string &sock, const std::string &file,
                const std::string &handshake)
        : socket_path_(sock), file_path_(file), handshake_(handshake) {}

    void SetTarget(Profiler *p) { profiler_ = p; }

    void Run() {
        if (!file_path_.empty()) {
            file_ = std::fopen(file_path_.c_str(), "wb");
            if (file_)
                WriteFileFrame(FT_HANDSHAKE, handshake_.data(),
                               static_cast<uint32_t>(handshake_.size()), 0);
        }
        auto last_flush = std::chrono::steady_clock::now();
        auto last_hb = last_flush;
        auto last_agg = last_flush;
        while (!stop_.load(std::memory_order_relaxed)) {
            size_t got = 0;
            for (auto *ts : profiler_->Threads())
                got += ts->ring->Pop(batch_ + got, kBatchRecords - got);
            if (got > 0) {
                FlushBatch(batch_, got);
                last_flush = std::chrono::steady_clock::now();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= std::chrono::milliseconds(50)) {
                if (connected_ && !outbox_.empty())
                    connected_ = SendOutbox();
                last_flush = now;
            }
            if (now - last_hb >= kHeartbeatInterval) {
                EmitAuxiliary();
                last_hb = now;
            }
            if (now - last_agg >= kAggregateInterval) {
                EmitAggregates();
                profiler_->FlushDirtyCounters();
                last_agg = now;
            }
            if (got == 0)
                std::this_thread::sleep_for(
                    std::chrono::microseconds(kPollIntervalUs));
        }
        // Final drain: push everything still buffered.
        for (auto *ts : profiler_->Threads()) {
            size_t n;
            while ((n = ts->ring->Pop(batch_, kBatchRecords)) > 0)
                FlushBatch(batch_, n);
        }
        if (connected_) {
            AppendHeader(&outbox_, FT_EOS, 0, 0);
            SendOutbox();
            if (fd_ >= 0) ::close(fd_);
            fd_ = -1;
            connected_ = false;
        }
        if (file_) {
            WriteFileFrame(FT_EOS, nullptr, 0, 0);
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void RequestStop() {
        // Final flush so short runs still report their counters/aggregates.
        profiler_->FlushDirtyCounters();
        EmitAggregates();
        EmitAuxiliary();
        stop_.store(true, std::memory_order_relaxed);
    }

private:
    bool TryConnect() {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_try_) return false;
        next_try_ = now + kReconnectInterval;
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        MakeNoPipe(fd_);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(),
                     sizeof(addr.sun_path) - 1);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr),
                      sizeof(addr)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        timeval tv{0, 500 * 1000};  // bound every send to 500 ms
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        connected_ = true;
        outbox_.clear();
        // The handshake is the first frame on every (re)connection.
        AppendHeader(&outbox_, FT_HANDSHAKE,
                     static_cast<uint32_t>(handshake_.size()), 0);
        outbox_.insert(outbox_.end(), handshake_.begin(), handshake_.end());
        return SendOutbox();
    }

    static void AppendHeader(std::vector<char> *out, uint16_t type,
                             uint32_t len, uint32_t count) {
        FrameHeader h{{'C', 'R', 'P', 'F'}, kProtocolVersion, type, len, count};
        const char *p = reinterpret_cast<const char *>(&h);
        out->insert(out->end(), p, p + sizeof(h));
    }

    // Write a batch to file (if any) and socket (if connected).
    void FlushBatch(const EventRecord *recs, size_t n) {
        const uint32_t len = static_cast<uint32_t>(n * sizeof(EventRecord));
        if (file_)
            WriteFileFrame(FT_EVENT_BATCH, recs, len,
                           static_cast<uint32_t>(n));
        if (socket_path_.empty()) return;
        if (!connected_ && !TryConnect()) {
            dropped_send_ += n;
            return;
        }
        AppendHeader(&outbox_, FT_EVENT_BATCH, len, static_cast<uint32_t>(n));
        const char *p = reinterpret_cast<const char *>(recs);
        outbox_.insert(outbox_.end(), p, p + len);
        connected_ = SendOutbox();
    }

    // Best-effort ordered drain of outbox_. Returns false when the
    // connection died (outbox_ is dropped and counted in that case).
    bool SendOutbox() {
        if (!connected_) {
            DropOutbox();
            return false;
        }
        size_t off = 0;
        while (off < outbox_.size()) {
            const ssize_t w =
                ::send(fd_, outbox_.data() + off, outbox_.size() - off, 0);
            if (w > 0) {
                off += static_cast<size_t>(w);
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                          errno == EINTR)) {
                outbox_.erase(outbox_.begin(),
                              outbox_.begin() + static_cast<long>(off));
                return true;  // bounded wait; retry on the next poll
            }
            ::close(fd_);
            fd_ = -1;
            connected_ = false;
            DropOutbox();
            return false;
        }
        outbox_.clear();
        if (outbox_.size() > kMaxPendingBytes) DropOutbox();
        return true;
    }

    void DropOutbox() {
        if (!outbox_.empty()) {
            dropped_send_ +=
                outbox_.size() / sizeof(EventRecord) +
                (outbox_.size() % sizeof(EventRecord) ? 1 : 0);
            outbox_.clear();
        }
    }

    void WriteFileFrame(uint16_t type, const void *payload, uint32_t len,
                        uint32_t count) {
        FrameHeader h{{'C', 'R', 'P', 'F'}, kProtocolVersion, type, len, count};
        if (std::fwrite(&h, sizeof(h), 1, file_) != 1) return;
        if (payload && len)
            std::fwrite(payload, len, 1, file_);  // best effort on overflow
    }

    // Heartbeats + drop accounting. These are sender-side records so they
    // stay visible even while the producer is silent inside one long SAT.
    void EmitAuxiliary() {
        const uint64_t now = profiler_->NowNs();
        const uint64_t now_abs = SteadyNs();
        for (auto *ts : profiler_->Threads()) {
            const uint64_t span =
                ts->sat_span.load(std::memory_order_relaxed);
            EventRecord rec{};
            rec.timestamp_ns = now;
            rec.thread_id = ts->thread_id;
            rec.event_type = static_cast<uint16_t>(EventType::HEARTBEAT);
            rec.span_id = span;
            if (span != 0) {
                rec.value0 = span;
                const uint64_t started =
                    ts->sat_start_ns.load(std::memory_order_relaxed);
                rec.value1 = started ? (now_abs - started) : 0;
            }
            FlushBatch(&rec, 1);
        }
        const uint64_t dropped =
            profiler_->DroppedCounter()->load(std::memory_order_relaxed) +
            dropped_send_;
        if (dropped != last_dropped_) {
            EventRecord rec{};
            rec.timestamp_ns = now;
            rec.event_type = static_cast<uint16_t>(EventType::EVENTS_DROPPED);
            rec.value0 = dropped;
            FlushBatch(&rec, 1);
            last_dropped_ = dropped;
        }
    }

    void EmitAggregates() {
        const uint64_t now = profiler_->NowNs();
        for (auto *ts : profiler_->Threads()) {
            for (size_t c = 0; c < Profiler::ThreadState::kCats; ++c) {
                const uint64_t cnt =
                    ts->agg_count[c].load(std::memory_order_relaxed);
                if (cnt == 0) continue;
                const uint64_t ns =
                    ts->agg_ns[c].load(std::memory_order_relaxed);
                EventRecord rec{};
                rec.timestamp_ns = now;
                rec.thread_id = ts->thread_id;
                rec.event_type = static_cast<uint16_t>(EventType::SAT_AGGREGATE);
                rec.value0 = c;
                // 30-bit count << 34 | 34-bit cumulative microseconds.
                rec.value1 = ((cnt & 0x3FFFFFFFULL) << 34) |
                             ((ns / 1000) & 0x3FFFFFFFFULL);
                FlushBatch(&rec, 1);
                const uint64_t mx =
                    ts->agg_max_ns[c].load(std::memory_order_relaxed);
                if (mx != 0) {
                    EventRecord mrec = rec;
                    mrec.value1 = mx / 1000;  // max sample in microseconds
                    mrec.flags = 1;  // marks the max sample
                    FlushBatch(&mrec, 1);
                }
            }
        }
    }

    std::string socket_path_, file_path_, handshake_;
    Profiler *profiler_ = nullptr;
    std::atomic<bool> stop_{false};
    bool connected_ = false;
    int fd_ = -1;
    std::FILE *file_ = nullptr;
    std::vector<char> outbox_;    // ordered frame bytes not yet accepted by the OS
    uint64_t dropped_send_ = 0;
    uint64_t last_dropped_ = 0;
    std::chrono::steady_clock::time_point next_try_{};
    EventRecord batch_[kBatchRecords]{};
};

}  // namespace

struct Profiler::Impl {
    std::thread sender;
    std::unique_ptr<TraceWriter> writer;
};

uint64_t SatCategoryFromStage(Stage stage) {
    switch (stage) {
    case Stage::IC3_INIT:
    case Stage::FCAR_INIT:
    case Stage::BCAR_INIT: return SATCAT_INIT;
    case Stage::IC3_CTI:
    case Stage::FCAR_START_ENUM:
    case Stage::BCAR_START_ENUM: return SATCAT_START;
    case Stage::IC3_BLOCK:
    case Stage::FCAR_SEARCH:
    case Stage::BCAR_SEARCH: return SATCAT_BLOCK;
    case Stage::IC3_GENERALIZE:
    case Stage::FCAR_LEARN:
    case Stage::BCAR_LEARN: return SATCAT_GENERALIZE;
    case Stage::IC3_PROPAGATE:
    case Stage::FCAR_PROPAGATE:
    case Stage::BCAR_PROPAGATE: return SATCAT_PROPAGATE;
    case Stage::FCAR_INVARIANT:
    case Stage::BCAR_INVARIANT: return SATCAT_INVARIANT;
    case Stage::BCAR_CHECK_BAD: return SATCAT_BASE;
    case Stage::BCAR_REFINE: return SATCAT_REFINE;
    case Stage::KIND_BASE: return SATCAT_BASE;
    case Stage::KIND_IND: return SATCAT_IND;
    default: return SATCAT_OTHER;
    }
}

Profiler &Profiler::Get() {
    static Profiler instance;
    return instance;
}

uint64_t Profiler::NextInstanceId() {
    return instance_seq_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Profiler::NowNs() const { return SteadyNs() - run_start_ns_; }

Profiler::ThreadState *Profiler::RegisterThread() {
    static std::mutex mu;
    static uint32_t next_id = 0;
    auto *ts = new ThreadState(new RingBuffer(kRingRecords), next_id++);
    std::lock_guard<std::mutex> lock(registry_mu_);
    threads_.push_back(ts);
    return ts;
}

Profiler::ThreadState *Profiler::ThisThread() {
    static thread_local ThreadState *tls = nullptr;
    if (tls == nullptr) tls = RegisterThread();
    return tls;
}

void Profiler::Emit(EventRecord &rec) {
    ThreadState *ts = ThisThread();
    rec.timestamp_ns = NowNs();
    rec.sequence = ts->seq++;
    rec.thread_id = ts->thread_id;
    if (!ts->ring->Push(rec)) {
        ts->dropped++;
        dropped_total_.fetch_add(1, std::memory_order_relaxed);
    }
}

uint64_t Profiler::BeginSpan(Stage stage, uint32_t ctx) {
    ThreadState *ts = ThisThread();
    EventRecord rec{};
    rec.span_id = span_seq_.fetch_add(1, std::memory_order_relaxed);
    rec.parent_span_id = ts->span_stack.empty() ? 0
                                                : ts->span_stack.back().first;
    rec.event_type = static_cast<uint16_t>(EventType::PHASE_BEGIN);
    rec.stage_id = static_cast<uint16_t>(stage);
    rec.context_id = ctx;
    Emit(rec);
    ts->span_stack.emplace_back(rec.span_id, rec.stage_id);
    ts->cur_stage = rec.stage_id;
    return rec.span_id;
}

void Profiler::EndSpan(Stage stage, uint32_t ctx) {
    ThreadState *ts = ThisThread();
    if (ts->span_stack.empty()) return;
    const auto [span, stage_open] = ts->span_stack.back();
    (void)stage_open;
    ts->span_stack.pop_back();
    ts->cur_stage =
        ts->span_stack.empty() ? 0 : ts->span_stack.back().second;
    EventRecord rec{};
    rec.span_id = span;
    rec.parent_span_id = ts->span_stack.empty() ? 0
                                                : ts->span_stack.back().first;
    rec.event_type = static_cast<uint16_t>(EventType::PHASE_END);
    rec.stage_id = static_cast<uint16_t>(stage);
    rec.context_id = ctx;
    Emit(rec);
}

void Profiler::Counter(uint32_t counter_id, uint64_t value) {
    EventRecord rec{};
    rec.event_type = static_cast<uint16_t>(EventType::COUNTER_UPDATE);
    rec.value0 = counter_id;
    rec.value1 = value;
    Emit(rec);
}

void Profiler::Bump(uint32_t counter_id, uint64_t delta) {
    const size_t slot = counter_id < kCounterSlots ? counter_id : 0;
    accum_[slot].fetch_add(delta, std::memory_order_relaxed);
    accum_dirty_[slot].store(true, std::memory_order_relaxed);
    // Emission happens in FlushDirtyCounters() on the sender thread; hot
    // paths must never push per-call events (IC3 bumps obligations at
    // millions per minute).
}

void Profiler::FlushDirtyCounters() {
    for (uint32_t id = 1; id < kCounterSlots; ++id) {
        if (!accum_dirty_[id].exchange(false,
                                       std::memory_order_relaxed)) {
            continue;
        }
        Counter(id, accum_[id].load(std::memory_order_relaxed));
        // A producer bump racing between load and clear re-marks the slot;
        // its newer total goes out on the next flush cycle.
    }
}

void Profiler::ModelStat(uint32_t stat_id, uint64_t value) {
    EventRecord rec{};
    rec.event_type = static_cast<uint16_t>(EventType::MODEL_STAT);
    rec.value0 = stat_id;
    rec.value1 = value;
    Emit(rec);
}

void Profiler::IterationBegin(uint32_t iter) {
    EventRecord rec{};
    rec.event_type = static_cast<uint16_t>(EventType::ITERATION_BEGIN);
    rec.value0 = iter;
    Emit(rec);
}

void Profiler::IterationEnd(uint32_t iter, uint64_t progress,
                            bool converged) {
    EventRecord rec{};
    rec.event_type = static_cast<uint16_t>(EventType::ITERATION_END);
    rec.value0 = iter;
    rec.value1 = progress;
    rec.flags = converged ? 1u : 0u;
    Emit(rec);
}

void Profiler::SatBegin(uint64_t instance_id, uint32_t backend,
                        uint64_t category_override, uint64_t assumptions) {
    ThreadState *ts = ThisThread();
    uint64_t cat = category_override;
    if (cat == 0)
        cat = SatCategoryFromStage(static_cast<Stage>(ts->cur_stage));
    EventRecord rec{};
    rec.span_id = span_seq_.fetch_add(1, std::memory_order_relaxed);
    rec.parent_span_id = ts->span_stack.empty() ? 0
                                                : ts->span_stack.back().first;
    rec.event_type = static_cast<uint16_t>(EventType::SAT_BEGIN);
    rec.value0 = instance_id;
    rec.value1 = PackSatValue1(cat, 0, assumptions);
    rec.flags = static_cast<uint32_t>(backend & 0xFF) << 8;
    if (CurrentLevel() == Level::Detail) Emit(rec);
    // Active-query tracking and aggregates run in every level: the heartbeat
    // path makes long SAT calls visible even in Summary mode.
    ts->sat_span.store(rec.span_id, std::memory_order_relaxed);
    ts->sat_start_ns.store(SteadyNs(), std::memory_order_relaxed);
    ts->sat_meta.store((cat << 32) | (instance_id & 0xFFFFFFFFULL),
                       std::memory_order_relaxed);
    ts->cur_sat_span = rec.span_id;
    ts->cur_sat_cat = cat;
    ts->cur_sat_assumptions = assumptions;
    (void)backend;
}

void Profiler::SatEnd(uint64_t instance_id, uint32_t backend,
                      uint64_t /*category_override*/, uint64_t /*assumptions*/,
                      bool sat) {
    ThreadState *ts = ThisThread();
    const uint64_t span = ts->cur_sat_span;
    const uint64_t cat = ts->cur_sat_cat;
    const uint64_t started = ts->sat_start_ns.load(std::memory_order_relaxed);
    const uint64_t dur = started ? SteadyNs() - started : 0;
    ts->sat_span.store(0, std::memory_order_relaxed);
    EventRecord rec{};
    rec.span_id = span;
    rec.parent_span_id = ts->span_stack.empty() ? 0
                                                : ts->span_stack.back().first;
    rec.event_type = static_cast<uint16_t>(EventType::SAT_END);
    rec.value0 = instance_id;
    rec.value1 = dur;  // duration ns; category is in SAT_BEGIN + aggregates
    rec.flags = (sat ? 1u : 2u) | (static_cast<uint32_t>(backend & 0xFF) << 8);
    if (CurrentLevel() == Level::Detail) Emit(rec);
    const size_t c = cat < ThreadState::kCats ? cat : 0;
    ts->agg_count[c].fetch_add(1, std::memory_order_relaxed);
    ts->agg_ns[c].fetch_add(dur, std::memory_order_relaxed);
    uint64_t mx = ts->agg_max_ns[c].load(std::memory_order_relaxed);
    while (dur > mx &&
           !ts->agg_max_ns[c].compare_exchange_weak(mx, dur,
                                                    std::memory_order_relaxed)) {
    }
    if (!sat) ts->agg_unsat[c].fetch_add(1, std::memory_order_relaxed);
}

void Profiler::Start(const ProfilerConfig &cfg) {
    if (!cfg.enabled) return;
    bool expected = false;
    if (!enabled_.compare_exchange_strong(expected, true,
                                          std::memory_order_relaxed))
        return;
    run_start_ns_ = SteadyNs();
    run_id_ = NowStamp() + "-" + std::to_string(::getpid());
    level_.store(
        static_cast<uint8_t>(cfg.detail ? Level::Detail : Level::Summary),
        std::memory_order_relaxed);

    std::string argv_json;
    for (size_t i = 0; i < cfg.argv.size(); ++i) {
        if (i) argv_json += ", ";
        argv_json += "\"" + cfg.argv[i] + "\"";
    }
    std::string handshake =
        "{\"run_id\":\"" + run_id_ + "\",\"protocol_version\":" +
        std::to_string(kProtocolVersion) + ",\"carat_commit\":\"" +
        cfg.carat_commit + "\",\"model_path\":\"" + cfg.model_path +
        "\",\"algorithm\":\"" + cfg.algorithm + "\",\"sat_backend\":\"" +
        cfg.sat_backend + "\",\"trace_level\":\"" +
        (cfg.detail ? "detail" : "summary") + "\",\"argv\":[" + argv_json +
        "],\"pid\":" + std::to_string(::getpid()) + "}";

    auto *writer = new TraceWriter(cfg.socket_path, cfg.file_path, handshake);
    writer->SetTarget(this);
    impl_ = new Impl{std::thread([writer]() { writer->Run(); }), nullptr};
    impl_->writer.reset(writer);

    EventRecord rec{};
    rec.event_type = static_cast<uint16_t>(EventType::RUN_START);
    rec.flags = cfg.algorithm_code;
    Emit(rec);
}

void Profiler::Shutdown(uint64_t result_code, uint64_t termination) {
    if (!enabled_.exchange(false, std::memory_order_relaxed)) return;
    EventRecord rec{};
    rec.event_type = static_cast<uint16_t>(EventType::RUN_END);
    rec.value0 = result_code;
    rec.value1 = termination;
    Emit(rec);
    if (impl_ && impl_->sender.joinable()) {
        impl_->writer->RequestStop();
        impl_->sender.join();
    }
    delete impl_;
    impl_ = nullptr;
}

void Profiler::FlushAndStop() {
    if (!enabled_.exchange(false, std::memory_order_relaxed)) return;
    if (impl_ && impl_->sender.joinable()) {
        impl_->writer->RequestStop();
        impl_->sender.join();
    }
    delete impl_;
    impl_ = nullptr;
}

}  // namespace prof
}  // namespace car
