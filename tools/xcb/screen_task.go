// screen_task.go — any Job, hosted.
//
// The generic action screen: spinner, output, a clear end state. Builds get
// their own richer screen; everything else — bakes, test runs, traces — is a
// Job, and this screen neither knows nor cares which. New pipeline steps
// cost a Job implementation and a menu row, and no UI work.
//
// Output lives in a bubbles viewport rather than a hand-rolled tail: the
// full log is scrollable during and after the run, and it follows the
// bottom only while you are AT the bottom — scrolling up to read an early
// error pins the view there instead of yanking it away on the next line.
package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/spinner"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
)

// CmdJob is the trivial Job: run one command, stream it.
type CmdJob struct {
	name   string
	stream StreamedCmd
}

func (j *CmdJob) Name() string                  { return j.name }
func (j *CmdJob) Run(onLine func(string)) error { return j.stream.Run(onLine) }
func (j *CmdJob) Cancel()                       { j.stream.Cancel() }

// TraceJob wraps the Tracer so a deep profile is just another menu action.
type TraceJob struct {
	ctx    *Ctx
	spec   BuildSpec
	tracer *Tracer
}

func NewTraceJob(ctx *Ctx, spec BuildSpec) (*TraceJob, error) {
	tracer, err := NewTracer(ctx.WS)
	if err != nil {
		return nil, err
	}
	return &TraceJob{ctx: ctx, spec: spec, tracer: tracer}, nil
}

func (j *TraceJob) Name() string { return "trace " + j.spec.Label() + " (vcperf)" }

func (j *TraceJob) Run(onLine func(string)) error {
	_, err := j.tracer.Trace(j.spec, onLine)
	return err
}

func (j *TraceJob) Cancel() { j.tracer.Cancel() }

// ---- the screen --------------------------------------------------------

// Enough scrollback for a full ctest run or a noisy bake; beyond this the
// oldest lines drop, which a log this size has stopped being read for.
const maxTaskLines = 5000

type taskLineMsg string
type taskDoneMsg struct{ err error }

type taskScreen struct {
	job      Job
	ch       chan tea.Msg
	sp       spinner.Model
	vp       viewport.Model
	start    time.Time
	lines    []string
	err      error
	finished bool
	w, h     int
}

func newTaskScreen(job Job) *taskScreen {
	sp := spinner.New()
	sp.Spinner = spinner.MiniDot
	return &taskScreen{
		job:   job,
		ch:    make(chan tea.Msg, 256),
		sp:    sp,
		vp:    viewport.New(80, 20),
		start: time.Now(),
	}
}

func (s *taskScreen) Init() tea.Cmd {
	go func() {
		err := s.job.Run(func(line string) { s.ch <- taskLineMsg(line) })
		s.ch <- taskDoneMsg{err}
	}()
	return tea.Batch(s.sp.Tick, listen(s.ch))
}

func (s *taskScreen) SetSize(w, h int) {
	s.w, s.h = w, h
	s.vp.Width = max(20, w-2)
	s.vp.Height = max(6, h-6)
	s.vp.SetContent(strings.Join(s.lines, "\n"))
}

func (s *taskScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.KeyMsg:
		switch msg.String() {
		case "esc", "q":
			if !s.finished {
				s.job.Cancel()
				return s, nil
			}
			return s, backToMenu()
		case "enter":
			if s.finished {
				return s, backToMenu()
			}
		}
	case taskLineMsg:
		s.lines = append(s.lines, string(msg))
		if len(s.lines) > maxTaskLines {
			s.lines = s.lines[len(s.lines)-maxTaskLines:]
		}
		follow := s.vp.AtBottom()
		s.vp.SetContent(strings.Join(s.lines, "\n"))
		if follow {
			s.vp.GotoBottom()
		}
		return s, listen(s.ch)
	case taskDoneMsg:
		s.finished = true
		s.err = msg.err
		return s, nil
	}

	var spCmd, vpCmd tea.Cmd
	s.sp, spCmd = s.sp.Update(msg)
	s.vp, vpCmd = s.vp.Update(msg)
	return s, tea.Batch(spCmd, vpCmd)
}

func (s *taskScreen) View() string {
	var b strings.Builder

	head := s.sp.View()
	if s.finished {
		if s.err != nil {
			head = styleBad.Render("✗")
		} else {
			head = styleGood.Render("✓")
		}
	}
	fmt.Fprintf(&b, "  %s %s  %s\n\n", head, styleTitle.Render(s.job.Name()),
		time.Since(s.start).Round(time.Second))

	b.WriteString(s.vp.View() + "\n")

	if s.finished {
		if s.err != nil {
			fmt.Fprintf(&b, "\n  %s\n", styleBad.Render(trunc(s.err.Error(), s.w-4)))
		}
		b.WriteString(styleDim.Render("  ↑/↓ scroll · enter/esc — back to the menu") + "\n")
	} else {
		b.WriteString(styleDim.Render("  ↑/↓ scroll · esc — cancel") + "\n")
	}
	return b.String()
}
