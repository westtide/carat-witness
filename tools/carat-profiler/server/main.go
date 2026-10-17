// carat-profiler: standalone daemon for the Carat live profiler.
//
// Usage:
//   carat-profiler serve --socket /tmp/carat-prof.sock --http 127.0.0.1:8765
//
// Then run Carat with:
//   carat model.btor2 -a ic3 --trace-socket /tmp/carat-prof.sock
package main

import (
	"embed"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"sort"
	"sync"
	"time"
)

//go:embed web/index.html
var webFS embed.FS

type server struct {
	mu       sync.RWMutex
	runs     map[string]*Run
	monitors map[string]*procMonitor
	st       *store
}

func main() {
	fs := flag.NewFlagSet("carat-profiler", flag.ExitOnError)
	sockPath := fs.String("socket", "/tmp/carat-prof.sock", "unix socket path")
	httpAddr := fs.String("http", "127.0.0.1:8765", "HTTP listen address")
	dataDir := fs.String("data", "data", "directory for run history")
	args := os.Args[1:]
	if len(args) > 0 && args[0] == "serve" {
		args = args[1:] // optional subcommand from the docs
	}
	if err := fs.Parse(args); err != nil {
		log.Fatal(err)
	}

	st, err := newStore(*dataDir)
	if err != nil {
		log.Fatalf("data dir: %v", err)
	}
	s := &server{runs: map[string]*Run{}, monitors: map[string]*procMonitor{}, st: st}

	os.Remove(*sockPath)
	ln, err := net.Listen("unix", *sockPath)
	if err != nil {
		log.Fatalf("listen %s: %v", *sockPath, err)
	}
	log.Printf("carat-profiler listening on %s (http://%s, data: %s)",
		*sockPath, *httpAddr, *dataDir)

	go s.acceptLoop(ln)

	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handleIndex)
	mux.HandleFunc("/api/runs", s.handleRuns)
	mux.HandleFunc("/api/run/", s.handleRunAPI)
	mux.HandleFunc("/ws", s.handleWS)
	log.Fatal(http.ListenAndServe(*httpAddr, mux))
}

func (s *server) acceptLoop(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		go s.handleConn(conn)
	}
}

func (s *server) handleConn(conn net.Conn) {
	defer conn.Close()
	// First frame must be the handshake.
	hdr, payload, err := readFrame(conn)
	if err != nil || hdr.MsgType != ftHandshake {
		log.Printf("connection without handshake: %v", err)
		return
	}
	var meta map[string]any
	if err := json.Unmarshal(payload, &meta); err != nil {
		log.Printf("bad handshake json: %v", err)
		return
	}
	runID, _ := meta["run_id"].(string)
	if runID == "" {
		runID = fmt.Sprintf("unknown-%d", time.Now().UnixNano())
	}

	run, eventsFile := s.attachRun(runID, meta)
	defer s.detachRun(runID)

	if eventsFile != nil {
		defer eventsFile.Close()
		writeRawFrame(eventsFile, hdr, payload)
	}

	log.Printf("run %s attached (pid=%v algo=%v)", runID, meta["pid"], meta["algorithm"])

	for {
		hdr, payload, err := readFrame(conn)
		if err != nil {
			if err != io.EOF {
				log.Printf("run %s read: %v", runID, err)
			}
			s.markDisconnected(runID)
			return
		}
		if eventsFile != nil {
			writeRawFrame(eventsFile, hdr, payload)
		}
		switch hdr.MsgType {
		case ftEventBatch:
			recs, err := decodeRecords(payload)
			if err != nil {
				log.Printf("run %s decode: %v", runID, err)
				continue
			}
			run.Apply(recs)
		case ftEndOfStream:
			run.Mu.Lock()
			if run.State == "running" {
				run.State = "finished"
			}
			run.Mu.Unlock()
			s.persistSummary(runID)
			log.Printf("run %s end of stream", runID)
			return
		}
	}
}

func (s *server) attachRun(runID string, meta map[string]any) (*Run, *os.File) {
	s.mu.Lock()
	defer s.mu.Unlock()
	run := s.runs[runID]
	if run == nil {
		run = NewRun(runID)
		s.runs[runID] = run
		if alg, ok := meta["algorithm"].(string); ok {
			run.Algorithm = alg
		}
		if m, ok := meta["model_path"].(string); ok {
			run.ModelPath = m
		}
		if b, ok := meta["sat_backend"].(string); ok {
			run.SatBackend = b
		}
		if c, ok := meta["carat_commit"].(string); ok {
			run.Commit = c
		}
		if l, ok := meta["trace_level"].(string); ok {
			run.TraceLevel = l
		}
		if pid, ok := meta["pid"].(float64); ok {
			run.PID = int(pid)
		}
		run.StartedAt = time.Now().Unix()
		if mon := newProcMonitor(run.PID); mon != nil {
			s.monitors[runID] = mon
		}
		_ = s.st.appendMeta(runID, meta)
	}
	eventsFile, err := s.st.openEvents(runID)
	if err != nil {
		log.Printf("run %s events file: %v", runID, err)
		return run, nil
	}
	return run, eventsFile
}

func (s *server) detachRun(runID string) {
	s.mu.Lock()
	mon := s.monitors[runID]
	delete(s.monitors, runID)
	run := s.runs[runID]
	s.mu.Unlock()
	if run != nil && mon != nil {
		run.Mu.Lock()
		run.LastProc = mon.current() // freeze the final CPU/RSS sample
		run.Mu.Unlock()
	}
	if mon != nil {
		mon.close()
	}
}

func (s *server) markDisconnected(runID string) {
	s.mu.RLock()
	run := s.runs[runID]
	s.mu.RUnlock()
	if run == nil {
		return
	}
	run.Mu.Lock()
	if run.State == "running" {
		run.State = "disconnected"
	}
	run.Mu.Unlock()
	s.persistSummary(runID)
	log.Printf("run %s disconnected", runID)
}

func (s *server) persistSummary(runID string) {
	s.mu.RLock()
	run := s.runs[runID]
	mon := s.monitors[runID]
	s.mu.RUnlock()
	if run == nil {
		return
	}
	snap := buildSnapshot(run, mon)
	if err := s.st.writeSummary(runID, snap); err != nil {
		log.Printf("run %s summary: %v", runID, err)
	}
}

// writeRawFrame re-serializes header+payload so the .cprof file stays
// parseable by cprof_dump.py.
func writeRawFrame(w io.Writer, hdr frameHeader, payload []byte) {
	var h [16]byte
	h[0], h[1], h[2], h[3] = 'C', 'R', 'P', 'F'
	put16(h[4:], hdr.Version)
	put16(h[6:], hdr.MsgType)
	put32(h[8:], hdr.Length)
	put32(h[12:], hdr.Count)
	_, _ = w.Write(h[:])
	if len(payload) > 0 {
		_, _ = w.Write(payload)
	}
}

func put16(b []byte, v uint16) { b[0] = byte(v); b[1] = byte(v >> 8) }
func put32(b []byte, v uint32) {
	b[0] = byte(v)
	b[1] = byte(v >> 8)
	b[2] = byte(v >> 16)
	b[3] = byte(v >> 24)
}

// ---------- HTTP handlers ----------

func (s *server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	b, err := webFS.ReadFile("web/index.html")
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(b)
}

func (s *server) handleRuns(w http.ResponseWriter, r *http.Request) {
	type row struct {
		ID        string `json:"id"`
		Algorithm string `json:"algorithm"`
		Model     string `json:"model"`
		State     string `json:"state"`
		Result    *uint64 `json:"result_code"`
		StartedAt int64  `json:"started_at"`
		ElapsedMs uint64 `json:"elapsed_ms"`
	}
	s.mu.RLock()
	rows := []row{}
	for id, run := range s.runs {
		run.Mu.Lock()
		rows = append(rows, row{
			ID: id, Algorithm: run.Algorithm, Model: run.ModelPath,
			State: run.State, Result: run.ResultCode,
			StartedAt: run.StartedAt, ElapsedMs: run.lastEventNs / 1e6,
		})
		run.Mu.Unlock()
	}
	s.mu.RUnlock()
	for _, m := range s.st.listRuns() {
		if id, _ := m["id"].(string); id != "" {
			if _, live := s.runs[id]; !live {
				rows = append(rows, row{
					ID: id, Algorithm: str(m, "algorithm"),
					Model: str(m, "model_path"), State: "archived",
					StartedAt: 0,
				})
			}
		}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"runs": rows})
}

func str(m map[string]any, k string) string {
	v, _ := m[k].(string)
	return v
}

func (s *server) handleRunAPI(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Path[len("/api/run/"):]
	// optional suffix /summary
	if n := len(id) - len("/summary"); n >= 0 && id[n:] == "/summary" {
		id = id[:n]
	}
	s.mu.RLock()
	run := s.runs[id]
	mon := s.monitors[id]
	s.mu.RUnlock()
	if run == nil {
		http.Error(w, "unknown run", 404)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(buildSnapshot(run, mon))
}

func (s *server) handleWS(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")
	s.mu.RLock()
	run := s.runs[id]
	mon := s.monitors[id]
	s.mu.RUnlock()
	if run == nil {
		http.Error(w, "unknown run", 404)
		return
	}
	ws, err := upgradeWS(w, r)
	if err != nil {
		return
	}
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for range tick.C {
		b, err := json.Marshal(buildSnapshot(run, mon))
		if err != nil {
			return
		}
		if err := ws.writeText(b); err != nil {
			return
		}
		// Stop pushing once the run is finished and one last snapshot went out.
		run.Mu.Lock()
		done := run.State != "running"
		run.Mu.Unlock()
		if done {
			return
		}
	}
}

// ---------- snapshot ----------

func buildSnapshot(run *Run, mon *procMonitor) map[string]any {
	run.Mu.Lock()
	defer run.Mu.Unlock()

	runInfo := map[string]any{
		"id": run.ID, "state": run.State, "algorithm": run.Algorithm,
		"model": run.ModelPath, "sat_backend": run.SatBackend,
		"commit": run.Commit, "trace_level": run.TraceLevel,
		"elapsed_ms": run.lastEventNs / 1e6, "dropped": run.dropped,
		"events": run.totalEvents,
	}
	if run.ResultCode != nil {
		runInfo["result"] = map[uint64]string{0: "Unknown", 1: "Safe", 2: "Unsafe"}[*run.ResultCode]
		runInfo["result_code"] = *run.ResultCode
	}

	stages := []map[string]any{}
	for _, st := range run.stageStats {
		stages = append(stages, map[string]any{
			"stage": st.Stage, "name": stageName(st.Stage),
			"count": st.Count, "wall_ms": st.WallNs / 1e6,
			"min_ms": st.MinNs / 1e6, "max_ms": st.MaxNs / 1e6,
		})
	}
	sort.Slice(stages, func(i, j int) bool {
		return num(stages[i], "wall_ms") > num(stages[j], "wall_ms")
	})

	p50, p95, p99 := run.Percentiles()
	sat := map[string]any{
		"p50_ms": p50, "p95_ms": p95, "p99_ms": p99,
		"active": run.activeSAT,
	}
	totalQ := uint64(0)
	totalUs := uint64(0)
	byCat := []map[string]any{}
	for _, cs := range run.satByCat {
		byCat = append(byCat, map[string]any{
			"cat": cs.Cat, "name": satCatName(cs.Cat), "count": cs.Count,
			"total_ms": float64(cs.TotalUs) / 1000.0,
			"max_ms":   float64(cs.MaxUs) / 1000.0, "unsat": cs.Unsat,
		})
		totalQ += cs.Count
		totalUs += cs.TotalUs
	}
	sort.Slice(byCat, func(i, j int) bool {
		return num(byCat[i], "total_ms") > num(byCat[j], "total_ms")
	})
	sat["queries"] = totalQ
	sat["total_ms"] = float64(totalUs) / 1000.0
	sat["by_category"] = byCat

	counters := map[string]uint64{}
	for k, v := range run.Counters {
		counters[fmt.Sprintf("%#x", k)] = v
	}
	counterSeries := map[string][]seriesPoint{}
	for k, pts := range run.counterSeries {
		if d := downsample(pts); len(d) >= 2 {
			counterSeries[fmt.Sprintf("%#x", k)] = d
		}
	}
	satSeries := map[string]any{}
	for k, pts := range run.satSeries {
		if d := downsample(pts); len(d) >= 2 {
			satSeries[satCatName(k)] = d
		}
	}
	modelStats := map[string]any{}
	for k, v := range run.ModelStats {
		modelStats[fmt.Sprintf("%d", k)] = v
	}

	var proc any
	if mon != nil {
		proc = mon.current()
	} else if run.LastProc.Sampled {
		proc = run.LastProc // finished run keeps its last reading
	} else {
		proc = procSample{}
	}

	iters := make([]iterRow, len(run.iterations))
	copy(iters, run.iterations)

	timeline := make([]spanRow, 0, len(run.recent)+len(run.spans))
	timeline = append(timeline, run.recent...)
	nowMs := run.lastEventNs / 1e6
	for id, s := range run.spans {
		timeline = append(timeline, spanRow{
			Span: id, Parent: s.parent, Stage: s.stage,
			Name: stageName(s.stage), Begin: s.begin / 1e6,
			End: nowMs, Thread: s.thread, Ctx: s.context,
			Open: true,
		})
	}

	return map[string]any{
		"run": runInfo,
		"stages": stages,
		"current": map[string]any{
			"stage_path": run.CurrentStagePath(),
			"open_spans": len(run.spans),
		},
		"sat": sat,
		"counters": counters,
		"counter_series": counterSeries,
		"sat_series": satSeries,
		"model_stats": modelStats,
		"iterations": iters,
		"timeline": timeline,
		"process": proc,
	}
}

func num(m map[string]any, k string) float64 {
	v, _ := m[k].(float64)
	if u, ok := m[k].(uint64); ok {
		return float64(u)
	}
	return v
}
