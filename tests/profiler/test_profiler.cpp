// Unit tests for the profiler collection layer: ring buffer behavior,
// the 64-byte protocol layout, and a full offline-run round trip.
// Build: ninja carat_prof_test && ./carat_prof_test
#include "profiler/Profiler.h"
#include "profiler/Protocol.h"
#include "profiler/RingBuffer.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace car;
using namespace car::prof;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static void TestSizes() {
    CHECK(sizeof(EventRecord) == 64);
    CHECK(sizeof(FrameHeader) == 16);
}

static void TestRingBuffer() {
    RingBuffer rb(8);
    EventRecord rec{};
    for (size_t i = 0; i < 8; ++i) {
        rec.sequence = i;
        CHECK(rb.Push(rec));
    }
    CHECK(!rb.Push(rec));       // full
    CHECK(rb.Size() == 8);
    EventRecord out[16];
    CHECK(rb.Pop(out, 16) == 8);
    for (size_t i = 0; i < 8; ++i) CHECK(out[i].sequence == i);
    CHECK(rb.Size() == 0);
    // Interleaved producer/consumer keeps ordering per producer.
    rec.sequence = 100;
    CHECK(rb.Push(rec));
    CHECK(rb.Pop(out, 4) == 1 && out[0].sequence == 100);
}

static void TestRingBufferConcurrent() {
    RingBuffer rb(1024);
    std::atomic<uint64_t> produced{0}, consumed{0};
    std::thread prod([&] {
        EventRecord r{};
        for (uint64_t i = 0; i < 100000; ++i) {
            r.sequence = i;
            while (!rb.Push(r)) {}  // spin: keep the stream gap-free
            produced.fetch_add(1);
        }
    });
    EventRecord buf[64];
    std::thread cons([&] {
        while (consumed.load() < 100000) {
            size_t n = rb.Pop(buf, 64);
            if (n == 0) continue;
            const uint64_t base = consumed.load();
            for (size_t i = 0; i < n; ++i) {
                CHECK(buf[i].sequence == base + i);
            }
            consumed.fetch_add(n);
        }
    });
    prod.join();
    cons.join();
    CHECK(consumed.load() == produced.load());
    CHECK(produced.load() == 100000);
}

// Round trip: run the profiler in file mode, then parse the file with the
// same frame rules the daemon uses.
static void TestOfflineRunRoundTrip() {
    const char *path = "/tmp/carat_prof_test_roundtrip.cprof";
    std::remove(path);

    ProfilerConfig cfg;
    cfg.enabled = true;
    cfg.detail = true;
    cfg.socket_path = "";
    cfg.file_path = path;
    cfg.algorithm = "test";
    cfg.algorithm_code = 5;
    Profiler::Get().Start(cfg);
    CHECK(Profiler::Get().Enabled());

    {
        ScopedSpan outer(Stage::IC3_BLOCK, 7);
        {
            ScopedSpan inner(Stage::IC3_GENERALIZE, 7, Level::Detail);
            SatScope sat(42, BACKEND_MINICORE, 3);
        }
        Counter(CTR_IC3_LEMMAS, 5);
        Bump(CTR_IC3_OBL_CREATED);   // deferred to the sender's flush
        Bump(CTR_IC3_OBL_CREATED, 2);
        IterationBegin(7);
        IterationEnd(7, 3, true);
        ModelStat(MSTAT_AIG_ANDS, 1234);
    }
    Profiler::Get().Shutdown(1, 0);
    CHECK(!Profiler::Get().Enabled());

    // Parse.
    FILE *f = std::fopen(path, "rb");
    CHECK(f != nullptr);
    if (!f) return;
    FrameHeader hdr{};
    int batches = 0, events = 0, handshakes = 0, eos = 0;
    bool saw_sat_begin = false, saw_sat_end = false, saw_iter_end = false;
    uint64_t obl_created = 0;
    while (std::fread(&hdr, sizeof(hdr), 1, f) == 1) {
        CHECK(std::memcmp(hdr.magic, "CRPF", 4) == 0);
        CHECK(hdr.version == 1);
        std::vector<char> payload(hdr.payload_len);
        if (hdr.payload_len &&
            std::fread(payload.data(), hdr.payload_len, 1, f) != 1)
            break;
        if (hdr.msg_type == FT_HANDSHAKE) ++handshakes;
        else if (hdr.msg_type == FT_EOS) ++eos;
        else if (hdr.msg_type == FT_EVENT_BATCH) {
            ++batches;
            CHECK(hdr.payload_len == hdr.count * sizeof(EventRecord));
            for (uint32_t i = 0; i < hdr.count; ++i) {
                EventRecord rec;
                std::memcpy(&rec, payload.data() + i * sizeof(rec),
                            sizeof(rec));
                ++events;
                if (rec.event_type ==
                    static_cast<uint16_t>(EventType::SAT_BEGIN))
                    saw_sat_begin = true;
                if (rec.event_type ==
                    static_cast<uint16_t>(EventType::SAT_END))
                    saw_sat_end = true;
                if (rec.event_type ==
                    static_cast<uint16_t>(EventType::ITERATION_END))
                    saw_iter_end = true;
                if (rec.event_type ==
                        static_cast<uint16_t>(EventType::COUNTER_UPDATE) &&
                    rec.value0 == CTR_IC3_OBL_CREATED)
                    obl_created = rec.value1;
            }
        }
    }
    std::fclose(f);
    std::remove(path);

    CHECK(handshakes == 1);
    CHECK(eos == 1);
    CHECK(batches >= 1);
    CHECK(events >= 10);  // 4 phase + 2 sat + iter pair + counter + stat...
    CHECK(saw_sat_begin && saw_sat_end && saw_iter_end);
    // Bump() defers to the sender flush; the final value must still land.
    CHECK(obl_created == 3);
}

// Disabled profiling must be a no-op (acceptance #1).
static void TestDisabledNoop() {
    CHECK(!Profiler::Get().Enabled());
    ScopedSpan span(Stage::IC3_BLOCK);  // must not crash or emit
    Counter(CTR_IC3_LEMMAS, 1);
    SatScope sat(1, BACKEND_CADICAL, 0);
    sat.SetResult(true);
}

int main() {
    TestSizes();
    TestRingBuffer();
    TestRingBufferConcurrent();
    TestOfflineRunRoundTrip();
    TestDisabledNoop();
    if (g_failures == 0) {
        std::printf("carat_prof_test: all tests passed\n");
        return 0;
    }
    std::printf("carat_prof_test: %d failure(s)\n", g_failures);
    return 1;
}
