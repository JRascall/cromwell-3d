// app.go — the one-shot paths, for scripts and CI.
//
// The cockpit is for a human; these are for everything else. `xcb build
// game_core` in a script must behave like a compiler: stream milestones,
// print the report, exit nonzero on failure. Same Runner, same Report, same
// History underneath — the cockpit and the CLI can never disagree about what
// a build cost, because neither owns the measurement.
package main

import (
	"fmt"
	"strings"
	"time"
)

func timeMs(ms int) time.Duration { return time.Duration(ms) * time.Millisecond }

type App struct {
	Workspace *Workspace
	History   *History
}

func (a *App) Build(spec BuildSpec) error {
	fmt.Println("building " + spec.Label())
	previous := a.History.LastComparable(spec)

	result := NewRunner(a.Workspace, spec).Run(printMilestones)

	Report{Result: result, Previous: previous}.Print()
	if err := a.History.Append(result); err != nil {
		fmt.Println(styleBad.Render("history not recorded: " + err.Error()))
	}
	if !result.OK {
		return fmt.Errorf("build failed: %s", result.ExitErr)
	}
	return nil
}

func (a *App) Trace(spec BuildSpec) error {
	job, err := NewTraceJob(&Ctx{WS: a.Workspace, History: a.History}, spec)
	if err != nil {
		return err
	}
	return job.Run(func(line string) { fmt.Println(line) })
}

func (a *App) PrintHistory(n int) error {
	entries, err := a.History.Recent(n)
	if err != nil {
		return err
	}
	if len(entries) == 0 {
		fmt.Println("no builds recorded yet — run `xcb build` or the cockpit")
		return nil
	}
	fmt.Printf("%-20s %-24s %-8s %9s %7s  %s\n",
		"when", "targets", "config", "wall", "files", "compile/link")
	for _, e := range entries {
		status := ""
		if !e.OK {
			status = " ✗"
		}
		fmt.Printf("%-20s %-24s %-8s %9s %7d  %s/%s%s\n",
			e.TS[:19], trunc(strings.Join(e.Targets, " "), 24), e.Config,
			e.Wall().Round(100_000_000), e.Files,
			(timeMs(e.ClMs)).Round(100_000_000), (timeMs(e.LinkMs)).Round(100_000_000),
			status)
	}
	return nil
}

// Milestones only: project completions and diagnostics. The full compiler
// stream is what plain mode exists to avoid.
func printMilestones(line string) {
	if strings.Contains(line, ".vcxproj ->") ||
		strings.Contains(line, "error") || strings.Contains(line, "warning") {
		fmt.Println(strings.TrimSpace(line))
	}
}
