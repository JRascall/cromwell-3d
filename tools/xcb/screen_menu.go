// screen_menu.go — the cockpit's front page.
//
// The menu is a list of actions, and an action is a title plus a constructor
// for the screen that performs it. Rows appear only when their prerequisites
// exist on this machine (a bake script, a test runner), so the menu is an
// inventory of what this checkout can actually do rather than a wish list.
package main

import (
	"os"
	"path/filepath"
	"strings"

	"github.com/charmbracelet/bubbles/list"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
)

type action struct {
	title string
	desc  string
	// make builds the screen this action runs in. A nil screen with a nil
	// error means "quit the cockpit".
	make func(ctx *Ctx) (Screen, error)
}

func (a action) Title() string       { return a.title }
func (a action) Description() string { return a.desc }
func (a action) FilterValue() string { return a.title }

type menuScreen struct {
	ctx  *Ctx
	list list.Model
	err  string
	w, h int
}

func newMenuScreen(ctx *Ctx) *menuScreen {
	items := []list.Item{
		action{"build xcom", "the game, staged to builds/win", func(c *Ctx) (Screen, error) {
			return newBuildScreen(c, BuildSpec{Targets: []string{"xcom"}, Config: "Release"}), nil
		}},
		action{"build game_core", "headless simulation — the fast loop", func(c *Ctx) (Screen, error) {
			return newBuildScreen(c, BuildSpec{Targets: []string{"game_core"}, Config: "Release"}), nil
		}},
		action{"build …", "type any cmake target", func(c *Ctx) (Screen, error) {
			return newTargetPrompt(c), nil
		}},
	}

	if _, err := os.Stat(ctx.WS.CTest()); err == nil {
		items = append(items, action{"test", "ctest -C Release — every suite", func(c *Ctx) (Screen, error) {
			return newTaskScreen(&CmdJob{name: "ctest (all suites)",
				stream: StreamedCmd{Argv: []string{c.WS.CTest(), "-C", "Release"},
					Dir: c.WS.BuildTree()}}), nil
		}})
	}

	// Bake steps appear when their scripts do. Adding a pipeline step to the
	// cockpit is: write the script, add a row here.
	for _, bake := range []struct{ script, title, desc string }{
		{"bake_msdf.py", "bake msdf atlas", "distance-field font atlas (msdf-atlas-gen)"},
		{"gen_fa_icons.py", "bake icon font", "Font Awesome icon vocabulary"},
	} {
		script := filepath.Join(ctx.WS.Root, "tools", bake.script)
		if _, err := os.Stat(script); err != nil {
			continue
		}
		title, desc := bake.title, bake.desc
		items = append(items, action{title, desc, func(c *Ctx) (Screen, error) {
			return newTaskScreen(&CmdJob{name: title,
				stream: StreamedCmd{Argv: []string{"python", script}, Dir: c.WS.Root}}), nil
		}})
	}

	items = append(items,
		action{"trace a build", "vcperf → Chrome trace for ui.perfetto.dev (needs elevation)",
			func(c *Ctx) (Screen, error) {
				job, err := NewTraceJob(c, BuildSpec{Targets: []string{"xcom"}, Config: "Release"})
				if err != nil {
					return nil, err
				}
				return newTaskScreen(job), nil
			}},
		action{"history", "every recorded build, newest first", func(c *Ctx) (Screen, error) {
			return newHistoryScreen(c)
		}},
		action{"quit", "", func(*Ctx) (Screen, error) { return nil, nil }},
	)

	delegate := list.NewDefaultDelegate()
	l := list.New(items, delegate, 0, 0)
	l.Title = "what are we doing?"
	l.SetShowStatusBar(false)
	l.SetFilteringEnabled(false)
	l.SetShowHelp(true)

	return &menuScreen{ctx: ctx, list: l}
}

func (s *menuScreen) Init() tea.Cmd { return nil }

func (s *menuScreen) SetSize(w, h int) {
	s.w, s.h = w, h
	s.list.SetSize(w-2, h-2)
}

func (s *menuScreen) Update(msg tea.Msg) (Screen, tea.Cmd) {
	if key, ok := msg.(tea.KeyMsg); ok {
		switch key.String() {
		case "q":
			return s, tea.Quit
		case "enter":
			act, ok := s.list.SelectedItem().(action)
			if !ok {
				return s, nil
			}
			next, err := act.make(s.ctx)
			if err != nil {
				s.err = err.Error()
				return s, nil
			}
			if next == nil {
				return s, tea.Quit
			}
			return s, nav(next)
		}
	}
	var cmd tea.Cmd
	s.list, cmd = s.list.Update(msg)
	return s, cmd
}

func (s *menuScreen) View() string {
	view := s.list.View()
	if s.err != "" {
		view += "\n" + styleBad.Render("  "+s.err)
	}
	return view
}

// ---- the free-target prompt -------------------------------------------

// A one-line prompt rather than a config file: the target names are cmake's,
// and the person typing one knows it better than a curated list would.
// bubbles/textinput rather than hand-rolled key handling — it brings the
// cursor, paste, and the editing keys nobody remembers to implement.
type targetPrompt struct {
	ctx   *Ctx
	input textinput.Model
	w, h  int
}

func newTargetPrompt(ctx *Ctx) *targetPrompt {
	input := textinput.New()
	input.Placeholder = "cmake target, e.g. xcom_perf"
	input.CharLimit = 64
	input.Width = 40
	input.Focus()
	return &targetPrompt{ctx: ctx, input: input}
}

func (s *targetPrompt) Init() tea.Cmd    { return textinput.Blink }
func (s *targetPrompt) SetSize(w, h int) { s.w, s.h = w, h }

func (s *targetPrompt) Update(msg tea.Msg) (Screen, tea.Cmd) {
	if key, ok := msg.(tea.KeyMsg); ok {
		switch key.String() {
		case "esc":
			return s, backToMenu()
		case "enter":
			target := strings.TrimSpace(s.input.Value())
			if target == "" {
				return s, nil
			}
			return s, nav(newBuildScreen(s.ctx,
				BuildSpec{Targets: []string{target}, Config: "Release"}))
		}
	}
	var cmd tea.Cmd
	s.input, cmd = s.input.Update(msg)
	return s, cmd
}

func (s *targetPrompt) View() string {
	return "  " + styleTitle.Render("target: ") + s.input.View() +
		"\n\n" + styleDim.Render("  enter to build · esc to go back")
}
