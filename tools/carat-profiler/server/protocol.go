package main

// Wire protocol v1 (docs/profiler_protocol.md). Keep in sync with
// src/profiler/Protocol.h.

import (
	"encoding/binary"
	"errors"
	"io"
)

const (
	protoVersion = 1
	frameMagic   = 0x46505243 // "CRPF" little-endian
)

const (
	ftHandshake   = 1
	ftEventBatch  = 2
	ftEndOfStream = 3
)

// Event types.
const (
	evRunStart = 1
	evRunEnd   = 2
	// PHASE_BEGIN=3 PHASE_END=4 ITERATION_BEGIN=5 ITERATION_END=6
	evSATBegin = 7
	evSATEnd   = 8
	// COUNTER_UPDATE=9 MODEL_STAT=10 ALGORITHM_SNAPSHOT=11
	evHeartbeat     = 12
	evEventsDropped = 13
	evSATAggregate  = 14
)

// EventRecord mirrors the 64-byte C++ struct.
type EventRecord struct {
	TimestampNs uint64
	Sequence    uint64
	SpanID      uint64
	ParentSpan  uint64
	Value0      uint64
	Value1      uint64
	ThreadID    uint32
	EventType   uint16
	StageID     uint16
	ContextID   uint32
	Flags       uint32
}

const recordSize = 64

func decodeRecords(payload []byte) ([]EventRecord, error) {
	if len(payload)%recordSize != 0 {
		return nil, errors.New("payload not a multiple of 64 bytes")
	}
	out := make([]EventRecord, len(payload)/recordSize)
	for i := range out {
		p := payload[i*recordSize:]
		r := &out[i]
		r.TimestampNs = binary.LittleEndian.Uint64(p[0:])
		r.Sequence = binary.LittleEndian.Uint64(p[8:])
		r.SpanID = binary.LittleEndian.Uint64(p[16:])
		r.ParentSpan = binary.LittleEndian.Uint64(p[24:])
		r.Value0 = binary.LittleEndian.Uint64(p[32:])
		r.Value1 = binary.LittleEndian.Uint64(p[40:])
		r.ThreadID = binary.LittleEndian.Uint32(p[48:])
		r.EventType = binary.LittleEndian.Uint16(p[52:])
		r.StageID = binary.LittleEndian.Uint16(p[54:])
		r.ContextID = binary.LittleEndian.Uint32(p[56:])
		r.Flags = binary.LittleEndian.Uint32(p[60:])
	}
	return out, nil
}

// frameHeader is the 16-byte stream frame header.
type frameHeader struct {
	Version uint16
	MsgType uint16
	Length  uint32
	Count   uint32
}

func readFrame(r io.Reader) (frameHeader, []byte, error) {
	var hdr [16]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return frameHeader{}, nil, err
	}
	if binary.LittleEndian.Uint32(hdr[0:]) != frameMagic {
		return frameHeader{}, nil, errors.New("bad frame magic")
	}
	h := frameHeader{
		Version: binary.LittleEndian.Uint16(hdr[4:]),
		MsgType: binary.LittleEndian.Uint16(hdr[6:]),
		Length:  binary.LittleEndian.Uint32(hdr[8:]),
		Count:   binary.LittleEndian.Uint32(hdr[12:]),
	}
	if h.Version != protoVersion {
		return h, nil, errors.New("unsupported protocol version")
	}
	if h.Length > 64*1024*1024 {
		return h, nil, errors.New("frame too large")
	}
	payload := make([]byte, h.Length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return h, nil, err
	}
	return h, payload, nil
}

var stageNames = map[uint16]string{
	0: "NONE",
	101: "MODEL_LOAD", 102: "MODEL_COI", 103: "MODEL_ARRAY_SIMPLIFY",
	104: "MODEL_PACKAGE_RESIZE", 105: "MODEL_BITBLAST", 106: "MODEL_INIT_CHECKER",
	201: "CEGAR_ROUND", 202: "CEGAR_CHECK", 203: "CEGAR_CEX_ANALYZE",
	204: "CEGAR_REFINE", 205: "CEGAR_PREFIX_BMC",
	301: "IC3_INIT", 302: "IC3_EXTEND", 303: "IC3_CTI", 304: "IC3_BLOCK",
	305: "IC3_GENERALIZE", 306: "IC3_PROPAGATE",
	401: "FCAR_INIT", 402: "FCAR_START_ENUM", 403: "FCAR_SEARCH",
	404: "FCAR_LEARN", 405: "FCAR_PROPAGATE", 406: "FCAR_INVARIANT",
	501: "BCAR_INIT", 502: "BCAR_CHECK_BAD", 503: "BCAR_START_ENUM",
	504: "BCAR_SEARCH", 505: "BCAR_LEARN", 506: "BCAR_PROPAGATE",
	507: "BCAR_INVARIANT", 508: "BCAR_REFINE",
	601: "KIND_INIT", 602: "KIND_UNROLL", 603: "KIND_BASE", 604: "KIND_IND",
	605: "KIND_SIMPLEPATH",
}

func stageName(id uint16) string {
	if n, ok := stageNames[id]; ok {
		return n
	}
	return "STAGE_" + uitoa(uint64(id))
}

var satCatNames = map[uint64]string{
	0: "other", 1: "init", 2: "start", 3: "lift", 4: "block",
	5: "generalize", 6: "propagate", 7: "invariant", 8: "base", 9: "ind",
	10: "refine",
}

func satCatName(c uint64) string {
	if n, ok := satCatNames[c]; ok {
		return n
	}
	return "cat" + uitoa(c)
}

func uitoa(v uint64) string {
	if v == 0 {
		return "0"
	}
	var buf [20]byte
	i := len(buf)
	for v > 0 {
		i--
		buf[i] = byte('0' + v%10)
		v /= 10
	}
	return string(buf[i:])
}
