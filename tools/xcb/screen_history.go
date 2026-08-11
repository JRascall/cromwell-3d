// screen_history.go — the record, browsable, with the trend drawn.
//
// THE CHART IS A BAR CHART, ONE BAR PER BUILD, and that shape was chosen
// against two alternatives that both shipped briefly and both misled:
//
//   - A wall-clock time axis interpolated a line through hours where no
//     build ran — it read as data at times nothing happened ("where's the
//     13:26 build?" — there wasn't one; the line just passed through
//     13:26). Worse, the library's time-axis labels render in UTC, so the
//     axis disagreed with the table by the machine's UTC offset.
//   - An index-spaced line fixed the phantom times but still fed indices
//     through machinery that thinks X is seconds, and collapsed.
//
// A bar per build is the honest shape for this data: builds are discrete
// events, a bar exists exactly where a build happened, its height is what
// that build cost, and nothing is implied between bars. Newest on the
// right, the slowest bar tinted red so the outlier names itself, and the
// time range written under the chart in LOCAL time — the same clock as the
// table. Per-bar time labels were considered and dropped: the library
// bounds a bar's label to the bar's width, two characters here.
//
// Tab cycles through the specs that have history — trends only mean
// anything within one spec, because building game_core and building xcom
// are different measurements. The table under the chart is the same data
// row by row, newest first, with the compile/link split that says which
// half of a regression moved.
package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/NimbleMarkets/ntcharts/barchart"
	"github.com/charmbracelet/bubbles/table"
	tea "github.com/charmbracelet/bubbletea"
)

const (
	chartHeight = 9
	barWidth    = 2
	barGap      = 1
)

type historyScreen struct {
	ctx     *Ctx
	entries []Entry
	keys    []string // unique spec keys, most recently built first
	keyIdx  int
	table   table.Model
	w, h    int

	chart      barchart.Model
	plotted    int // bars on screen (the last N that fit)
	recorded   int // successful builds this spec has in history
	oldest     time.Time
	newest     time.Time
	slowestSec float64
}

func newHistoryScreen(ctx *Ctx) (Screen, error) {
	entries, err := ctx.History.Recent(500)
	if err != nil {
		return nil, err
	}

	s := &historyScreen{ctx: ctx, entries: entries}

	seen := map[string]bool{}
	for i := len(entries) - 1; i >= 0; i-- {
		if key := entries[i].Key(); !seen[key] {
			seen[key] = true
			s.keys = append(s.keys, key)
		}
	}

	columns := []table.Column{
		{Title: "when", Width: 16},
		{Title: "targets", Width: 22},
		{Title: "config", Width: 8},
		{Title: "wall", Width: 9},
		{Title: "files", Width: 6},
		{Title: "compile", Width: 9},
		{Title: "link", Width: 9},
		{Title: "", Width: 2},
	}
	rows := make([]table.Row, 0, len(entries))
	for i := len(entries) - 1; i >= 0; i-- { // newest first
		e := entries[i]
		ts, _ := time.Parse(time.RFC3339, e.TS)
		status := ""
		if !e.OK {
			status = "✗"
		}
		rows = append(rows, table.Row{
			ts.Local().Format("01-02 15:04:05"),
			trunc(strings.Join(e.Targets, " "), 22),
			e.Config,
			e.Wall().Round(100 * time.Millisecond).String(),
			fmt.Sprintf("%d", e.Files),
			timeMs(e.ClMs).Round(100 * time.Millisecond).String(),
			timeMs(e.LinkMs).Round(100 * time.Millisecond).String(),
			status,
		})
	}
	s.table = table.New(table.WithColumns(columns), table.WithRows(rows), table.WithFocused(true))
	return s, nil
}

func (s *historyScreen) Init() tea.Cmd { return nil }

func (s *historyScreen) SetSize(w, h int) {
	s.w, s.h = w, h
	s.table.SetHeight(max(4, h-chartHeight-9))
	s.rebuildChart()
}

// The chart is rebuilt rather than mutated on every spec or size change:
// it is a few hundred points at most, and rebuild-from-data is the version
// of this code with no stale-state bug to find.
func (s *historyScreen) rebuildChart() {
	if len(s.keys) == 0 || s.w < 24 {
		return
	}
	key := s.keys[s.keyIdx]

	type buildPoint struct {
		when time.Time
		secs float64
	}
	var points []buildPoint
	for _, e := range s.entries {
		if !e.OK || e.Key() != key {
			continue
		}
		ts, err := time.Parse(time.RFC3339, e.TS)
		if err != nil {
			continue
		}
		points = append(points, buildPoint{ts.Local(), e.Wall().Seconds()})
	}
	s.recorded = len(points)
	if len(points) == 0 {
		return
	}

	// The last N builds that fit the width, newest at the right — history
	// beyond the window is summarised by the "last N of M" footer rather
	// than squeezed into ever-thinner bars.
	chartWidth := s.w - 4
	maxBars := max(1, (chartWidth-4)/(barWidth+barGap))
	if len(points) > maxBars {
		points = points[len(points)-maxBars:]
	}
	s.plotted = len(points)
	s.oldest = points[0].when
	s.newest = points[len(points)-1].when

	s.slowestSec = 0
	for _, p := range points {
		if p.secs > s.slowestSec {
			s.slowestSec = p.secs
		}
	}

	s.chart = barchart.New(chartWidth, chartHeight,
		barchart.WithBarWidth(barWidth), barchart.WithBarGap(barGap))
	for _, p := range points {
		style := chartLine()
		// The slowest bar is the one worth noticing; tint it so the
		// outlier names itself. Only meaningful once there is company.
		if len(points) > 1 && p.secs == s.slowestSec {
			style = styleBad
		}
		s.chart.Push(barchart.BarData{
			Values: []barchart.BarValue{{Name: "wall", Value: p.secs, Style: style}},
		})
	}
	s.chart.Draw()
}

func (s *historyScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	if key, ok := msg.(tea.KeyMsg); ok {
		switch key.String() {
		case "esc", "q", "enter":
			return s, backToMenu()
		case "tab":
			if len(s.keys) > 1 {
				s.keyIdx = (s.keyIdx + 1) % len(s.keys)
				s.rebuildChart()
			}
			return s, nil
		}
	}
	var cmd tea.Cmd
	s.table, cmd = s.table.Update(msg)
	return s, cmd
}

func (s *historyScreen) View() string {
	if len(s.entries) == 0 {
		return "  no builds recorded yet — run one from the menu\n\n" +
			styleDim.Render("  esc — back")
	}

	var b strings.Builder
	fmt.Fprintf(&b, "  %s %s %s\n",
		styleTitle.Render("wall time per build —"),
		styleBar.Render(s.keys[s.keyIdx]),
		styleDim.Render("(one bar per build · newest right · tab — next spec)"))

	if s.recorded == 0 {
		b.WriteString(styleDim.Render(
			"\n  no successful builds recorded for this spec\n\n"))
	} else {
		b.WriteString(s.chart.View() + "\n")
		b.WriteString(s.chartFooter() + "\n\n")
	}

	b.WriteString(s.table.View() + "\n")
	b.WriteString(styleDim.Render("  ↑/↓ scroll · tab — chart spec · esc — back") + "\n")
	return b.String()
}

// Oldest at the left edge, newest at the right — under the bars they
// describe — with the window and the outlier summarised between them.
func (s *historyScreen) chartFooter() string {
	layout := "15:04"
	if s.oldest.Format("01-02") != s.newest.Format("01-02") {
		layout = "01-02 15:04"
	}
	left := s.oldest.Format(layout) + " ⟵"
	right := "⟶ " + s.newest.Format(layout)
	middle := fmt.Sprintf("last %d of %d · slowest %.1fs", s.plotted, s.recorded, s.slowestSec)
	if s.plotted == 1 {
		left = ""
		middle = fmt.Sprintf("one build · %.1fs", s.slowestSec)
	}

	width := s.w - 4
	gap := width - len(left) - len(middle) - len(right)
	if gap < 2 {
		return styleDim.Render("  " + middle)
	}
	leftPad := gap / 2
	return styleDim.Render("  " + left + strings.Repeat(" ", leftPad) + middle +
		strings.Repeat(" ", gap-leftPad) + right)
}
