package main

// Periodic CPU/RSS sampling of the profiled Carat process via `ps`
// (works on both macOS and Linux).

import (
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

type procSample struct {
	CPUPct  float64 `json:"cpu_pct"`
	RSSMB   float64 `json:"rss_mb"`
	PeakMB  float64 `json:"peak_rss_mb"`
	Sampled bool    `json:"sampled"`
}

type procMonitor struct {
	mu      sync.Mutex
	sample  procSample
	pid     int
	stop    chan struct{}
	stopped sync.WaitGroup
}

func newProcMonitor(pid int) *procMonitor {
	m := &procMonitor{pid: pid, stop: make(chan struct{})}
	if pid > 0 {
		m.stopped.Add(1)
		go m.loop()
	}
	return m
}

func (m *procMonitor) loop() {
	defer m.stopped.Done()
	tick := time.NewTicker(1 * time.Second)
	defer tick.Stop()
	for {
		select {
		case <-m.stop:
			return
		case <-tick.C:
			if s, ok := m.sampleOnce(); ok {
				m.mu.Lock()
				s.PeakMB = max64(s.RSSMB, m.sample.PeakMB)
				m.sample = s
				m.mu.Unlock()
			}
		}
	}
}

func (m *procMonitor) current() procSample {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.sample
}

func (m *procMonitor) close() {
	if m.pid > 0 {
		close(m.stop)
		m.stopped.Wait()
	}
}

func (m *procMonitor) sampleOnce() (procSample, bool) {
	out, err := exec.Command("/bin/ps", "-o", "pcpu=,rss=", "-p",
		strconv.Itoa(m.pid)).Output()
	if err != nil {
		return procSample{}, false
	}
	fields := strings.Fields(strings.TrimSpace(string(out)))
	if len(fields) != 2 {
		return procSample{}, false
	}
	cpu, err1 := strconv.ParseFloat(fields[0], 64)
	rssKb, err2 := strconv.ParseFloat(fields[1], 64)
	if err1 != nil || err2 != nil {
		return procSample{}, false
	}
	return procSample{CPUPct: cpu, RSSMB: rssKb / 1024.0, Sampled: true}, true
}

func max64(a, b float64) float64 {
	if a > b {
		return a
	}
	return b
}
