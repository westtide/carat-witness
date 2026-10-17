// Carat Live Profiler: wire protocol shared by the C++ collector, the
// daemon and the web frontend. See docs/profiler_protocol.md (v1).
#pragma once

#include <cstdint>

namespace car {
namespace prof {

constexpr uint16_t kProtocolVersion = 1;

enum class EventType : uint16_t {
    RUN_START = 1,
    RUN_END = 2,
    PHASE_BEGIN = 3,
    PHASE_END = 4,
    ITERATION_BEGIN = 5,
    ITERATION_END = 6,
    SAT_BEGIN = 7,
    SAT_END = 8,
    COUNTER_UPDATE = 9,
    MODEL_STAT = 10,
    ALGORITHM_SNAPSHOT = 11,
    HEARTBEAT = 12,
    EVENTS_DROPPED = 13,
    SAT_AGGREGATE = 14,
};

// Stable numeric stage ids (docs/profiler_instrumentation.md §6).
// Never reuse a number; unknown ids must be ignored by consumers.
enum class Stage : uint16_t {
    NONE = 0,
    // Word-level preprocessing.
    MODEL_LOAD = 101,
    MODEL_COI = 102,
    MODEL_ARRAY_SIMPLIFY = 103,
    MODEL_PACKAGE_RESIZE = 104,
    MODEL_BITBLAST = 105,
    MODEL_INIT_CHECKER = 106,
    // Outer array CEGAR.
    CEGAR_ROUND = 201,
    CEGAR_CHECK = 202,
    CEGAR_CEX_ANALYZE = 203,
    CEGAR_REFINE = 204,
    CEGAR_PREFIX_BMC = 205,
    // IC3.
    IC3_INIT = 301,
    IC3_EXTEND = 302,
    IC3_CTI = 303,
    IC3_BLOCK = 304,
    IC3_GENERALIZE = 305,
    IC3_PROPAGATE = 306,
    // Forward CAR (searches predecessors of bad states).
    FCAR_INIT = 401,
    FCAR_START_ENUM = 402,
    FCAR_SEARCH = 403,
    FCAR_LEARN = 404,
    FCAR_PROPAGATE = 405,
    FCAR_INVARIANT = 406,
    // Backward CAR (searches successors of initial states).
    BCAR_INIT = 501,
    BCAR_CHECK_BAD = 502,
    BCAR_START_ENUM = 503,
    BCAR_SEARCH = 504,
    BCAR_LEARN = 505,
    BCAR_PROPAGATE = 506,
    BCAR_INVARIANT = 507,
    BCAR_REFINE = 508,
    // k-induction.
    KIND_INIT = 601,
    KIND_UNROLL = 602,
    KIND_BASE = 603,
    KIND_IND = 604,
    KIND_SIMPLEPATH = 605,
};

// SAT query categories carried in SAT_BEGIN/SAT_END value1 bits 0-15.
enum SatCategory : uint64_t {
    SATCAT_OTHER = 0,
    SATCAT_INIT = 1,
    SATCAT_START = 2,
    SATCAT_LIFT = 3,
    SATCAT_BLOCK = 4,
    SATCAT_GENERALIZE = 5,
    SATCAT_PROPAGATE = 6,
    SATCAT_INVARIANT = 7,
    SATCAT_BASE = 8,
    SATCAT_IND = 9,
    SATCAT_REFINE = 10,
};

// Solver backend ids carried in SAT flags bits 8-15.
enum SolverBackend : uint32_t {
    BACKEND_UNKNOWN = 0,
    BACKEND_MINISAT = 1,
    BACKEND_CADICAL = 2,
    BACKEND_MINICORE = 3,
    BACKEND_KISSAT = 4,
};

#pragma pack(push, 1)
struct EventRecord {
    uint64_t timestamp_ns;    // monotonic ns since run start
    uint64_t sequence;        // per-thread event ordinal
    uint64_t span_id;         // counter-derived id, never a pointer
    uint64_t parent_span_id;  // 0 at the top of the span tree
    uint64_t value0;
    uint64_t value1;
    uint32_t thread_id;
    uint16_t event_type;
    uint16_t stage_id;
    uint32_t context_id;      // frame / depth / CEGAR round
    uint32_t flags;           // SAT_END: bit0 sat, bit1 unsat; SAT_*: bits 8-15 backend
};
#pragma pack(pop)
static_assert(sizeof(EventRecord) == 64, "EventRecord must be 64 bytes");

#pragma pack(push, 1)
struct FrameHeader {
    char magic[4];  // 'C','R','P','F'
    uint16_t version;
    uint16_t msg_type;
    uint32_t payload_len;
    uint32_t count;
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be 16 bytes");

enum FrameType : uint16_t {
    FT_HANDSHAKE = 1,
    FT_EVENT_BATCH = 2,
    FT_EOS = 3,
};

// SAT_BEGIN / SAT_END value1 packing helpers.
inline uint64_t PackSatValue1(uint64_t category, uint64_t stage_hint, uint64_t assumptions) {
    return (category & 0xFFFF) | ((stage_hint & 0xFFFF) << 16) | ((assumptions & 0xFFFFFFFFULL) << 32);
}

// Counter ids (docs/profiler_protocol.md §5).
enum CounterId : uint32_t {
    // 0x1000+cat SAT queries, 0x1100+cat cumulative SAT ms, 0x1200+cat UNSAT count
    CTR_SAT_QUERIES = 0x1000,
    CTR_SAT_MS = 0x1100,
    CTR_SAT_UNSAT = 0x1200,
    CTR_IC3_OBL_CREATED = 0x2000,
    CTR_IC3_OBL_DONE = 0x2001,
    CTR_IC3_OBL_QUEUE = 0x2002,
    CTR_IC3_LEMMAS = 0x2010,
    CTR_IC3_PROP_TRIED = 0x2011,
    CTR_IC3_PROP_OK = 0x2012,
    CTR_CAR_NEW_STATES = 0x2100,
    CTR_CAR_CORES = 0x2101,
    CTR_CAR_GEN_OK = 0x2102,
    CTR_CAR_PROP_TRIED = 0x2103,
    CTR_CAR_PROP_OK = 0x2104,
    CTR_CAR_RESTARTS = 0x2105,
    CTR_KIND_SP_PAIRS = 0x2200,
};

// Model statistic ids (docs/profiler_protocol.md §6).
enum ModelStatId : uint32_t {
    MSTAT_BTOR2_NODES = 1,
    MSTAT_BTOR2_COI_NODES = 2,
    MSTAT_STATES = 3,
    MSTAT_STATE_BITS = 4,
    MSTAT_ARRAY_STATES = 5,
    MSTAT_ARRAY_READS = 6,
    MSTAT_ARRAY_WRITES = 7,
    MSTAT_AIG_INPUTS = 8,
    MSTAT_AIG_LATCHES = 9,
    MSTAT_AIG_ANDS = 10,
    MSTAT_ARRAY_SELECTORS = 11,
    MSTAT_ARRAY_SLOTS = 12,
};

}  // namespace prof
}  // namespace car
