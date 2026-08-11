// xcb — the project's cockpit.
//
// Launch it bare and it stays open: a menu of everything this checkout can
// do — build targets, run the test suites, bake assets, trace a build —
// with live progress, a per-project time breakdown after every build, and a
// history that makes "builds are getting slower" a diff against data rather
// than a feeling. Nothing builds differently through it; it wraps the same
// cmake invocation CLAUDE.md documents.
//
// THE SHAPE OF THE TOOL, one type per file, one job per type:
//
//	Workspace   where the checkout, build tree and toolchain are   workspace.go
//	StreamedCmd one child process, streamed and tree-killable      stream.go
//	BuildSpec   WHAT to build; Runner is HOW                       runner.go
//	PerfSummary the numbers MSBuild measured, parsed               perflog.go
//	History     every build as a JSON line, and the diffing        history.go
//	Report      how a finished build reads (CLI and cockpit)       report.go
//	Tracer      the vcperf deep dive, exported for Perfetto        trace.go
//	Cockpit     the resident UI: Screen and Job interfaces         cockpit.go
//	screens     menu / build / task / history                      screen_*.go
//	App         the one-shot CLI paths for scripts and CI          app.go
//
// main.go owns nothing but the command line: parse, wire, dispatch.
package main

import (
	"fmt"
	"os"
	"strings"
)

func main() {
	args := os.Args[1:]

	command := "cockpit"
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		command = args[0]
		args = args[1:]
	}

	if command == "help" || command == "--help" || command == "-h" {
		usage()
		return
	}

	workspace, err := FindWorkspace()
	if err != nil {
		fatal(err)
	}
	history := NewHistory(workspace.HistoryPath())

	switch command {
	case "cockpit":
		err = RunCockpit(&Ctx{WS: workspace, History: history})
	case "build":
		app := &App{Workspace: workspace, History: history}
		err = app.Build(parseBuildArgs(args))
	case "trace":
		app := &App{Workspace: workspace, History: history}
		err = app.Trace(parseBuildArgs(args))
	case "history":
		app := &App{Workspace: workspace, History: history}
		err = app.PrintHistory(parseHistoryArgs(args))
	default:
		usage()
		err = fmt.Errorf("unknown command %q", command)
	}
	if err != nil {
		fatal(err)
	}
}

func parseBuildArgs(args []string) BuildSpec {
	spec := BuildSpec{Config: "Release"}
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--config":
			if i+1 < len(args) {
				i++
				spec.Config = args[i]
			}
		default:
			if !strings.HasPrefix(args[i], "-") {
				spec.Targets = append(spec.Targets, args[i])
			}
		}
	}
	if len(spec.Targets) == 0 {
		spec.Targets = []string{"xcom"}
	}
	return spec
}

func parseHistoryArgs(args []string) int {
	n := 20
	for i := 0; i < len(args); i++ {
		if args[i] == "-n" && i+1 < len(args) {
			fmt.Sscanf(args[i+1], "%d", &n)
			i++
		}
	}
	return n
}

func usage() {
	fmt.Println(`xcb — the build cockpit

  xcb
        open the cockpit: build, test, bake and trace from a resident UI,
        with live progress and per-project time breakdowns
  xcb build [target...] [--config Release]
        one-shot build for scripts and CI: milestones, report, exit code
  xcb history [-n 20]
        recent builds, oldest first
  xcb trace [target...]
        build wrapped in vcperf (needs an ELEVATED shell — ETW). Writes
        Chrome Trace JSON under builds/buildtraces; open at ui.perfetto.dev`)
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "xcb:", err)
	os.Exit(1)
}
