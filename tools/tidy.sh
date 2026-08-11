#!/usr/bin/env bash
# tidy.sh — run clang-tidy over the headless simulation and engine code.
#
# SINGLE RESPONSIBILITY: point clang-tidy at the portable half of the tree and
# print what it finds. Configuration lives in .clang-tidy at the repo root.
#
# NO COMPILE DATABASE, DELIBERATELY. The usual setup exports
# compile_commands.json from a Ninja build, which needs a Visual Studio
# developer environment to be sourced first. The code checked here is portable
# C++20 with no raylib and no Windows headers, so `-std=c++20 -Isrc` is the
# entire flag set it needs — and clang-tidy locates the MSVC standard library on
# its own. One less moving part, and it runs from any shell.
#
# RENDER CODE IS COVERED TOO, as long as the project has been configured once:
# raylib and imgui are fetched into builds/_cmake-win/_deps, and their include
# paths are added below when they exist. If they do not, the render files are
# skipped with a note rather than filling the output with "raylib.h not found".
#
# WHAT IS NOT CHECKED, and why:
#   cromwell/net, cromwell/steam, cromwell/diag/CrashHandler
#       Windows SDK headers, and not performance-sensitive
#   cromwell/web
#       CEF, whose headers are a separate fetched tree
#
# Neither matters much: the performance-sensitive code is the simulation, and
# that is all covered.
#
#   ./tools/tidy.sh              everything below
#   ./tools/tidy.sh src/game/los/RayCaster.cpp    one file
#   ./tools/tidy.sh --fix        apply the fixes clang-tidy is confident about
#
# REMEMBER WHAT THIS CANNOT SEE. It finds local patterns - a needless copy, a
# vector that should have reserved. The expensive mistakes in this codebase were
# contextual: correct code called from inside a hot loop four levels up. See the
# note at the top of .clang-tidy.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TIDY="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin/clang-tidy.exe"
if [ ! -f "$TIDY" ]; then
    TIDY="$(command -v clang-tidy || true)"
fi
if [ -z "$TIDY" ] || [ ! -f "$TIDY" ]; then
    echo "clang-tidy not found. It ships with Visual Studio 2022 under" >&2
    echo "  VC/Tools/Llvm/bin/, or install LLVM and put it on PATH." >&2
    exit 1
fi

FIX=""
FILES=()
for arg in "$@"; do
    case "$arg" in
        --fix) FIX="--fix" ;;
        *)     FILES+=("$arg") ;;
    esac
done

# Include paths for the fetched dependencies, when a configure has produced them.
DEPS="builds/_cmake-win/_deps"
EXTRA_INCLUDES=()
HAVE_RAYLIB=0
if [ -f "$DEPS/raylib-src/src/raylib.h" ]; then
    EXTRA_INCLUDES+=("-I$DEPS/raylib-src/src" "-I$DEPS/raylib-src/src/external")
    # GLFW rides along inside raylib's tree. One file wants its headers -
    # platform/ModalLoopPump.cpp - for the same reason its CMake include is
    # PRIVATE: the refresh callback and the native handle, which raylib does
    # not re-export.
    EXTRA_INCLUDES+=("-I$DEPS/raylib-src/src/external/glfw/include")
    HAVE_RAYLIB=1
fi
if [ -f "$DEPS/imgui-src/imgui.h" ]; then
    EXTRA_INCLUDES+=("-I$DEPS/imgui-src" "-I$DEPS/imgui-src/backends" "-I$DEPS/rlimgui-src")
fi
# FreeType, which both text stacks rasterise through - the UI font set directly
# and imgui through its own loader. Without this the font files land here as a
# missing-header error rather than as anything about our code.
if [ -f "$DEPS/freetype-src/include/ft2build.h" ]; then
    EXTRA_INCLUDES+=("-I$DEPS/freetype-src/include")
fi

if [ ${#FILES[@]} -eq 0 ]; then
    # cromwell first, so an engine warning is not buried under game files.
    #
    # THE UI KIT IS SPLIT ACROSS BOTH LISTS, the same way it is split across the
    # two library targets: everything under ui/ except ui/paint is headless and
    # parses without raylib, while ui/paint is the renderer half and does not.
    # Listing ui/ wholesale would emit a page of missing-header noise on a tree
    # that has not been configured yet.
    SEARCH=(src/cromwell/entities src/cromwell/spatial src/cromwell/events
            src/cromwell/ui/core src/cromwell/ui/shape src/cromwell/ui/loader
            src/cromwell/ui/gauge src/cromwell/ui/control src/cromwell/ui/panel
            src/game)
    if [ $HAVE_RAYLIB -eq 1 ]; then
        SEARCH+=(src/cromwell/camera src/cromwell/decal src/cromwell/geometry
                 src/cromwell/gpu src/cromwell/lighting src/cromwell/material
                 src/cromwell/model src/cromwell/overlay src/cromwell/platform
                 src/cromwell/post
                 src/cromwell/ribbon src/cromwell/input src/cromwell/ui/paint)
    else
        echo "note: raylib not found under $DEPS - skipping render code."
        echo "      configure the project once to fetch it."
        echo
    fi
    while IFS= read -r f; do FILES+=("$f"); done < <(
        find "${SEARCH[@]}" -name '*.cpp' 2>/dev/null | sort
    )
    # Render files cannot be parsed without raylib; drop them rather than emit
    # a page of missing-header errors that say nothing about our code.
    if [ $HAVE_RAYLIB -eq 0 ]; then
        FILTERED=()
        for f in "${FILES[@]}"; do
            case "$f" in
                src/game/render/*|src/game/picking/*|src/game/path/*) ;;
                *) FILTERED+=("$f") ;;
            esac
        done
        FILES=("${FILTERED[@]}")
    fi
fi

FLAGS=(-std=c++20 -Isrc "${EXTRA_INCLUDES[@]}" -x c++)

# ONE PROCESS PER FILE, RUN IN PARALLEL. clang-tidy has no incremental mode and
# no shared header cache between runs, so each of the ~110 files re-parses the
# whole include tree — raylib, imgui and the standard library included. Serially
# that is minutes of wall time for a few seconds of actual analysis, and a check
# nobody waits for is a check nobody runs.
#
# The work is embarrassingly parallel: no file's result depends on another's.
# Default to the core count, which takes it from minutes to seconds.
#
# --fix is the exception and runs SERIALLY. Two processes rewriting files that
# include each other's headers can interleave edits; the fixing path is rare and
# not worth the risk.
JOBS="${TIDY_JOBS:-$(nproc 2>/dev/null || echo 8)}"
if [ -n "$FIX" ]; then JOBS=1; fi

echo "clang-tidy over ${#FILES[@]} file(s), ${JOBS} at a time"
echo

# Each job writes its own file, so parallel output cannot interleave mid-line.
# Collected in the original order afterwards, so a run is reproducible rather
# than ordered by whichever process happened to finish first.
SCRATCH="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/tidy.$$")"
mkdir -p "$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

index=0
for f in "${FILES[@]}"; do
    (
        out="$("$TIDY" $FIX "$f" -- "${FLAGS[@]}" 2>&1)"
        # The "N warnings generated / suppressed in non-user code" trailer is
        # noise when there is nothing else; only report a file that had
        # something to say.
        if echo "$out" | grep -qE 'warning:|error:'; then
            {
                echo "--- $f"
                echo "$out" | grep -vE '^\s*$' \
                            | grep -vE 'warnings? generated\.$' \
                            | grep -vE 'Suppressed [0-9]+ warnings' \
                            | grep -vE 'Use -header-filter|Use -system-headers'
                echo
            } > "$SCRATCH/$(printf '%04d' "$index")"
        fi
    ) &

    index=$((index + 1))

    # Block until a slot frees. `wait -n` needs bash 4.3+, which Git Bash has;
    # the fallback drains everything, which is slower but always correct.
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
        wait -n 2>/dev/null || wait
    done
done
wait

reported=0
for report in "$SCRATCH"/*; do
    [ -e "$report" ] || continue
    cat "$report"
    reported=1
done

if [ $reported -eq 0 ]; then
    echo "clean - nothing to report"
fi

# Always exit 0. This is advice, not a gate: see WarningsAsErrors in .clang-tidy.
exit 0
