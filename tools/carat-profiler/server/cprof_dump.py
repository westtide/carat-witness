#!/usr/bin/env python3
"""Decode and summarize a Carat profiler binary trace (docs/profiler_protocol.md v1).

Usage: cprof_dump.py <trace.cprof> [--events]
"""
import json
import struct
import sys

MAGIC = b"CRPF"
HDR = struct.Struct("<4sHHII")
REC = struct.Struct("<QQQQQQIHHII")  # 64 bytes

STAGE_NAMES = {
    0: "NONE", 101: "MODEL_LOAD", 102: "MODEL_COI", 103: "MODEL_ARRAY_SIMPLIFY",
    104: "MODEL_PACKAGE_RESIZE", 105: "MODEL_BITBLAST", 106: "MODEL_INIT_CHECKER",
    201: "CEGAR_ROUND", 202: "CEGAR_CHECK", 203: "CEGAR_CEX_ANALYZE",
    204: "CEGAR_REFINE", 205: "CEGAR_PREFIX_BMC",
    301: "IC3_INIT", 302: "IC3_EXTEND", 303: "IC3_CTI", 304: "IC3_BLOCK",
    305: "IC3_GENERALIZE", 306: "IC3_PROPAGATE",
    401: "FCAR_INIT", 402: "FCAR_START_ENUM", 403: "FCAR_SEARCH", 404: "FCAR_LEARN",
    405: "FCAR_PROPAGATE", 406: "FCAR_INVARIANT",
    501: "BCAR_INIT", 502: "BCAR_CHECK_BAD", 503: "BCAR_START_ENUM",
    504: "BCAR_SEARCH", 505: "BCAR_LEARN", 506: "BCAR_PROPAGATE",
    507: "BCAR_INVARIANT", 508: "BCAR_REFINE",
    601: "KIND_INIT", 602: "KIND_UNROLL", 603: "KIND_BASE", 604: "KIND_IND",
    605: "KIND_SIMPLEPATH",
}
EVENT_NAMES = {
    1: "RUN_START", 2: "RUN_END", 3: "PHASE_BEGIN", 4: "PHASE_END",
    5: "ITERATION_BEGIN", 6: "ITERATION_END", 7: "SAT_BEGIN", 8: "SAT_END",
    9: "COUNTER_UPDATE", 10: "MODEL_STAT", 11: "ALGORITHM_SNAPSHOT",
    12: "HEARTBEAT", 13: "EVENTS_DROPPED", 14: "SAT_AGGREGATE",
}
SAT_CATS = {
    0: "other", 1: "init", 2: "start", 3: "lift", 4: "block",
    5: "generalize", 6: "propagate", 7: "invariant", 8: "base",
    9: "ind", 10: "refine",
}


def iter_frames(path):
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    while off + HDR.size <= len(data):
        magic, ver, msg, plen, count = HDR.unpack_from(data, off)
        if magic != MAGIC:
            raise ValueError(f"bad magic at offset {off}: {magic!r}")
        if ver != 1:
            raise ValueError(f"unsupported protocol version {ver} at offset {off}")
        off += HDR.size
        payload = data[off:off + plen]
        if len(payload) != plen:
            raise ValueError("truncated frame payload")
        off += plen
        yield msg, payload, count


def iter_records(payload):
    for i in range(len(payload) // REC.size):
        yield REC.unpack_from(payload, i * REC.size)


def summarize(path):
    meta = None
    spans = {}          # span_id -> [stage, begin_ns]
    stage_stats = {}    # stage -> [count, wall_ns_total]
    sat_calls = 0
    sat_ns = 0
    aggregates = {}    # cat -> [count, total_ms]  (summary-level reports)
    counters = {}
    model_stats = {}
    dropped = 0
    n_events = 0
    run_end = None
    heartbeat_active_max_ns = 0
    for msg, payload, count in iter_frames(path):
        if msg == 1:
            meta = json.loads(payload.decode("utf-8"))
        elif msg == 2:
            for (ts, seq, span, parent, v0, v1, tid, etype, stage, ctx,
                 flags) in iter_records(payload):
                n_events += 1
                if etype == 3:
                    spans[span] = (stage, ts)
                elif etype == 4:
                    if span in spans:
                        s, b = spans.pop(span)
                        st = stage_stats.setdefault(s, [0, 0])
                        st[0] += 1
                        st[1] += ts - b
                elif etype == 7:
                    sat_calls += 1
                elif etype == 8:
                    sat_ns += v1
                elif etype == 9:
                    counters[v0] = v1
                elif etype == 14:
                    if flags & 1:
                        if v0 in aggregates:
                            aggregates[v0][2] = v1 / 1000.0  # max us -> ms
                    else:
                        cnt = v1 >> 34
                        us = v1 & 0x3FFFFFFFF
                        aggregates.setdefault(v0, [0, 0, 0])
                        aggregates[v0][0] = cnt
                        aggregates[v0][1] = us / 1000.0
                elif etype == 10:
                    model_stats[v0] = v1
                elif etype == 12:
                    if span and v1 > heartbeat_active_max_ns:
                        heartbeat_active_max_ns = v1
                elif etype == 13:
                    dropped = max(dropped, v0)
                elif etype == 2:
                    run_end = (v0, v1)
    print(f"== {path}")
    if meta:
        print(f"  run_id={meta.get('run_id')} algorithm={meta.get('algorithm')} "
              f"backend={meta.get('sat_backend')} level={meta.get('trace_level')} "
              f"commit={meta.get('carat_commit')}")
    print(f"  events={n_events} sat_calls(detail)={sat_calls} "
          f"sat_total_ms(detail)={sat_ns/1e6:.1f} dropped={dropped} "
          f"open_spans={len(spans)}")
    for cat, (cnt, ms, mx) in sorted(aggregates.items()):
        if cnt:
            print(f"  sat[{SAT_CATS.get(cat, cat):11s}] count={cnt:6d} "
                  f"total_ms={ms} max_ms={mx}")
    print(f"  longest_active_sat_ms(seen_in_heartbeat)={heartbeat_active_max_ns/1e6:.1f}")
    if run_end:
        print(f"  result_code={run_end[0]} termination={run_end[1]}")
    for stage in sorted(stage_stats):
        cnt, ns = stage_stats[stage]
        print(f"  stage {STAGE_NAMES.get(stage, stage):22s} calls={cnt:6d} wall_ms={ns/1e6:9.2f}")
    if model_stats:
        print("  model_stats:", {k: v for k, v in sorted(model_stats.items())})
    if counters:
        print("  counters:", {hex(k): v for k, v in sorted(counters.items())})


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    summarize(sys.argv[1])
