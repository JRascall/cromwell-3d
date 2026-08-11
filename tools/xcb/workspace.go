// workspace.go — where everything is.
//
// Workspace is the only type that knows paths: the checkout root, the build
// tree, the history file, the trace directory, and where VS2022 hides its
// cmake. Everything else asks it, which is what keeps a path decision — say,
// moving history out of a deletable directory — a one-file change.
package main

import (
	"errors"
	"os"
	"path/filepath"
)

type Workspace struct {
	Root string
}

// FindWorkspace walks up from the working directory to the first configured
// build tree. Anchoring on CMakeCache.txt rather than CMakeLists.txt means
// "you have not configured yet" is its own clear error rather than a cryptic
// cmake one three steps later.
func FindWorkspace() (*Workspace, error) {
	dir, err := os.Getwd()
	if err != nil {
		return nil, err
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "builds", "_cmake-win", "CMakeCache.txt")); err == nil {
			return &Workspace{Root: dir}, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return nil, errors.New("no configured build tree (builds/_cmake-win/CMakeCache.txt) above the working directory")
		}
		dir = parent
	}
}

func (w *Workspace) BuildTree() string {
	return filepath.Join(w.Root, "builds", "_cmake-win")
}

// History lives beside the tool, NOT under builds/: the build tree is
// documented as deletable-whole, and history is the one thing xcb produces
// that must survive that — or "getting slower over time" resets whenever the
// tree is wiped. Gitignored; measurements from one machine say nothing about
// another.
func (w *Workspace) HistoryPath() string {
	return filepath.Join(w.Root, "tools", "xcb", "history.jsonl")
}

// Traces DO go under builds/: they are large, machine-specific artefacts of
// one investigation, exactly the class of file a build-tree wipe should take
// with it.
func (w *Workspace) TraceDir() string {
	return filepath.Join(w.Root, "builds", "buildtraces")
}

// cmake ships with VS2022 and is not on PATH — the same fact CLAUDE.md leads
// with, resolved here so the tool works from a bare shell.
func (w *Workspace) CMake() string {
	vs := `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
	if _, err := os.Stat(vs); err == nil {
		return vs
	}
	return "cmake"
}

// ctest lives beside whichever cmake was found.
func (w *Workspace) CTest() string {
	cm := w.CMake()
	if cm == "cmake" {
		return "ctest"
	}
	return filepath.Join(filepath.Dir(cm), "ctest.exe")
}
