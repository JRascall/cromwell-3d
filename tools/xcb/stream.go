// stream.go — one process, streamed line by line.
//
// StreamedCmd is the single owner of "run a child process and watch its
// output": the pipe plumbing, the line scanning, and the tree kill. Runner
// (cmake), the bake tasks (python) and the tracer (vcperf-wrapped build) all
// sit on top of it, which is what keeps process handling out of the UI and
// identical everywhere — a bug in cancellation is one bug, not four.
package main

import (
	"bufio"
	"io"
	"os/exec"
	"strconv"
)

type StreamedCmd struct {
	Argv []string
	Dir  string

	cmd *exec.Cmd
}

// Run blocks until the process exits, calling onLine for every output line
// (stdout and stderr merged — build tools scatter diagnostics across both,
// and a display that loses stderr loses exactly the lines that matter).
func (s *StreamedCmd) Run(onLine func(string)) error {
	s.cmd = exec.Command(s.Argv[0], s.Argv[1:]...)
	s.cmd.Dir = s.Dir

	pr, pw := io.Pipe()
	s.cmd.Stdout = pw
	s.cmd.Stderr = pw

	drained := make(chan struct{})
	go func() {
		defer close(drained)
		scanner := bufio.NewScanner(pr)
		scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
		for scanner.Scan() {
			if onLine != nil {
				onLine(scanner.Text())
			}
		}
	}()

	err := s.cmd.Start()
	if err == nil {
		err = s.cmd.Wait()
	}
	pw.Close()
	<-drained
	return err
}

// Cancel kills the whole tree: cmake spawns MSBuild spawns cl, and killing
// only the root leaves compilers holding source files open — the orphaned
// cl.exe failure mode this project has already been bitten by (edits then
// fail with EPERM and it reads like the editor's fault).
func (s *StreamedCmd) Cancel() {
	if s.cmd != nil && s.cmd.Process != nil {
		_ = exec.Command("taskkill", "/T", "/F", "/PID",
			strconv.Itoa(s.cmd.Process.Pid)).Run()
	}
}
