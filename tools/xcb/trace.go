// trace.go — the deep dive, delegated to the tool that owns the data.
//
// Tracer wraps vcperf, MSVC's C++ Build Insights front end, which ships with
// VS2022. It records the compiler's own ETW events — per-file front/back-end
// time, per-header parse cost, template instantiation, function codegen —
// and /timetrace exports them as Chrome Trace JSON. That lands in the exact
// viewer this project already lives in for frame captures (ui.perfetto.dev),
// which is why xcb renders none of this itself: the flame graph exists, and
// the team already knows how to read it.
//
// ETW REQUIRES ELEVATION. An unelevated run fails at /start with an access
// error; that is surfaced as advice rather than papered over, because
// silently degrading to a wall clock would report a number while pretending
// to be a profile.
//
// NUMBERED OUTPUT, like the game's F9 captures and for the same reason: a
// trace that can overwrite the previous one destroys the before/after
// comparison that justified taking it.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

type Tracer struct {
	ws     *Workspace
	vcperf string
	runner *Runner // the build under trace, kept so Cancel can reach it
}

func NewTracer(ws *Workspace) (*Tracer, error) {
	vcperf, err := findVcperf()
	if err != nil {
		return nil, err
	}
	return &Tracer{ws: ws, vcperf: vcperf}, nil
}

// Trace runs the build under an ETW session and exports a numbered Chrome
// trace. onLine sees the build's output plus the tracer's own milestones.
func (t *Tracer) Trace(spec BuildSpec, onLine func(string)) (string, error) {
	session := fmt.Sprintf("xcb%d", os.Getpid())
	if out, err := t.vcperfCmd("/start", "/nocpusampling", session); err != nil {
		return "", fmt.Errorf(
			"vcperf /start failed — tracing needs an ELEVATED shell (ETW):\n%s",
			strings.TrimSpace(out))
	}
	onLine("tracing under vcperf session " + session)

	t.runner = NewRunner(t.ws, spec)
	result := t.runner.Run(onLine)

	outPath, err := t.nextTracePath()
	if err != nil {
		return "", err
	}
	onLine("stopping trace and exporting — vcperf analyses the whole build, allow a minute...")
	if out, err := t.vcperfCmd("/stop", session, "/timetrace", outPath); err != nil {
		return "", fmt.Errorf("vcperf /stop failed:\n%s", strings.TrimSpace(out))
	}

	onLine("trace written: " + outPath)
	onLine("open it at ui.perfetto.dev — same viewer as the game's F9 captures.")
	onLine("look for: wide c1xx spans (front end / headers), wide c2 spans (codegen),")
	onLine("and the same header parsed across many files (the PCH argument).")
	if !result.OK {
		return outPath, fmt.Errorf("build failed during trace: %s", result.ExitErr)
	}
	return outPath, nil
}

// Cancel stops the build under trace. The ETW session is left for /stop to
// clean up on the next attempt; killing it here would race the exporter.
func (t *Tracer) Cancel() {
	if t.runner != nil {
		t.runner.Cancel()
	}
}

func (t *Tracer) vcperfCmd(args ...string) (string, error) {
	out, err := exec.Command(t.vcperf, args...).CombinedOutput()
	return string(out), err
}

func (t *Tracer) nextTracePath() (string, error) {
	dir := t.ws.TraceDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	for n := 1; ; n++ {
		p := filepath.Join(dir, fmt.Sprintf("build_%03d.json", n))
		if _, err := os.Stat(p); os.IsNotExist(err) {
			return p, nil
		}
	}
}

// The newest vcperf under the VS2022 toolsets. Numeric-aware compare on the
// version directories — lexicographic would sort 14.9 above 14.44.
func findVcperf() (string, error) {
	base := `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC`
	dirs, err := os.ReadDir(base)
	if err != nil {
		return "", fmt.Errorf("no MSVC toolsets under %s", base)
	}
	var versions []string
	for _, d := range dirs {
		if d.IsDir() {
			versions = append(versions, d.Name())
		}
	}
	sort.Slice(versions, func(i, j int) bool { return versionLess(versions[i], versions[j]) })
	for i := len(versions) - 1; i >= 0; i-- {
		p := filepath.Join(base, versions[i], `bin\Hostx64\x64\vcperf.exe`)
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("vcperf.exe not found in any toolset under %s", base)
}

func versionLess(a, b string) bool {
	as, bs := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < len(as) && i < len(bs); i++ {
		var an, bn int
		fmt.Sscanf(as[i], "%d", &an)
		fmt.Sscanf(bs[i], "%d", &bn)
		if an != bn {
			return an < bn
		}
	}
	return len(as) < len(bs)
}
