// screen_build.go — a build, live.
//
// WHAT THE PROGRESS BAR MEANS. There is no honest "percent of a build"
// without knowing the future, but history knows the recent past: the bar is
// elapsed time against the last successful build of the same spec, capped
// below full so it never claims a finish it cannot know. First-ever builds
// get the spinner and the clock instead of a bar pretending. That is the
// same incumbent-information idea the game's AI scoring uses — the previous
// run is data, and ignoring it wastes it.
//
// The bar runs in bubbles' ANIMATED mode — SetPercent starts a harmonica
// spring toward the target and FrameMsg drives it — so motion is smooth
// spring physics rather than 200ms steps.
//
// When the build finishes the screen swaps to the same Report the one-shot
// CLI prints, and the result is appended to history HERE, in Update, on the
// done message — never from the worker goroutine.
package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/progress"
	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
)

type buildLineMsg string
type buildDoneMsg struct{ result *BuildResult }

type buildScreen struct {
	ctx      *Ctx
	spec     BuildSpec
	runner   *Runner
	expected *Entry // last comparable build, or nil on a first run

	ch    chan tea.Msg
	sp    spinner.Model
	bar   progress.Model
	start time.Time

	files    int
	finished []string // "x.vcxproj -> ..." completions, kept on screen
	tail     []string
	result   *BuildResult
	report   string
	w, h     int
}

func newBuildScreen(ctx *Ctx, spec BuildSpec) *buildScreen {
	sp := spinner.New()
	sp.Spinner = spinner.MiniDot
	return &buildScreen{
		ctx:      ctx,
		spec:     spec,
		runner:   NewRunner(ctx.WS, spec),
		expected: ctx.History.LastComparable(spec),
		ch:       make(chan tea.Msg, 256),
		sp:       sp,
		bar:      progress.New(progress.WithDefaultGradient()),
		start:    time.Now(),
	}
}

func (s *buildScreen) Init() tea.Cmd {
	go func() {
		result := s.runner.Run(func(line string) { s.ch <- buildLineMsg(line) })
		s.ch <- buildDoneMsg{result}
	}()
	return tea.Batch(s.sp.Tick, listen(s.ch), tick())
}

func (s *buildScreen) SetSize(w, h int) {
	s.w, s.h = w, h
	s.bar.Width = min(60, w-14)
}

func (s *buildScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.KeyMsg:
		switch msg.String() {
		case "esc", "q":
			if s.result == nil {
				s.runner.Cancel() // tree-kill; the done message still arrives
				return s, nil
			}
			return s, backToMenu()
		case "enter":
			if s.result != nil {
				return s, backToMenu()
			}
		}
	case buildLineMsg:
		line := string(msg)
		if isSourceEcho(line) {
			s.files++
		}
		if strings.Contains(line, ".vcxproj ->") {
			s.finished = append(s.finished, strings.TrimSpace(line))
		}
		s.tail = append(s.tail, strings.TrimSpace(line))
		if len(s.tail) > 6 {
			s.tail = s.tail[len(s.tail)-6:]
		}
		return s, listen(s.ch) // re-arm; the worker is still talking
	case buildDoneMsg:
		s.result = msg.result
		s.report = Report{Result: s.result, Previous: s.expected}.Render()
		if err := s.ctx.History.Append(s.result); err != nil {
			s.report += "\n" + styleBad.Render("history not recorded: "+err.Error())
		}
		return s, nil // deliberately no re-arm: the worker is done
	case tickMsg:
		if s.result == nil {
			cmds := []tea.Cmd{tick()}
			if s.expected != nil {
				pct := float64(time.Since(s.start)) / float64(s.expected.Wall())
				if pct > 0.97 {
					pct = 0.97 // never claim a finish the build has not reported
				}
				cmds = append(cmds, s.bar.SetPercent(pct))
			}
			return s, tea.Batch(cmds...)
		}
		return s, nil
	case progress.FrameMsg:
		barModel, cmd := s.bar.Update(msg)
		s.bar = barModel.(progress.Model)
		return s, cmd
	}
	var cmd tea.Cmd
	s.sp, cmd = s.sp.Update(msg)
	return s, cmd
}

func (s *buildScreen) View() string {
	var b strings.Builder

	if s.result != nil {
		b.WriteString(s.report)
		b.WriteString("\n" + styleDim.Render("  enter/esc — back to the menu") + "\n")
		return b.String()
	}

	elapsed := time.Since(s.start)
	fmt.Fprintf(&b, "  %s %s  %s · %d files\n\n",
		s.sp.View(),
		styleTitle.Render("building "+s.spec.Label()),
		elapsed.Round(time.Second), s.files)

	if s.expected != nil {
		fmt.Fprintf(&b, "  %s %s\n\n", s.bar.View(),
			styleDim.Render("~"+s.expected.Wall().Round(time.Second).String()+" last time"))
	}

	for _, f := range s.finished {
		fmt.Fprintf(&b, "  %s %s\n", styleGood.Render("✓"), styleDim.Render(trunc(f, s.w-6)))
	}
	if len(s.finished) > 0 {
		b.WriteString("\n")
	}
	for _, t := range s.tail {
		fmt.Fprintf(&b, "  %s\n", styleDim.Render(trunc(t, s.w-4)))
	}
	b.WriteString("\n" + styleDim.Render("  esc — cancel") + "\n")
	return b.String()
}
