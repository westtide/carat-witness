// Carat Live Profiler: public collection API.
//
// Usage from Carat code (all macros/functions are no-ops when profiling is
// off; the hot path is one relaxed atomic load plus a 64-byte ring push):
//
//   car::prof::ScopedSpan span(car::prof::Stage::IC3_BLOCK, /*ctx=*/m_k);
//   car::prof::Counter(car::prof::CTR_IC3_LEMMAS, 1);
//   car::prof::ModelStat(car::prof::MSTAT_AIG_ANDS, n_ands);
//
// SAT calls are wrapped centrally in SATSolver::Solve via SatScope; the
// query category is derived from the innermost open stage on that thread,
// so no lower-level signature changes are needed.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Protocol.h"

namespace car {
namespace prof {

enum class Level : uint8_t { Off = 0, Summary = 1, Detail = 2 };

struct ProfilerConfig {
    bool enabled = false;
    bool detail = false;  // Level::Detail when true, otherwise Summary
    std::string socket_path;  // Unix domain socket; empty = no live stream
    std::string file_path;    // offline/mirror binary trace; empty = none
    std::string algorithm;    // "ic3" / "fcar" / "bcar" / "kind" / ...
    uint8_t algorithm_code = 0;
    std::string model_path;
    std::string sat_backend;  // value of -s
    std::string carat_commit;
    std::vector<std::string> argv;
};

class Profiler {
public:
    static Profiler &Get();

    // Idempotent; the second call is ignored. Starts the sender thread.
    void Start(const ProfilerConfig &cfg);
    // Emits RUN_END, drains buffers, writes END_OF_STREAM, joins sender.
    void Shutdown(uint64_t result_code, uint64_t termination);
    // For SIGINT paths: flush as much as possible without RUN_END semantics.
    void FlushAndStop();

    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
    Level CurrentLevel() const {
        return static_cast<Level>(level_.load(std::memory_order_relaxed));
    }

    // --- hot-path API (called only when Enabled(); cheap otherwise guarded) ---
    uint64_t BeginSpan(Stage stage, uint32_t ctx);
    void EndSpan(Stage stage, uint32_t ctx);
    void Counter(uint32_t counter_id, uint64_t value);
    // Monotonic counters (lemma/obligation/core counts): accumulates
    // atomically; the sender thread batches value updates at a fixed
    // cadence so hot paths never flood the ring (docs §4.2 Summary level).
    void Bump(uint32_t counter_id, uint64_t delta = 1);
    // Sender-side: emit one COUNTER_UPDATE per changed slot, then clear.
    void FlushDirtyCounters();
    void ModelStat(uint32_t stat_id, uint64_t value);
    void IterationBegin(uint32_t iter);
    void IterationEnd(uint32_t iter, uint64_t progress, bool converged);

    // SAT query bracket. backend = SolverBackend enum; category is derived
    // from the current span when cat is passed as SATCAT_OTHER_NO_OVERRIDE.
    void SatBegin(uint64_t instance_id, uint32_t backend,
                  uint64_t category_override, uint64_t assumptions);
    void SatEnd(uint64_t instance_id, uint32_t backend,
                uint64_t category_override, uint64_t assumptions, bool sat);

    // Monotonic instance ids for SATSolver instances.
    static uint64_t NextInstanceId();

    // Internal: per-thread registration + telemetry used by the sender.
    struct ThreadState;
    ThreadState *RegisterThread();
    const std::vector<ThreadState *> &Threads() const { return threads_; }
    std::atomic<uint64_t> *DroppedCounter() { return &dropped_total_; }
    uint64_t NowNs() const;
    uint64_t RunStartWallNs() const { return run_start_ns_; }
    const std::string &RunId() const { return run_id_; }

private:
    Profiler() = default;
    struct Impl;
    void Emit(EventRecord &rec);  // fills timestamp/seq/thread then pushes
    ThreadState *ThisThread();

    std::atomic<bool> enabled_{false};
    std::atomic<uint8_t> level_{static_cast<uint8_t>(Level::Off)};
    std::atomic<uint64_t> span_seq_{1};
    inline static std::atomic<uint64_t> instance_seq_{1};
    std::atomic<uint64_t> dropped_total_{0};
    // Monotonic accumulator table for Bump(); 0x300 covers all counter ids.
    static constexpr size_t kCounterSlots = 0x300;
    inline static std::atomic<uint64_t> accum_[kCounterSlots]{};
    inline static std::atomic<bool> accum_dirty_[kCounterSlots]{};
    std::mutex registry_mu_;
    std::vector<ThreadState *> threads_;  // append-only after start
    std::string run_id_;
    uint64_t run_start_ns_ = 0;  // steady ns at Start()
    Impl *impl_ = nullptr;       // sender thread + writer (hidden)
};

// Derive a SAT category from the innermost open stage (thread-local).
uint64_t SatCategoryFromStage(Stage stage);

// RAII span. min_level lets hot per-task/per-query spans be Detail-only.
class ScopedSpan {
public:
    explicit ScopedSpan(Stage stage, uint32_t ctx = 0,
                        Level min_level = Level::Summary)
        : stage_(stage), ctx_(ctx) {
        if (Profiler::Get().Enabled() &&
            static_cast<uint8_t>(Profiler::Get().CurrentLevel()) >=
                static_cast<uint8_t>(min_level)) {
            span_id_ = Profiler::Get().BeginSpan(stage, ctx);
            active_ = true;
        }
    }
    ~ScopedSpan() {
        if (active_) Profiler::Get().EndSpan(stage_, ctx_);
    }
    ScopedSpan(const ScopedSpan &) = delete;
    ScopedSpan &operator=(const ScopedSpan &) = delete;
    uint64_t SpanId() const { return span_id_; }

private:
    Stage stage_;
    uint32_t ctx_;
    uint64_t span_id_ = 0;
    bool active_ = false;
};

// RAII SAT-query bracket used by SATSolver::Solve overloads.
class SatScope {
public:
    SatScope(uint64_t instance_id, uint32_t backend, uint64_t assumptions)
        : instance_id_(instance_id),
          backend_(backend),
          assumptions_(assumptions) {
        if (Profiler::Get().Enabled()) {
            entered_ = true;
            Profiler::Get().SatBegin(instance_id_, backend_,
                                     /*category_override=*/0, assumptions_);
        }
    }
    ~SatScope() {
        if (entered_)
            Profiler::Get().SatEnd(instance_id_, backend_, 0, assumptions_,
                                   result_);
    }
    SatScope(const SatScope &) = delete;
    SatScope &operator=(const SatScope &) = delete;
    void SetResult(bool sat) { result_ = sat; }

private:
    uint64_t instance_id_;
    uint32_t backend_;
    uint64_t assumptions_;
    bool result_ = false;
    bool entered_ = false;
};

// Convenience free functions (no-ops when profiling is off).
inline void Counter(uint32_t id, uint64_t value) {
    if (Profiler::Get().Enabled()) Profiler::Get().Counter(id, value);
}
inline void Bump(uint32_t id, uint64_t delta = 1) {
    if (Profiler::Get().Enabled()) Profiler::Get().Bump(id, delta);
}
inline void ModelStat(uint32_t stat_id, uint64_t value) {
    if (Profiler::Get().Enabled()) Profiler::Get().ModelStat(stat_id, value);
}
inline void IterationBegin(uint32_t iter) {
    if (Profiler::Get().Enabled()) Profiler::Get().IterationBegin(iter);
}
inline void IterationEnd(uint32_t iter, uint64_t progress, bool converged) {
    if (Profiler::Get().Enabled())
        Profiler::Get().IterationEnd(iter, progress, converged);
}

}  // namespace prof
} // namespace car
