// report.go — how a finished build reads.
//
// Report renders a BuildResult (and the previous comparable Entry, when
// there is one) as text. ONE renderer for both worlds: the cockpit embeds
// the same string the one-shot CLI prints, so the breakdown can never
// disagree with itself depending on how the build was launched.
package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/charmbracelet/lipgloss"
)

var (
	styleTitle = lipgloss.NewStyle().Bold(true)
	styleDim   = lipgloss.NewStyle().Faint(true)
	styleGood  = lipgloss.NewStyle().Foreground(lipgloss.Color("42"))
	styleBad   = lipgloss.NewStyle().Foreground(lipgloss.Color("196"))
	styleBar   = lipgloss.NewStyle().Foreground(lipgloss.Color("62"))
)

// chartLine matches the history chart to the rest of the accent colour. A
// function rather than a var because ntcharts wants its own style value and
// pulling that import into every file that uses the palette would spread a
// chart dependency through files that never chart.
func chartLine() lipgloss.Style {
	return lipgloss.NewStyle().Foreground(lipgloss.Color("62"))
}

type Report struct {
	Result   *BuildResult
	Previous *Entry
}

func (r Report) Render() string {
	var b strings.Builder
	result := r.Result

	status := styleGood.Render("✓")
	if !result.OK {
		status = styleBad.Render("✗ FAILED")
	}

	delta := ""
	if r.Previous != nil {
		diff := result.Wall - r.Previous.Wall()
		sign := "+"
		if diff < 0 {
			sign = "−"
			diff = -diff
		}
		delta = styleDim.Render(fmt.Sprintf("  (last %s, %s%s)",
			r.Previous.Wall().Round(100*time.Millisecond),
			sign, diff.Round(100*time.Millisecond)))
	}

	fmt.Fprintf(&b, "%s %s in %s%s\n", status,
		styleTitle.Render(result.Spec.Label()),
		result.Wall.Round(100*time.Millisecond), delta)
	if result.Files > 0 {
		fmt.Fprintf(&b, "%s\n", styleDim.Render(
			fmt.Sprintf("  %d files compiled", result.Files)))
	} else if result.OK {
		// Say what a no-op means, because the on-disk evidence is
		// counter-intuitive: an up-to-date build touches nothing, so the
		// exe keeps the timestamp of the last build that actually linked —
		// which reads as "my build didn't happen" the first time you meet
		// it.
		fmt.Fprintf(&b, "%s\n", styleDim.Render(
			"  up to date — nothing recompiled, outputs untouched (timestamps keep the last real link)"))
	}

	if total := result.Summary.TotalProjectMs(); total > 0 {
		fmt.Fprintf(&b, "\n  where the time went (MSBuild):\n")
		for _, p := range result.Summary.Projects {
			if p.Ms*100 < total { // under 1% is noise, not signal
				continue
			}
			bar := strings.Repeat("█", p.Ms*24/total)
			fmt.Fprintf(&b, "    %-22s %8s  %s %d%%\n", p.Name,
				(time.Duration(p.Ms) * time.Millisecond).Round(10*time.Millisecond),
				styleBar.Render(bar), p.Ms*100/total)
		}
	}

	cl := result.Summary.TaskMs("CL")
	link := result.Summary.TaskMs("Link")
	lib := result.Summary.TaskMs("Lib")
	if cl+link+lib > 0 {
		fmt.Fprintf(&b, "%s\n", styleDim.Render(fmt.Sprintf(
			"  compile %s · link %s · lib %s",
			(time.Duration(cl)*time.Millisecond).Round(10*time.Millisecond),
			(time.Duration(link)*time.Millisecond).Round(10*time.Millisecond),
			(time.Duration(lib)*time.Millisecond).Round(10*time.Millisecond))))
	}
	return b.String()
}

func (r Report) Print() { fmt.Println("\n" + r.Render()) }

func trunc(s string, n int) string {
	if n < 4 || len(s) <= n {
		return s
	}
	return s[:n-1] + "…"
}
