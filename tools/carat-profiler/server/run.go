package main

import (
	"encoding/binary"
	"sort"
	"sync"
)

// Aggregated state of one profiler run (one Carat process connection).
type Run struct {
	Mu sync.Mutex `json:"-"`

	ID          string
	Algorithm   string
	ModelPath   string
	SatBackend  string
	Commit      string
	TraceLevel  string
	PID         int
	StartedAt   int64 // unix seconds
	State       string // running | finished | disconnected
	ResultCode  *uint64
	Termination uint64

	// Span aggregation.
	spans      map[uint64]*openSpan // open PHASE spans by span id
	stageStats map[uint16]*stageStat
	recent     []spanRow // ring of recent closed spans for the timeline
	recentMax  int

	// SAT analytics.
	spanCat   map[uint64]uint64 // SAT span id -> category (detail mode)
	satByCat  map[uint64]*catStat
	activeSAT []activeQuery
	satSeq    []float64 // recent per-query durations (ms) for percentiles
	satMax    int

	// Progress.
	Counters     map[uint64]uint64
	counterSeries map[uint64][]seriesPoint // capped raw history per counter
	satSeries     map[uint64][]seriesPoint // cumulative SAT µs per category
	ModelStats  map[uint64]uint64
	iterations  []iterRow
	lastEventNs uint64
	dropped     uint64
	totalEvents uint64
	LastProc    procSample `json:"-"` // frozen when the process monitor closes
}

type seriesPoint struct {
	T uint64 `json:"t"` // ms since run start
	V uint64 `json:"v"`
}

const (
	kMaxRawPoints    = 4096 // per-series raw cap before oldest are dropped
	kPushPoints      = 240  // points per series in a snapshot
)

type openSpan struct {
	stage   uint16
	parent  uint64
	begin   uint64
	thread  uint32
	context uint32
}

type spanRow struct {
	Span   uint64 `json:"span"`
	Parent uint64 `json:"parent"`
	Stage  uint16 `json:"stage"`
	Name   string `json:"name"`
	Begin  uint64 `json:"begin_ms"`
	End    uint64 `json:"end_ms"`
	Thread uint32 `json:"thread"`
	Ctx    uint32 `json:"ctx"`
	Open   bool   `json:"open,omitempty"`
}

type stageStat struct {
	Stage  uint16
	Count  uint64
	WallNs uint64
	MinNs  uint64
	MaxNs  uint64
}

type catStat struct {
	Cat     uint64
	Count   uint64
	TotalUs uint64
	MaxUs   uint64
	Unsat   uint64
}

type activeQuery struct {
	Span       uint64 `json:"span"`
	Cat        uint64 `json:"cat"`
	CatName    string `json:"cat_name"`
	Stage      uint16 `json:"stage"`
	StageName  string `json:"stage_name"`
	StartedAgo uint64 `json:"started_ms_ago"`
	ThreadID   uint32 `json:"thread"`
}

type iterRow struct {
	Iter      uint32  `json:"iter"`
	BeginMs   uint64  `json:"begin_ms"`
	EndMs     *uint64 `json:"end_ms"`
	Converged bool    `json:"converged"`
}

func NewRun(id string) *Run {
	return &Run{
		ID:         id,
		State:      "running",
		spans:      map[uint64]*openSpan{},
		stageStats: map[uint16]*stageStat{},
		recentMax:  4000,
		spanCat:    map[uint64]uint64{},
		satByCat:   map[uint64]*catStat{},
		satMax:     20000,
		Counters:     map[uint64]uint64{},
		counterSeries: map[uint64][]seriesPoint{},
		satSeries:     map[uint64][]seriesPoint{},
		ModelStats:  map[uint64]uint64{},
	}
}

func (r *Run) Apply(recs []EventRecord) {
	r.Mu.Lock()
	defer r.Mu.Unlock()
	for i := range recs {
		e := &recs[i]
		if e.TimestampNs > r.lastEventNs {
			r.lastEventNs = e.TimestampNs
		}
		r.totalEvents++
		switch e.EventType {
		case 2: // RUN_END
			v := e.Value0
			r.ResultCode = &v
			r.Termination = e.Value1
			r.State = "finished"
		case 3: // PHASE_BEGIN
			r.spans[e.SpanID] = &openSpan{
				stage: e.StageID, parent: e.ParentSpan,
				begin: e.TimestampNs, thread: e.ThreadID, context: e.ContextID,
			}
		case 4: // PHASE_END
			if s, ok := r.spans[e.SpanID]; ok {
				delete(r.spans, e.SpanID)
				dur := e.TimestampNs - s.begin
				st := r.stageStats[s.stage]
				if st == nil {
					st = &stageStat{Stage: s.stage, MinNs: dur}
					r.stageStats[s.stage] = st
				}
				st.Count++
				st.WallNs += dur
				if dur < st.MinNs || st.MinNs == 0 {
					st.MinNs = dur
				}
				if dur > st.MaxNs {
					st.MaxNs = dur
				}
				if len(r.recent) >= r.recentMax {
					copy(r.recent, r.recent[1:])
					r.recent = r.recent[:len(r.recent)-1]
				}
				r.recent = append(r.recent, spanRow{
					Span: e.SpanID, Parent: s.parent, Stage: s.stage,
					Name: stageName(s.stage), Begin: s.begin / 1e6,
					End: e.TimestampNs / 1e6, Thread: s.thread, Ctx: s.context,
				})
			}
		case 5: // ITERATION_BEGIN; Value0 = frame / depth / round
			r.iterations = append(r.iterations, iterRow{
				Iter: uint32(e.Value0), BeginMs: e.TimestampNs / 1e6,
			})
			if len(r.iterations) > 500 {
				r.iterations = r.iterations[len(r.iterations)-500:]
			}
		case 6: // ITERATION_END
			for i := len(r.iterations) - 1; i >= 0; i-- {
				if r.iterations[i].Iter == uint32(e.Value0) &&
					r.iterations[i].EndMs == nil {
					end := e.TimestampNs / 1e6
					r.iterations[i].EndMs = &end
					r.iterations[i].Converged = e.Flags&1 != 0
					break
				}
			}
		case 7: // SAT_BEGIN (detail): value1 packs cat | stage | assumptions
			cat := e.Value1 & 0xFFFF
			r.spanCat[e.SpanID] = cat
			cs := r.catStat(cat)
			cs.Count++
			r.pushActive(activeQuery{
				Span: e.SpanID, Cat: cat, CatName: satCatName(cat),
				Stage: e.StageID, StageName: stageName(e.StageID),
				StartedAgo: 0, ThreadID: e.ThreadID,
			})
		case 8: // SAT_END (detail): value1 = duration ns
			cat := r.spanCat[e.SpanID]
			delete(r.spanCat, e.SpanID)
			cs := r.catStat(cat)
			cs.TotalUs += e.Value1 / 1000
			if e.Value1/1000 > cs.MaxUs {
				cs.MaxUs = e.Value1 / 1000
			}
			r.appendSatPoint(cat, e.TimestampNs/1e6, cs.TotalUs)
			if e.Flags&2 != 0 {
				cs.Unsat++
			}
			r.appendSatSeq(float64(e.Value1) / 1e6)
			r.removeActive(e.SpanID)
		case 9:
			r.Counters[e.Value0] = e.Value1
			r.appendCounterPoint(e.Value0, e.TimestampNs/1e6, e.Value1)
		case 10:
			r.ModelStats[e.Value0] = e.Value1
		case 12: // HEARTBEAT: value0 = the thread's active SAT span (0 = idle).
			// A thread runs at most one query at a time, so each heartbeat
			// fully replaces that thread's active entry.
			if e.SpanID == 0 {
				r.clearActiveOfThread(e.ThreadID)
			} else {
				r.replaceActiveOfThread(e.ThreadID, e.SpanID, e.Value1)
			}
		case 13:
			r.dropped = e.Value0
		case 14: // SAT_AGGREGATE (summary): value1 = count<<34 | total_us
			cs := r.catStat(e.Value0)
			if e.Flags&1 != 0 { // max-sample record (microseconds)
				if e.Value1 > cs.MaxUs {
					cs.MaxUs = e.Value1
				}
			} else {
				cs.Count = e.Value1 >> 34
				cs.TotalUs = e.Value1 & 0x3FFFFFFFF
				r.appendSatPoint(e.Value0, e.TimestampNs/1e6, cs.TotalUs)
			}
		}
	}
}

func (r *Run) appendCounterPoint(id uint64, t, v uint64) {
	pts := r.counterSeries[id]
	pts = append(pts, seriesPoint{t, v})
	if len(pts) > kMaxRawPoints {
		pts = pts[len(pts)-kMaxRawPoints:]
	}
	r.counterSeries[id] = pts
}

func (r *Run) appendSatPoint(cat uint64, t, totalUs uint64) {
	pts := r.satSeries[cat]
	if n := len(pts); n > 0 && pts[n-1].T == t {
		pts[n-1].V = totalUs // collapse same-tick updates
		r.satSeries[cat] = pts
		return
	}
	pts = append(pts, seriesPoint{t, totalUs})
	if len(pts) > kMaxRawPoints {
		pts = pts[len(pts)-kMaxRawPoints:]
	}
	r.satSeries[cat] = pts
}

// downsample keeps the per-bucket last point so cumulative counters keep
// their monotone shape; snapshotted series stay small for 250ms WS pushes.
func downsample(pts []seriesPoint) []seriesPoint {
	if len(pts) <= kPushPoints {
		return pts
	}
	out := make([]seriesPoint, 0, kPushPoints)
	step := float64(len(pts)) / kPushPoints
	for i := 0; i < kPushPoints; i++ {
		hi := int((float64(i) + 1) * step)
		if hi <= int(float64(i)*step) {
			hi = int(float64(i)*step) + 1
		}
		if hi > len(pts) {
			hi = len(pts)
		}
		if lo := int(float64(i) * step); lo < hi {
			out = append(out, pts[hi-1])
		}
	}
	return out
}

func (r *Run) catStat(cat uint64) *catStat {
	cs := r.satByCat[cat]
	if cs == nil {
		cs = &catStat{Cat: cat}
		r.satByCat[cat] = cs
	}
	return cs
}

func (r *Run) appendSatSeq(ms float64) {
	if len(r.satSeq) >= r.satMax {
		copy(r.satSeq, r.satSeq[1:])
		r.satSeq = r.satSeq[:len(r.satSeq)-1]
	}
	r.satSeq = append(r.satSeq, ms)
}

func (r *Run) pushActive(q activeQuery) {
	for i := range r.activeSAT {
		if r.activeSAT[i].Span == q.Span {
			r.activeSAT[i] = q
			return
		}
	}
	r.activeSAT = append(r.activeSAT, q)
}

// replaceActiveOfThread swaps the thread's whole active-query slot; stale
// entries from finished queries disappear even without SAT_END events.
func (r *Run) replaceActiveOfThread(thread uint32, span uint64, durNs uint64) {
	for i := range r.activeSAT {
		if r.activeSAT[i].ThreadID == thread {
			r.activeSAT[i].Span = span
			r.activeSAT[i].StartedAgo = durNs / 1e6
			if r.activeSAT[i].CatName == "" ||
				r.activeSAT[i].CatName == "running" {
				r.activeSAT[i].CatName = "running"
			}
			return
		}
	}
	r.activeSAT = append(r.activeSAT, activeQuery{
		Span: span, CatName: "running", StartedAgo: durNs / 1e6,
		ThreadID: thread,
	})
}

func (r *Run) clearActiveOfThread(thread uint32) {
	out := r.activeSAT[:0]
	for _, q := range r.activeSAT {
		if q.ThreadID != thread {
			out = append(out, q)
		}
	}
	r.activeSAT = out
}

func (r *Run) removeActive(span uint64) {
	for i := range r.activeSAT {
		if r.activeSAT[i].Span == span {
			r.activeSAT = append(r.activeSAT[:i], r.activeSAT[i+1:]...)
			return
		}
	}
}

// Percentiles returns p50/p95/p99 of recent SAT durations (ms).
func (r *Run) Percentiles() (p50, p95, p99 float64) {
	if len(r.satSeq) == 0 {
		return 0, 0, 0
	}
	s := make([]float64, len(r.satSeq))
	copy(s, r.satSeq)
	sort.Float64s(s)
	pick := func(p float64) float64 {
		idx := int(p * float64(len(s)-1))
		return s[idx]
	}
	return pick(0.50), pick(0.95), pick(0.99)
}

// CurrentStagePath walks the innermost leaf span up through its parents.
func (r *Run) CurrentStagePath() []string {
	leaf := map[uint64]bool{}
	for id := range r.spans {
		leaf[id] = true
	}
	for _, s := range r.spans {
		delete(leaf, s.parent)
	}
	var bestID uint64
	var best *openSpan
	for id, isLeaf := range leaf {
		if !isLeaf {
			continue
		}
		s := r.spans[id]
		if best == nil || s.begin > best.begin {
			best, bestID = s, id
		}
	}
	var path []string
	for cur := bestID; cur != 0; {
		s, ok := r.spans[cur]
		if !ok {
			break
		}
		path = append([]string{stageName(s.stage)}, path...)
		cur = s.parent
	}
	return path
}

func be64(v uint64) []byte {
	var b [8]byte
	binary.LittleEndian.PutUint64(b[:], v)
	return b[:]
}
