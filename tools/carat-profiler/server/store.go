package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

// On-disk layout (docs/profiler_protocol.md):
//   data/<run_id>/metadata.json
//   data/<run_id>/events.cprof   (raw frames, readable by cprof_dump.py)
//   data/<run_id>/summary.json

type store struct{ root string }

func newStore(root string) (*store, error) {
	if err := os.MkdirAll(root, 0o755); err != nil {
		return nil, err
	}
	return &store{root: root}, nil
}

func (s *store) runDir(runID string) string {
	// run_id comes from Carat itself (timestamp-pid); sanitize defensively.
	safe := make([]rune, 0, len(runID))
	for _, r := range runID {
		if r == '-' || r == '_' || (r >= '0' && r <= '9') ||
			(r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') {
			safe = append(safe, r)
		}
	}
	return filepath.Join(s.root, string(safe))
}

func (s *store) appendMeta(runID string, meta map[string]any) error {
	dir := s.runDir(runID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	b, err := json.MarshalIndent(meta, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "metadata.json"), b, 0o644)
}

func (s *store) openEvents(runID string) (*os.File, error) {
	dir := s.runDir(runID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	return os.OpenFile(filepath.Join(dir, "events.cprof"),
		os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
}

func (s *store) writeSummary(runID string, summary any) error {
	b, err := json.MarshalIndent(summary, "", "  ")
	if err != nil {
		return err
	}
	tmp := filepath.Join(s.runDir(runID), "summary.json.tmp")
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(s.runDir(runID), "summary.json"))
}

// listRuns returns metadata of persisted runs (for history after restart).
func (s *store) listRuns() []map[string]any {
	entries, err := os.ReadDir(s.root)
	if err != nil {
		return nil
	}
	var out []map[string]any
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		for _, name := range []string{"summary.json", "metadata.json"} {
			b, err := os.ReadFile(filepath.Join(s.root, e.Name(), name))
			if err != nil {
				continue
			}
			var m map[string]any
			if json.Unmarshal(b, &m) == nil {
				m["id"] = e.Name()
				out = append(out, m)
				break
			}
		}
	}
	return out
}
