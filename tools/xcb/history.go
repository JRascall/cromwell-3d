// history.go — every build, one JSON line, and the diffing against the past.
//
// History is the memory that makes "builds are getting slower" answerable:
// each finished build appends an Entry, and LastComparable finds the previous
// build of the same spec to diff against. JSONL rather than a database
// because the queries are a scan of at most a few thousand lines, and a
// text file survives every tool that will ever want to read it.
package main

import (
	"bufio"
	"encoding/json"
	"os"
	"strings"
	"time"
)

type Entry struct {
	TS      string         `json:"ts"`
	Targets []string       `json:"targets"`
	Config  string         `json:"config"`
	WallMs  int64          `json:"wall_ms"`
	OK      bool           `json:"ok"`
	Files   int            `json:"files"`
	Project map[string]int `json:"projects,omitempty"`
	ClMs    int            `json:"cl_ms,omitempty"`
	LinkMs  int            `json:"link_ms,omitempty"`
}

func (e Entry) Key() string {
	return strings.Join(e.Targets, "+") + "/" + e.Config
}

func (e Entry) Wall() time.Duration {
	return time.Duration(e.WallMs) * time.Millisecond
}

type History struct {
	path string
}

func NewHistory(path string) *History { return &History{path: path} }

func (h *History) Append(result *BuildResult) error {
	entry := Entry{
		TS:      time.Now().Format(time.RFC3339),
		Targets: result.Spec.Targets,
		Config:  result.Spec.Config,
		WallMs:  result.Wall.Milliseconds(),
		OK:      result.OK,
		Files:   result.Files,
		ClMs:    result.Summary.TaskMs("CL"),
		LinkMs:  result.Summary.TaskMs("Link"),
	}
	if len(result.Summary.Projects) > 0 {
		entry.Project = map[string]int{}
		for _, p := range result.Summary.Projects {
			entry.Project[p.Name] = p.Ms
		}
	}
	data, err := json.Marshal(entry)
	if err != nil {
		return err
	}
	f, err := os.OpenFile(h.path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = f.Write(append(data, '\n'))
	return err
}

func (h *History) Entries() ([]Entry, error) {
	f, err := os.Open(h.path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	defer f.Close()

	var entries []Entry
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		var e Entry
		if json.Unmarshal(scanner.Bytes(), &e) == nil {
			entries = append(entries, e)
		}
	}
	return entries, scanner.Err()
}

// LastComparable finds the previous build that measured the same thing:
// same spec key, and it succeeded — a failed build's wall time measures
// where it died, not how long building takes.
func (h *History) LastComparable(spec BuildSpec) *Entry {
	entries, err := h.Entries()
	if err != nil {
		return nil
	}
	for i := len(entries) - 1; i >= 0; i-- {
		if entries[i].OK && entries[i].Key() == spec.Key() {
			return &entries[i]
		}
	}
	return nil
}

// Recent returns the last n entries, oldest first.
func (h *History) Recent(n int) ([]Entry, error) {
	entries, err := h.Entries()
	if err != nil {
		return nil, err
	}
	if len(entries) > n {
		entries = entries[len(entries)-n:]
	}
	return entries, nil
}
