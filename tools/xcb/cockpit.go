// cockpit.go — the app you stay in.
//
// The cockpit is a resident Bubble Tea program: launch xcb with no arguments
// and it opens on the menu, actions run inside it, and finishing one returns
// to the menu rather than the shell. The one-shot subcommands (`xcb build
// ...`) still exist for scripts and CI; this file is for the human.
//
// STRUCTURE: the root model owns nothing but navigation. Each screen — menu,
// build, task, history — is its own type behind the Screen interface, and
// long-running work sits behind the Job interface, so a new action (a bake
// step, a package step) is a new Job plus a menu row, with no change to the
// screens that display it. That is the open/closed seam this tool grows
// along.
//
// CONCURRENCY, the one rule: child processes stream from a reader goroutine,
// but ALL state changes happen in Update. The bridge is a channel of
// tea.Msgs and the listen() command below — the goroutine writes, the
// message loop reads, and nothing else is shared.
package main

import (
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

// Ctx is what every screen gets to work with: where things are, and what
// happened before. Screens receive it at construction — none of them reach
// for globals.
type Ctx struct {
	WS      *Workspace
	History *History
}

// Screen is one full-window state of the cockpit. Update returns the screen
// to show next — usually itself — which is what makes a screen swap a
// return value rather than a side effect.
type Screen interface {
	Init() tea.Cmd
	Update(msg tea.Msg) (Screen, tea.Cmd)
	View() string
	SetSize(w, h int)
}

// Job is a long-running action a screen can host: a build, a bake, a trace.
// Run blocks and reports lines; Cancel must be safe to call from Update
// while Run is blocked in a goroutine.
type Job interface {
	Name() string
	Run(onLine func(string)) error
	Cancel()
}

// ---- navigation messages ----------------------------------------------

type navMsg struct{ screen Screen }
type backMsg struct{}

func nav(s Screen) tea.Cmd  { return func() tea.Msg { return navMsg{s} } }
func backToMenu() tea.Cmd   { return func() tea.Msg { return backMsg{} } }

// listen bridges a worker goroutine into the message loop: each received
// message is delivered once, and the handler re-arms by returning listen
// again. Not re-arming after a done message is what lets the goroutine end.
func listen(ch chan tea.Msg) tea.Cmd {
	return func() tea.Msg { return <-ch }
}

type tickMsg time.Time

func tick() tea.Cmd {
	return tea.Tick(200*time.Millisecond, func(t time.Time) tea.Msg { return tickMsg(t) })
}

// ---- the root model ----------------------------------------------------

var (
	styleHeader = lipgloss.NewStyle().Bold(true).
			Foreground(lipgloss.Color("230")).
			Background(lipgloss.Color("62")).
			Padding(0, 1)
	styleHeaderPath = lipgloss.NewStyle().Faint(true).Padding(0, 1)
)

type Cockpit struct {
	ctx    *Ctx
	screen Screen
	w, h   int
}

func RunCockpit(ctx *Ctx) error {
	root := &Cockpit{ctx: ctx, screen: newMenuScreen(ctx)}
	_, err := tea.NewProgram(root, tea.WithAltScreen()).Run()
	return err
}

func (c *Cockpit) Init() tea.Cmd { return c.screen.Init() }

func (c *Cockpit) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		c.w, c.h = msg.Width, msg.Height
		c.screen.SetSize(c.w, c.h-2) // minus the header
		return c, nil
	case tea.KeyMsg:
		if msg.String() == "ctrl+c" {
			return c, tea.Quit
		}
	case navMsg:
		c.screen = msg.screen
		c.screen.SetSize(c.w, c.h-2)
		return c, c.screen.Init()
	case backMsg:
		c.screen = newMenuScreen(c.ctx)
		c.screen.SetSize(c.w, c.h-2)
		return c, c.screen.Init()
	}
	next, cmd := c.screen.Update(msg)
	c.screen = next
	return c, cmd
}

func (c *Cockpit) View() string {
	header := lipgloss.JoinHorizontal(lipgloss.Top,
		styleHeader.Render("⚒ xcb"),
		styleHeaderPath.Render(c.ctx.WS.Root))
	return header + "\n\n" + c.screen.View()
}
