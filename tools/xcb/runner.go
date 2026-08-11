// runner.go — WHAT to build and HOW, and nothing about how it looks.
//
// BuildSpec is the value that names a build (targets + config); Runner turns
// one into a cmake invocation and a BuildResult. The caller supplies an
// onLine observer; whether those lines feed a cockpit screen or a plain
// stdout is not this file's business — that separation is what lets the
// same Runner sit under the TUI, the one-shot CLI and the tracer.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type BuildSpec struct {
	Targets []string
	Config  string
}

func (s BuildSpec) Label() string {
	return strings.Join(s.Targets, " ") + " (" + s.Config + ")"
}

// Key is what makes two builds comparable in history: same targets, same
// config. Wall times across different keys measure different things.
func (s BuildSpec) Key() string {
	return strings.Join(s.Targets, "+") + "/" + s.Config
}

type BuildResult struct {
	Spec    BuildSpec
	Wall    time.Duration
	OK      bool
	ExitErr string
	Files   int
	Summary PerfSummary
}

type Runner struct {
	ws     *Workspace
	spec   BuildSpec
	stream StreamedCmd
	files  int
}

func NewRunner(ws *Workspace, spec BuildSpec) *Runner {
	return &Runner{ws: ws, spec: spec}
}

// Run blocks until the build finishes. onLine sees every output line and may
// be nil; it is called from the reader goroutine, so a TUI must forward into
// its own message loop rather than mutate state directly.
func (r *Runner) Run(onLine func(string)) *BuildResult {
	// The temp dir rather than the repo for the logger file: MSBuild logger
	// parameters are semicolon-separated with no quoting convention that
	// survives every layer, and this repo's absolute path contains a space.
	perfLog := filepath.Join(os.TempDir(), fmt.Sprintf("xcb_perf_%d.log", os.Getpid()))
	defer os.Remove(perfLog)

	args := []string{r.ws.CMake(), "--build", r.ws.BuildTree(),
		"--config", r.spec.Config, "--parallel"}
	for _, t := range r.spec.Targets {
		args = append(args, "--target", t)
	}
	args = append(args, "--", "/nologo",
		"/flp:PerformanceSummary;Verbosity=minimal;Encoding=UTF-8;LogFile="+perfLog)

	r.stream = StreamedCmd{Argv: args, Dir: r.ws.Root}

	start := time.Now()
	err := r.stream.Run(func(line string) {
		if isSourceEcho(line) {
			r.files++
		}
		if onLine != nil {
			onLine(line)
		}
	})

	result := &BuildResult{
		Spec:    r.spec,
		Wall:    time.Since(start),
		OK:      err == nil,
		Files:   r.files,
		Summary: parsePerfSummary(perfLog),
	}
	if err != nil {
		result.ExitErr = err.Error()
	}
	return result
}

func (r *Runner) Cancel() { r.stream.Cancel() }
