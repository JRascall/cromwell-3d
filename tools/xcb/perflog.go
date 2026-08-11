// perflog.go — the numbers MSBuild measured, parsed and nothing else.
//
// THE BREAKDOWN IS MSBUILD'S OWN, not something scraped from timestamps.
// /flp:PerformanceSummary has MSBuild write per-project and per-task
// milliseconds into a file logger at the end of the build; parsing that is
// the difference between reporting what the build system measured and
// guessing from when lines happened to reach a pipe. The per-task summary is
// what splits compile (CL) from link (Link/Lib), which is the first question
// about any slow incremental build.
package main

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

type TimedItem struct {
	Name string
	Ms   int
}

type PerfSummary struct {
	Projects []TimedItem // per .vcxproj, largest first
	Tasks    []TimedItem // CL / Link / Lib / ... build-wide
}

func (s PerfSummary) TaskMs(name string) int {
	for _, t := range s.Tasks {
		if t.Name == name {
			return t.Ms
		}
	}
	return 0
}

func (s PerfSummary) TotalProjectMs() int {
	total := 0
	for _, p := range s.Projects {
		total += p.Ms
	}
	return total
}

// One row of either summary: "   1234 ms  <name>   2 calls".
var perfLine = regexp.MustCompile(`^\s*(\d+) ms\s+(.+?)\s+(\d+) calls\s*$`)

// A compiler echoing the file it is on — the lines counted as build volume.
var sourceEcho = regexp.MustCompile(`^\s+[\w.-]+\.(cpp|cc|c)$`)

func isSourceEcho(line string) bool { return sourceEcho.MatchString(line) }

func parsePerfSummary(path string) PerfSummary {
	var summary PerfSummary
	data, err := os.ReadFile(path)
	if err != nil {
		return summary // build died before MSBuild wrote it; wall time still stands
	}

	section := 0
	for _, line := range strings.Split(string(data), "\n") {
		if strings.Contains(line, "Project Performance Summary:") {
			section = 1
			continue
		}
		if strings.Contains(line, "Task Performance Summary:") {
			section = 2
			continue
		}
		m := perfLine.FindStringSubmatch(strings.TrimRight(line, "\r"))
		if m == nil {
			continue
		}
		ms, _ := strconv.Atoi(m[1])
		name := strings.TrimSpace(m[2])
		switch section {
		case 1:
			// Project rows name a .vcxproj path; the indented rows under
			// each are per-target detail, skipped by this suffix test
			// rather than by trusting indentation widths.
			if strings.HasSuffix(name, ".vcxproj") {
				base := strings.TrimSuffix(filepath.Base(name), ".vcxproj")
				summary.Projects = append(summary.Projects, TimedItem{base, ms})
			}
		case 2:
			summary.Tasks = append(summary.Tasks, TimedItem{name, ms})
		}
	}
	sort.Slice(summary.Projects, func(i, j int) bool {
		return summary.Projects[i].Ms > summary.Projects[j].Ms
	})
	return summary
}
