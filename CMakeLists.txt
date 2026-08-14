cmake_minimum_required(VERSION 3.16)
project(XcomCpp CXX)

# C++20 for <format>. The logger and every diagnostic string in the tree are
# built with std::format rather than printf: type-safe, checked at compile
# time, and it takes a std::string without .c_str(). See src/cromwell/diag/.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- parallel compilation, and why it has to be said at all -------------
# The Visual Studio generator compiles ONE FILE AT A TIME within each project
# unless /MP is in the flags — MSBuild's own /m only parallelises across
# projects, and this tree is a near-linear chain of five, so without /MP a
# 16-core machine builds like a 1-core one. Measured on a Vec3.hpp touch:
# 36s serial, and the whole difference is this flag. Set before the deps are
# fetched so raylib, freetype and imgui compile in parallel too.
if(MSVC)
    add_compile_options(/MP)
endif()

# The app target is added once src/render exists; the tile core and its
# headless tests build with no raylib dependency at all, same split as
# FromDustPrototype's sim core.
option(XC_BUILD_APP   "Build the raylib app"       ON)
option(XC_BUILD_TESTS "Build headless tile tests"  ON)
option(XC_BUILD_WEB   "Build the CEF web surface"  ON)

# ON, but it degrades rather than fails: the Steamworks SDK cannot be fetched
# by a build — it is behind a partner login — so if third_party/steamworks is
# empty the app is built without Steam and says so at configure time. See
# cmake/steamworks.cmake and third_party/steamworks/README.md.
option(XC_BUILD_STEAM "Link Steamworks, when the SDK has been dropped in" ON)

# ---- Tracy: the frame profiler ----------------------------------------
# OFF by default, and it must stay that way. The Tracy CLIENT is cheap but not
# free — it opens a listening socket, allocates a ring buffer and stamps every
# zone — so a shipping build should not carry it. Turning it on is a deliberate
# act for a profiling session.
#
# WHY TRACY AND NOT VISUAL STUDIO'S PROFILER. Two reasons, both hard:
#   1. VS profiles a whole process run. There is no in-game trigger, so it
#      cannot answer "what happened between these two keypresses".
#   2. VS's GPU Usage tool supports Direct3D 10/11/12 and NOT OpenGL. This
#      renderer is GL 4.3, so VS can say nothing at all about the GPU.
# Tracy does per-frame CPU and OpenGL GPU zones on one aligned timeline, which
# is the only way to tell a GPU-bound frame from a CPU-bound one.
#
# The macros are wrapped in cromwell/diag/Profile.hpp so nothing else in the
# tree includes Tracy or knows whether it exists; with this OFF they compile to
# nothing.
option(XC_TRACY "Build with the Tracy profiler client" OFF)

# 480 is Spacewar, Valve's public test app. Every Steam account owns it, so
# identity, the friends list and the overlay all work against it with no app of
# our own and no partner paperwork. ONE PLACE: it reaches the code as a compile
# definition and nothing hardcodes it.
set(XC_STEAM_APPID "480" CACHE STRING "Steam app id this build is stamped with")

# ---- where the runnable product lands ---------------------------------
# ONE staging directory, not the project root. Everything the app needs at
# runtime — xcom.exe, cromwell_web_helper.exe and the whole CEF runtime — is
# written straight here by the build, so nothing is copied into the source
# tree and `builds/` can be deleted whole without touching anything else.
#
# Two levels below the root on purpose: ShaderLibrary probes "assets",
# "../assets", "../../assets", so the app finds its shaders both when run
# from here (double-clicked) and from the project root (`builds\win\xcom.exe`).
# Debug is a sibling rather than a child for the same reason - a third level
# down would put assets out of probe range.
set(XC_STAGE_DIR "${CMAKE_SOURCE_DIR}/builds/win")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY                "${XC_STAGE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${XC_STAGE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${XC_STAGE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${XC_STAGE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_SOURCE_DIR}/builds/win-debug")

# ========================================================================
# WHICH PLATFORM, AND WHICH LIBRARY INSIDE IT
#
# The layout is TWO AXES, in this order:
#
#   src/cromwell/platform/            the interfaces, and anything portable
#   src/cromwell/platform/pc/         Windows, Linux and macOS together
#   src/cromwell/platform/pc/raylib/  the third-party library backing them
#   src/cromwell/platform/ps5/...     a folder per console
#
#   src/cromwell/rhi/                 the graphics interfaces
#   src/cromwell/rhi/pc/opengl/       the PC graphics backend
#   src/cromwell/rhi/ps5/...
#
# WHY PLATFORM FIRST AND LIBRARY SECOND. They are different questions with
# different lifetimes. The platform decides what a window, a save file and a
# gamepad even ARE - and "pc" genuinely covers all three desktops, because the
# only difference between them is where the per-user directory lives. The
# library is a dependency we chose and might change WITHOUT changing platform:
# replacing raylib with a hand-rolled GL layer is `pc/opengl/` beside
# `pc/raylib/`, not a new platform.
#
# WHY THE SELECTION IS HERE AND NOT AN #ifdef. A console build must not CONTAIN
# desktop code, not merely avoid calling it - the sources are chosen, so the
# other platform's files are never handed to a compiler. It also means the
# boundary cannot rot: platform code that leaked into a shared file fails to
# build the moment a second platform is configured, rather than years later.
#
# One backend defines IPlatform::create. Two selected at once is a duplicate
# symbol at link time, which is the right way to find that out.
set(XC_PLATFORM "pc" CACHE STRING "Target platform folder: pc, ps5, ...")
set(XC_PC_BACKEND "raylib" CACHE STRING "Third-party library backing the pc platform")

if(XC_PLATFORM STREQUAL "pc")
    # PC-specific but library-agnostic: std::filesystem covers all three
    # desktops and only the per-user directory differs. Headless, so it sits in
    # cromwell_base with the rest of the code a test can run without a window.
    set(XC_PLATFORM_HEADLESS_SOURCES
        src/cromwell/platform/pc/PcFileSystem.cpp)

    set(XC_PLATFORM_SOURCES
        src/cromwell/platform/pc/${XC_PC_BACKEND}/RaylibPlatform.cpp
        src/cromwell/platform/pc/${XC_PC_BACKEND}/RaylibSurface.cpp
        src/cromwell/platform/pc/${XC_PC_BACKEND}/RaylibInput.cpp
        src/cromwell/platform/pc/${XC_PC_BACKEND}/RaylibImageDecoder.cpp)

    set(XC_RHI_SOURCES
        src/cromwell/rhi/pc/opengl/OpenGlRenderDevice.cpp)
else()
    message(FATAL_ERROR
        "XC_PLATFORM '${XC_PLATFORM}' has no source set. Add one beside the pc "
        "block above, implementing ISurface, IInput, IFileSystem, IImageDecoder, "
        "IPlatform and rhi::IRenderDevice in src/cromwell/platform/${XC_PLATFORM}/ "
        "and src/cromwell/rhi/${XC_PLATFORM}/.")
endif()

message(STATUS "Platform: ${XC_PLATFORM} (backend: ${XC_PC_BACKEND})")

# ========================================================================
# FOUR TARGETS, AND ONE RULE BETWEEN THEM
#
#   cromwell_base -> nothing        the engine's headless half
#   cromwell      -> raylib         the engine's renderer
#   game_core     -> cromwell_base  this game's simulation, headless
#   game          -> cromwell       this game's renderer and app
#
# THE RULE IS THAT THE ARROWS NEVER REVERSE. cromwell may not include, link
# against, or know the name of anything under src/game. That is what makes it
# liftable into the next project, and it is checked rather than trusted:
#
#     cmake --build . --target cromwell
#
# builds the engine with no game target in the graph at all, so a stray
# #include "game/..." is a compile error rather than something that quietly
# works here and fails on the day it is copied out.
#
# The split into _base and the rest, on both sides, is the pre-existing
# headless discipline kept intact: the simulation and the event bus are
# testable without a window, and a test binary that has to open one is a test
# nobody runs on CI.
# ========================================================================

# ---- cromwell_base: the engine, minus everything that needs a GPU ------
# Logging and the event bus. Neither draws anything, both are wanted by the
# headless tests, and pulling raylib in for them would put a window between
# the test runner and the assertion.
add_library(cromwell_base STATIC
    src/cromwell/diag/Logger.cpp
    src/cromwell/events/Crier.cpp
    src/cromwell/entities/Entity.cpp
    src/cromwell/entities/EntityProps.cpp
    src/cromwell/diag/Profiler.cpp
    src/cromwell/spatial/SpatialHash.cpp
    # Easing curves and the two things that drive a value along one. Pure
    # arithmetic over a float and a clock, wanted by the UI, by the camera and
    # by anything that moves - so it sits with the vector types rather than
    # behind the renderer. See math/Easing.hpp.
    src/cromwell/math/curve/Easing.cpp
    src/cromwell/math/curve/Tween.cpp
    # ---- the portable half of the platform layer ------------------------
    # HERE RATHER THAN IN A PLATFORM FOLDER, because it needs no platform.
    #
    # FrameClock is the frame clock for EVERY target: the clamp, the pause, the
    # resume skip and the scaling are arithmetic, and the one platform question
    # ("what time is it") is answered portably by std::chrono::steady_clock. A
    # per-platform clock would mean one copy of the hitch clamp per target,
    # which is the single rule whose absence turns a stall into a unit
    # teleporting through a wall - one chance to leave it out per port, in the
    # least-tested code on the least-tested platform.
    src/cromwell/platform/FrameClock.cpp
    ${XC_PLATFORM_HEADLESS_SOURCES}
    src/cromwell/settings/Settings.cpp
    src/cromwell/net/WinHttpClient.cpp
    src/cromwell/steam/SteamAvatar.cpp
    # Pointer arbitration and gestures. HERE RATHER THAN IN cromwell because
    # neither reads a device: one collects claims about who owns the cursor,
    # the other separates a click from a drag given three booleans. That is
    # what makes both testable without a window - see xcom_pointer_tests. The
    # projection helpers they pair with DO need the camera's matrices and so
    # live with the renderer; see camera/Viewport.hpp.
    # The key and mouse BINDINGS. Moved here from the renderer once it stopped
    # calling IsKeyPressed directly - it now reads IInput, IClock and ISurface,
    # so it needs no window and the bindings became testable without one. That
    # move is the point of the platform interfaces, made concrete: thirty-six
    # raylib calls in the single densest non-render file, gone.
    src/cromwell/input/InputHandler.cpp
    src/cromwell/input/PointerDrag.cpp
    src/cromwell/input/PointerFocus.cpp
    # ---- collision queries ---------------------------------------------
    # Layers, swept-shape contacts and the grid traversal that walks them.
    # HEADLESS ON PURPOSE, and it is the whole reason the trace layer is
    # worth having: "does a box sweeping this way stop at that cell, and
    # exactly where" is arithmetic, so it is a test rather than something
    # somebody has to see go wrong in the game. The traversal and the hit
    # buffer are header-only templates; only these two have code that is not
    # inlined into a caller. See collision/GridTrace.hpp.
    src/cromwell/collision/Intersect.cpp
    src/cromwell/collision/LayerMatrix.cpp
    # Debug geometry, the QUEUE half. Here rather than with the renderer for
    # the reason that makes the whole facility worth having: the code that
    # most needs to draw - a pathfinder, a trace, a steering behaviour - is
    # headless and has no renderer to ask. See debug/DebugDraw.hpp.
    src/cromwell/debug/DebugDraw.cpp

    # ---- the UI widget kit, geometry half -----------------------------
    # HERE RATHER THAN IN cromwell BECAUSE IT DRAWS NOTHING. Every widget
    # below turns (rect, spec, state) into triangles and text runs in a
    # draw list; the painter that uploads them lives with the renderer.
    # That split is what makes "does the segment ring leave a gap between
    # chips" an assertion about vertex positions rather than a screenshot
    # somebody has to look at - see xcom_ui_tests, and the header comment
    # in ui/core/UiDrawList.hpp.
    src/cromwell/ui/core/UiContext.cpp
    src/cromwell/ui/core/UiDrawList.cpp
    src/cromwell/ui/core/UiText.cpp
    src/cromwell/ui/shape/Glow.cpp
    src/cromwell/ui/shape/Outline.cpp
    src/cromwell/ui/shape/Shapes.cpp
    src/cromwell/ui/loader/ActivitySpinner.cpp
    src/cromwell/ui/loader/LoadingBar.cpp
    src/cromwell/ui/loader/LoadingRing.cpp
    src/cromwell/ui/gauge/SegmentBar.cpp
    src/cromwell/ui/gauge/SegmentRing.cpp
    src/cromwell/ui/control/BorderButton.cpp
    src/cromwell/ui/control/Label.cpp
    src/cromwell/ui/control/SettingSlider.cpp
    src/cromwell/ui/control/SettingStepper.cpp
    src/cromwell/ui/control/TextButton.cpp
    src/cromwell/ui/control/WipeFill.cpp
    src/cromwell/ui/panel/BlurPanel.cpp
    src/cromwell/ui/panel/TipPanel.cpp
)
target_include_directories(cromwell_base PUBLIC src)

# ---- Tracy client ------------------------------------------------------
# Attached to cromwell_base rather than to the renderer, because the simulation
# wants zones too — a frame spike in pathfinding is as interesting as one in a
# shadow pass, and game_core links this. The GPU half needs GL and therefore
# lives with the renderer; see cromwell/diag/ProfileGpu.hpp.
#
# PUBLIC on both counts. TracyClient's headers are in cromwell's own interface
# through Profile.hpp, and XC_HAVE_TRACY decides what that header expands to —
# a definition set for the engine but not for its users is how two translation
# units end up disagreeing about what a macro does. Same reasoning as
# XC_HAVE_STEAM and XC_HAVE_WEB further down.
set(XC_TRACY_ENABLED 0)
if(XC_TRACY)
    include(FetchContent)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.11.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(tracy)
    target_link_libraries(cromwell_base PUBLIC TracyClient)
    set(XC_TRACY_ENABLED 1)
    message(STATUS "Tracy: profiler client ON - run the Tracy server to connect")
endif()
target_compile_definitions(cromwell_base PUBLIC XC_HAVE_TRACY=${XC_TRACY_ENABLED})

# WinHTTP is the engine's HTTP transport - part of Windows, so this is a link
# line rather than a dependency. See cromwell/net/HttpClient.hpp.
if(WIN32)
    target_link_libraries(cromwell_base PUBLIC winhttp)
endif()
if(NOT MSVC)
    target_link_libraries(cromwell_base PUBLIC m)
endif()

# ---- game_core: the tactical simulation, pure C++, no raylib -----------
set(GAME_CORE_SOURCES
    src/game/lattice/Lattice.cpp
    src/game/world/World.cpp
    src/game/world/OcclusionGrid.cpp
    src/game/world/authoring/MapAuthor.cpp
    src/game/world/ramp/RampValidator.cpp
    src/game/world/ramp/RampSupport.cpp
    src/game/world/authoring/DemoMapFactory.cpp
    src/game/query/Terrain.cpp
    # The trace against the tile world. HEADLESS, and worth being: it names no
    # raylib type, so "does a sweep stop where it should in a real world" is a
    # test rather than something to notice in the game. The pickers that wrap it
    # take a raylib Ray and stay with the app. See game/picking/WorldTrace.hpp.
    src/game/picking/WorldTrace.cpp
    src/game/query/Standability.cpp
    src/game/query/BlockedMass.cpp
    src/game/query/cover/LedgeCover.cpp
    src/game/query/LadderQuery.cpp
    src/game/query/cover/CoverModel.cpp
    src/game/movement/occupancy/ColumnScanner.cpp
    src/game/movement/occupancy/OccupancyGrid.cpp
    src/game/movement/graph/InfantryMoveGraph.cpp
    src/game/movement/graph/VehicleMoveGraph.cpp
    src/game/movement/search/ReachField.cpp
    src/game/movement/search/Pathfinder.cpp
    src/game/movement/search/PathReconstructor.cpp
    src/game/units/kinds/Unit.cpp
    src/game/units/UnitFactory.cpp
    src/game/components/Components.cpp
    src/game/units/roster/UnitRoster.cpp
    src/game/units/roster/DemoRosterFactory.cpp
    src/game/units/roster/OccupancyMaskBuilder.cpp
    src/game/light/SunBaker.cpp
    src/game/light/SunBakeBenchmark.cpp
    src/game/perf/PerfBenchmark.cpp
    src/game/light/RoomPartition.cpp
    src/game/los/RayCaster.cpp
    src/game/los/PeekFinder.cpp
    src/game/los/EyeSet.cpp
    src/game/los/VisibilityField.cpp
    src/game/los/VisibilityComputer.cpp
    src/game/border/band/Band.cpp
    src/game/border/band/BandBuilder.cpp
    src/game/border/band/BandConnectivity.cpp
    src/game/border/band/BandExtractor.cpp
    src/game/border/loop/LoopSet.cpp
    src/game/border/loop/EdgeCapHeight.cpp
    src/game/border/loop/LoopPolyliner.cpp
    src/game/ui/state/UIStateMachine.cpp
)

add_library(game_core STATIC ${GAME_CORE_SOURCES})
target_link_libraries(game_core PUBLIC cromwell_base)

# ---- headless test target (no raylib dependency) -----------------------
if(XC_BUILD_TESTS)
    add_executable(xcom_tests tests/TileCoreTests.cpp)
    target_link_libraries(xcom_tests PRIVATE game_core)

    # The event bus gets its own binary rather than more cases in the tile
    # tests: it shares no fixture with them, and a bus bug should name itself.
    # It tests cromwell, not the game, so it links the engine's base alone.
    add_executable(xcom_crier_tests tests/CrierTests.cpp)
    target_link_libraries(xcom_crier_tests PRIVATE cromwell_base)

    # The UI state machine draws nothing and needs no window, so it is tested
    # the same way the simulation is.
    add_executable(xcom_ui_tests tests/UIStateTests.cpp)
    target_link_libraries(xcom_ui_tests PRIVATE game_core)

    # The easing curves and the two things that drive a value along one. Their
    # shape is a judgement and is checked by looking; what is tested here is
    # the set of properties that make them usable at all - endpoints landing
    # exactly, no extrapolation past the ends, and a tween that arrives rather
    # than settling a fraction short. See tests/EasingTests.cpp.
    add_executable(xcom_easing_tests tests/EasingTests.cpp)
    target_link_libraries(xcom_easing_tests PRIVATE cromwell_base)

    # The UI widget kit. Its geometry half produces a draw list rather than
    # pixels, which is the whole reason it can be tested at all - "does the
    # segment ring's fill mirror its track" is an assertion about vertex
    # counts. What it CANNOT answer is whether any of it looks right; that is
    # the gallery on F2. See tests/WidgetTests.cpp.
    add_executable(xcom_widget_tests tests/WidgetTests.cpp)
    target_link_libraries(xcom_widget_tests PRIVATE cromwell_base)

    # The entity/component plumbing is cromwell's and needs no window. Its
    # think accumulator is invisible from outside, so it is tested rather than
    # eyeballed.
    add_executable(xcom_entity_tests tests/EntityTests.cpp)
    target_link_libraries(xcom_entity_tests PRIVATE cromwell_base)

    # The service container and the settings bag the engine reads its own
    # defaults through.
    add_executable(xcom_services_tests tests/ServicesTests.cpp)
    target_link_libraries(xcom_services_tests PRIVATE cromwell_base)

    # The engine's spatial index. Checked against a brute-force scan rather
    # than against constants - it is an optimisation, so agreeing with the loop
    # it replaces is the entire specification.
    add_executable(xcom_spatial_tests tests/SpatialHashTests.cpp)
    target_link_libraries(xcom_spatial_tests PRIVATE cromwell_base)

    # When a second camera actually redraws. The only headless part of scene
    # capture and the part most worth pinning: capturing every frame instead
    # of five times a second is a performance bug with no visual symptom at
    # all. See tests/CaptureTests.cpp.
    add_executable(xcom_capture_tests tests/CaptureTests.cpp)
    target_link_libraries(xcom_capture_tests PRIVATE cromwell_base)

    # Debug drawing, minus the drawing. Most of this subsystem is judged by
    # looking at it; what is tested is the part with no visible symptom - the
    # one-frame default lifetime, the overflow report, and the capsule's cap
    # centres agreeing with TraceShape so the picture cannot lie about the
    # shape that was swept. See tests/DebugDrawTests.cpp.
    add_executable(xcom_debug_tests tests/DebugDrawTests.cpp)
    target_link_libraries(xcom_debug_tests PRIVATE cromwell_base)

    # The direction and rotation vocabulary. Header-only, so this links the
    # engine base only to get the include path. What it pins down is the
    # degenerate cases - a zero-length forward, an exactly-opposed pair, acos
    # of 1.0000001 - every one of which produces NaN or a plausibly-wrong
    # orientation rather than a crash. See tests/MathTests.cpp.
    add_executable(xcom_math_tests tests/MathTests.cpp)
    target_link_libraries(xcom_math_tests PRIVATE cromwell_base)

    # The backend interface, checked by implementing it. Two things, both at
    # BUILD time rather than at run time:
    #
    #   - a null backend overrides every pure virtual in IRenderDevice and
    #     ICommandEncoder, so a method added to either without a thought for
    #     what a real backend does with it fails here, on the machine of
    #     whoever added it - not months later on a console port under NDA;
    #
    #   - it links cromwell_base, which NEVER SEES raylib. So the RHI headers
    #     compiling in this target is the check that they have not quietly
    #     acquired a graphics API. Same arrangement as the engine's one
    #     architectural rule above: checked, not trusted.
    add_executable(xcom_rhi_tests tests/RhiTests.cpp)
    target_link_libraries(xcom_rhi_tests PRIVATE cromwell_base)

    # The platform interfaces - surface, input, clock, storage, image decode -
    # checked the same way and for the same two reasons: a null backend must be
    # able to satisfy every one of them, and linking cromwell_base (which never
    # sees raylib) is what proves the platform layer has not re-acquired a
    # windowing library.
    #
    # The null backend is not only a test fixture. It is what a headless replay
    # verifier, an asset cooker or a CI run of the simulation needs, and it
    # falls out of writing the interfaces honestly.
    add_executable(xcom_platform_tests tests/PlatformTests.cpp)
    target_link_libraries(xcom_platform_tests PRIVATE cromwell_base)

    # The trace against a real tile world - the half of the trace layer that
    # only exists once there are tiles. Links game_core rather than the engine,
    # because a floor, a wall and a ramp are this game's shapes.
    add_executable(xcom_trace_tests tests/TraceTests.cpp)
    target_link_libraries(xcom_trace_tests PRIVATE game_core)

    # The trace layer: layers and responses, swept-shape contacts, the grid
    # traversal, and the multi-hit buffer. Every number in it is exactly
    # knowable, and every failure it catches ("off by a half-extent", "off by
    # a cell") shows up in play as something vague. See tests/CollisionTests.cpp.
    add_executable(xcom_collision_tests tests/CollisionTests.cpp)
    target_link_libraries(xcom_collision_tests PRIVATE cromwell_base)

    # The pointer helpers - who owns the cursor, what it is hovering, and
    # whether that gesture was a click or a drag. All three are state machines
    # over booleans and a screen position, which is what lets the cases nobody
    # can see happening be pinned down here. See tests/PointerTests.cpp.
    add_executable(xcom_pointer_tests tests/PointerTests.cpp)
    target_link_libraries(xcom_pointer_tests PRIVATE cromwell_base)

    # The profiler's capture file. Checked for STRUCTURE, not for timings: the
    # failure that matters is a trace Perfetto refuses to open, and it is
    # otherwise discovered at the end of a profiling session.
    add_executable(xcom_profiler_tests tests/ProfilerTests.cpp)
    target_link_libraries(xcom_profiler_tests PRIVATE cromwell_base)

    # The avatar fetch. Registered with NO arguments, which runs only the
    # offline half - see the file header. `--live <steamid64>` exercises the
    # real transport and is a manual check, deliberately not in ctest.
    add_executable(xcom_steam_tests tests/SteamAvatarTests.cpp)
    target_link_libraries(xcom_steam_tests PRIVATE cromwell_base)

    # The simulation perf benchmark. NOT registered with ctest: it asserts
    # nothing and its output is numbers to read, so a test runner would only
    # ever report "passed" and hide them. Run it by hand, before and after a
    # change to the queries it times. See src/game/perf/PerfBenchmark.hpp.
    add_executable(xcom_perf tests/PerfMain.cpp)
    target_link_libraries(xcom_perf PRIVATE game_core)

    # The test binaries stay in the build tree. builds/win is the runnable
    # product - what you would zip and hand to someone - and test exes are
    # not part of it.
    set_target_properties(xcom_tests xcom_crier_tests xcom_ui_tests xcom_widget_tests xcom_easing_tests xcom_entity_tests xcom_services_tests xcom_steam_tests xcom_spatial_tests xcom_pointer_tests xcom_collision_tests xcom_trace_tests xcom_math_tests xcom_rhi_tests xcom_platform_tests xcom_debug_tests xcom_capture_tests xcom_profiler_tests xcom_perf PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/tests")

    enable_testing()
    add_test(NAME tile  COMMAND xcom_tests)
    add_test(NAME crier COMMAND xcom_crier_tests)
    add_test(NAME ui     COMMAND xcom_ui_tests)
    add_test(NAME widget COMMAND xcom_widget_tests)
    add_test(NAME easing COMMAND xcom_easing_tests)
    add_test(NAME entity   COMMAND xcom_entity_tests)
    add_test(NAME services COMMAND xcom_services_tests)
    add_test(NAME steam    COMMAND xcom_steam_tests)
    add_test(NAME spatial  COMMAND xcom_spatial_tests)
    add_test(NAME pointer  COMMAND xcom_pointer_tests)
    add_test(NAME collision COMMAND xcom_collision_tests)
    add_test(NAME trace     COMMAND xcom_trace_tests)
    add_test(NAME math      COMMAND xcom_math_tests)
    add_test(NAME rhi       COMMAND xcom_rhi_tests)
    add_test(NAME platform  COMMAND xcom_platform_tests)
    add_test(NAME debugdraw COMMAND xcom_debug_tests)
    add_test(NAME capture   COMMAND xcom_capture_tests)
    add_test(NAME profiler COMMAND xcom_profiler_tests)
endif()

# ---- app ---------------------------------------------------------------
if(XC_BUILD_APP)
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)

    # ---- OpenGL 4.3, for one feature: cubemap ARRAYS -------------------
    # The reflection probes are per-room, and the shader picks between them
    # per-pixel, so every probe has to be reachable from one draw call. That
    # means samplerCubeArray, which is GL 4.0 — GL 3.3 caps out at four probes
    # bound to four texture units, and a building has more rooms than that.
    #
    # Cheaper than it sounds. GLSL 330 shaders compile unchanged in a 4.3 core
    # context, so only the surface shader's #version moves; everything else in
    # assets/shaders stays where it is.
    set(OPENGL_VERSION "4.3" CACHE STRING "" FORCE)

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        5.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(raylib)

    # JPEG, which raylib leaves OFF by default (see its config.h: the line is
    # commented out, and rtextures.c defines STBI_NO_JPEG unless this is set).
    # Steam serves avatars as .jpg, so without this LoadImageFromMemory returns
    # an empty image and the only symptom is a blank avatar.
    target_compile_definitions(raylib PRIVATE SUPPORT_FILEFORMAT_JPG=1)

    # ---- FreeType: the rasteriser both text stacks share ----------------
    # WHY, when raylib and ImGui both already rasterise with stb_truetype.
    # stb_truetype does not hint. At the sizes a UI actually uses — 10 to 16 px,
    # where a stem is one or two pixels wide — an unhinted outline puts that
    # stem wherever the curve happens to fall, so it lands as one dark pixel or
    # two grey ones depending on nothing the reader can see. FreeType's light
    # hinting snaps stems and baselines to the pixel grid VERTICALLY only,
    # leaving horizontal metrics untouched, which is what keeps letter spacing
    # even while making every stem the same weight. That is the difference
    # between "smooth" and "crisp", and it is the half that rasterising at the
    # right size does not fix on its own.
    #
    # Every optional dependency off. FreeType can pull in zlib, libpng, brotli
    # and HarfBuzz; none of them matter for TrueType and OpenType outlines, and
    # each is another thing to build on a fresh machine.
    set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG        VER-2-13-3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(freetype)

    # The name CMake's own FindFreetype module hands out, which is what other
    # vendored projects look for. msdfgen asks `if(NOT TARGET
    # Freetype::Freetype)` before falling back to `find_package(Freetype
    # REQUIRED)` — and that fallback cannot succeed here, because our FreeType
    # was fetched and built rather than installed, so there is no package on
    # disk to find. Declaring the alias makes the question answer itself.
    #
    # FreeType does create a `freetype-interface` target carrying this as its
    # EXPORT_NAME, but an export name only exists for consumers of an installed
    # package; inside this build the target is still called `freetype`.
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    # ---- msdf-atlas-gen: the distance field baker -----------------------
    # A BUILD-TIME TOOL, NOT A RUNTIME DEPENDENCY. Nothing in the engine links
    # this. It builds an executable that tools/fonts/bake_msdf.py drives to turn a
    # font into an MSDF atlas, exactly the way tools/fonts/gen_fa_icons.py drives the
    # icon vocabulary — the output is generated, gitignored and one command
    # away, and the shipped binary carries only the PNG it produced.
    #
    # Generating a multi-channel field is not something to reimplement: it
    # needs the edge-colouring pass from Chlumsky's thesis (see
    # study/topics/surfaces/distance_fields.md) and this is that thesis's own library.
    #
    # Every option here is switched off something that is ON by default and
    # would otherwise want a package manager or a second graphics library:
    # vcpkg on both projects, and Skia, which msdfgen uses only for a geometry
    # preprocessing pass that TrueType outlines do not need.
    set(MSDF_ATLAS_USE_VCPKG      OFF CACHE BOOL "" FORCE)
    set(MSDF_ATLAS_USE_SKIA       OFF CACHE BOOL "" FORCE)
    set(MSDF_ATLAS_BUILD_STANDALONE ON CACHE BOOL "" FORCE)
    set(MSDF_ATLAS_INSTALL        OFF CACHE BOOL "" FORCE)
    # DYNAMIC, matching everything else in this build. Both projects default to
    # linking the static CRT, and mixing the two is not a warning — it is
    # `unresolved external symbol __imp_getenv` out of FreeType at link time,
    # because a /MD object expects the import-library form of the CRT and a /MT
    # link does not provide it. Anything sharing our FreeType has to share our
    # runtime.
    set(MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
    set(MSDFGEN_DYNAMIC_RUNTIME    ON CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_VCPKG         OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_SKIA          OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_BUILD_STANDALONE  OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_INSTALL           OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_DISABLE_SVG       ON  CACHE BOOL "" FORCE)

    # No libpng, and therefore no zlib either. msdf-atlas-gen writes BMP with
    # its own code, raylib loads BMP natively, and the atlas is a generated
    # intermediate that nothing ships compressed — so the only thing PNG would
    # buy is a smaller file in a directory that is already gitignored, at the
    # cost of two more libraries to build on a fresh machine. BMP also stays
    # openable in any image viewer, which matters the first time a bake looks
    # wrong and the question is whether the atlas or the shader is at fault.
    set(MSDFGEN_DISABLE_PNG       ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(
        msdf_atlas_gen
        GIT_REPOSITORY https://github.com/Chlumsky/msdf-atlas-gen.git
        GIT_TAG        v1.3
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(msdf_atlas_gen)

    # Keep the baker OUT of the staging directory. Every runtime output in this
    # build goes to builds/win, which is the shipping product — and a build-time
    # tool is not part of it. Left alone it lands beside xcom.exe and looks for
    # all the world like something the game needs.
    if(TARGET msdf-atlas-gen-standalone)
        set_target_properties(msdf-atlas-gen-standalone PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/tools"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tools"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/tools")
    endif()

    # ---- Dear ImGui, on raylib's own backend ---------------------------
    # Neither repository ships a CMakeLists worth using — imgui has none at
    # all, and rlImGui's builds its examples — so both are fetched as plain
    # sources and compiled into one static library here. SOURCE_SUBDIR names a
    # directory that does not exist, which is the documented way to say
    # "populate, do not add_subdirectory".
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.9b
        GIT_SHALLOW    TRUE
    )
    # Pinned to a commit, not to main: rlImGui tracks ImGui's backend API
    # closely — it already requires the 1.92 texture interface — and an
    # unpinned branch would silently step past whatever ImGui tag is above.
    FetchContent_Declare(
        rlimgui
        GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
        GIT_TAG        db8239140a68bf9a07f4499d2875d7cefa8fa7e6
        SOURCE_SUBDIR  no-cmake-here
    )
    FetchContent_MakeAvailable(imgui rlimgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${rlimgui_SOURCE_DIR}/rlImGui.cpp
        # The FreeType loader is a source file in imgui's tree rather than a
        # separate library, and IMGUI_ENABLE_FREETYPE below makes it the
        # default rasteriser rather than something to select at runtime.
        ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${rlimgui_SOURCE_DIR})
    target_link_libraries(imgui PUBLIC raylib freetype)

    # PUBLIC, not PRIVATE. This definition changes what ImFontAtlas uses as its
    # default font loader, and a translation unit that disagreed with the one
    # imgui.cpp was built with would be reasoning about a differently-shaped
    # atlas. Same argument as XC_HAVE_STEAM further up.
    target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE)

    # rlImGui bundles Font Awesome 6 Free, compiled in, and merges it into the
    # default font during setup. We ship Font Awesome Pro 7 instead, whose
    # codepoints the generated cromwell/ui/FontAwesomeIcons.hpp names — so its
    # copy is switched off rather than left to merge underneath ours. Two
    # reasons, and the second is the one that matters: two icon faces in one
    # atlas means whichever loads first silently answers for the codepoints
    # they share, and rlImGui's ICON_FA_* macros are FA6's, which would collide
    # with our header at the preprocessor. The switch removes both.
    target_compile_definitions(imgui PUBLIC NO_FONT_AWESOME)

    # ---- cromwell: the engine's renderer -------------------------------
    # NOTHING IN THIS LIST MAY NAME THE GAME. Everything here answers a
    # question that is true of any 3D project — how to load a shader, capture a
    # cubemap, tone-map an HDR buffer, drive a camera rig. What the pixels
    # depict is game/'s business.
    #
    # It links raylib PUBLICly because its headers do: Shader, Mesh, Camera3D
    # and Color are in cromwell's own interface, so anything using cromwell
    # needs raylib's include path anyway. imgui is deliberately NOT here — the
    # dev panel is the game's, and an engine that drags a UI toolkit into every
    # project that embeds it has made a choice on that project's behalf.
    add_library(cromwell STATIC
        # ---- the selected platform and its graphics backend ---------------
        # Chosen at the top of this file, by name, so the other platforms' code
        # is never handed to a compiler at all. See the two-axis note there.
        ${XC_PLATFORM_SOURCES}
        ${XC_RHI_SOURCES}
        # The backend conformance suite. Takes an IRenderDevice rather than a
        # concrete one, so the same stages run against every backend the engine
        # grows - which is what turns "is the console port finished" from a
        # judgement into a list of failures to work down. Needs a live context,
        # so it is a flag on the executable rather than a ctest suite; same
        # arrangement as ComputeSelfTest and for the same reason.
        src/cromwell/rhi/RenderDeviceSelfTest.cpp
        # The frame sequence and its targets. ENGINE, not game: a shadow map is
        # a depth-only pass from the sun over the world whether that world has
        # soldiers or tanks in it. The game supplies meshes through
        # IGeometrySource and decides nothing about the order.
        src/cromwell/render/ScenePipeline.cpp
        src/cromwell/camera/OrbitCamera.cpp
        # Screen space to world space and back. With the renderer rather than
        # in cromwell_base because it has to agree with the matrices the
        # renderer actually draws through - a projection re-derived without
        # raylib's near plane would put picks fractionally out of step with
        # pixels. See the note at the top of Viewport.cpp.
        src/cromwell/camera/Viewport.cpp
        # ...and the half that needs a GPU. One loop over line segments; the
        # shapes were tessellated when they were added.
        src/cromwell/debug/DebugRenderer.cpp
        src/cromwell/decal/Decal.cpp
        src/cromwell/decal/DecalBuffer.cpp
        src/cromwell/decal/DecalRenderer.cpp
        src/cromwell/decal/DecalSet.cpp
        src/cromwell/diag/CrashHandler.cpp
        # TEMPORARY: dumps a depth texture to a BMP, for comparing the two
        # renderers' shadow maps directly. Remove with the comparison.
        src/cromwell/diag/DepthDump.cpp
        src/cromwell/diag/RaylibLogBridge.cpp
        src/cromwell/geometry/BoxMesh.cpp
        src/cromwell/geometry/MeshVertexBuffer.cpp
        src/cromwell/gpu/compute/ComputeSelfTest.cpp
        src/cromwell/gpu/compute/ComputeShader.cpp
        src/cromwell/gpu/target/CustomDepthStencil.cpp
        src/cromwell/gpu/target/DepthTarget.cpp
        src/cromwell/gpu/GL.cpp
        src/cromwell/gpu/GpuProfiler.cpp
        src/cromwell/gpu/target/HdrTarget.cpp
        # A second camera into a texture - minimaps, scope optics, security
        # feeds. It owns the target, the camera and the resolve; WHAT to draw
        # is a callback, because a scene pass is the game's. Its schedule half
        # is header-only and headless. See gpu/target/SceneCapture.hpp.
        # The three screen-space buffers a full-quality view needs beyond its
        # colour target. Sized together because a prepass, its occlusion and
        # its DBuffer at different sizes is the same bug with more steps -
        # and per view, because all three are reconstructed from one camera's
        # depth. See gpu/target/ScenePassBuffers.hpp.
        src/cromwell/gpu/target/ScenePassBuffers.cpp
        # Several cameras, made from a descriptor and driven as one. What it
        # removes is the per-camera wiring - the forwarding lambda, the phase,
        # the profiler name, the four ordered construction steps - so adding
        # the tenth camera costs what the first did. See SceneCaptureSet.hpp.
        src/cromwell/camera/Camera.cpp
        src/cromwell/camera/CameraSet.cpp
        src/cromwell/gpu/ShaderLibrary.cpp
        src/cromwell/lighting/PbrShader.cpp
        src/cromwell/lighting/ProbeSpheres.cpp
        # The rhi half of the reflection probes: the cubemap array, the volumes
        # and the round-robin schedule, naming no graphics API at all. The
        # array creation and the per-layer attach that force ReflectionProbeSet
        # below to call GL directly are both on IRenderDevice, so this file
        # does not need the exemption that one has. Deleted together at parity.
        src/cromwell/lighting/DeviceProbeSet.cpp
        src/cromwell/lighting/ReflectionProbeSet.cpp
        src/cromwell/lighting/ShadowMap.cpp
        src/cromwell/lighting/SunLight.cpp
        src/cromwell/material/DeviceMaterials.cpp
        src/cromwell/material/MaterialDefinition.cpp
        src/cromwell/material/MaterialLibrary.cpp
        src/cromwell/model/ModelAsset.cpp
        src/cromwell/model/ModelPreview.cpp
        src/cromwell/overlay/OverlayShader.cpp
        src/cromwell/overlay/TexturePreviews.cpp
        # Frames keep presenting while the OS owns the thread — the window
        # drag / live resize fix. Engine-side because "the window does not
        # freeze while dragged" is true of any project; what a frame IS stays
        # the app's, handed in as a callable. See platform/ModalLoopPump.hpp.
        src/cromwell/platform/ModalLoopPump.cpp
        src/cromwell/post/AmbientOcclusion.cpp
        src/cromwell/post/PrepassShader.cpp
        src/cromwell/post/SkyPass.cpp
        src/cromwell/post/ToneMapPass.cpp
        src/cromwell/ribbon/RibbonRenderer.cpp
        src/cromwell/ribbon/RibbonShader.cpp
        src/cromwell/ribbon/StripSet.cpp
        # The UI kit's renderer half. Everything else about the widgets is in
        # cromwell_base; these files are where a weight becomes a rasterised
        # font and a draw list becomes pixels.
        #
        # TWO SUBSYSTEMS, TWO FOLDERS. ui/text turns a font file into coverage
        # and answers "how wide is this string" — the seam the headless layout
        # measures against, and it draws nothing. ui/paint turns a UiDrawList
        # into draw calls. They were one folder until text grew a second unit;
        # see ui/paint/UiPainter.hpp and ui/text/UiFontSet.hpp.
        #
        # GlyphAtlas is here only because FreeType is linked into this target:
        # it names no graphics API and needs no window, so moving it down to
        # cromwell_base is a CMake edit rather than a rewrite the day the
        # headless half wants to measure text.
        src/cromwell/ui/text/GlyphAtlas.cpp
        src/cromwell/ui/text/UiFontSet.cpp
        src/cromwell/ui/paint/DeviceUiPainter.cpp
        src/cromwell/ui/paint/UiPainter.cpp
        src/cromwell/ui/paint/WorldAnchor.cpp
        # UNCONDITIONAL, even with no SDK on the machine. The whole of the
        # Steamworks dependency is behind #if XC_HAVE_STEAM inside this one
        # file, so a build without the SDK compiles the same source into stubs
        # and no #if reaches main.cpp or Application. See SteamClient.hpp.
        src/cromwell/steam/SteamClient.cpp
        # Font PATHS, not fonts: this resolves where Inter and Font Awesome
        # live and names nothing that rasterises them, which is what keeps the
        # engine's typeface engine-side while the ImGui binding stays with
        # ImGui. See UiFontAssets.hpp.
        src/cromwell/ui/UiFontAssets.cpp
        # Distance fields: the MSDF font table and atlas. The BAKER is a
        # build-time tool and is not linked by anything here - see the
        # msdf-atlas-gen block above and tools/fonts/bake_msdf.py.
        src/cromwell/sdf/MsdfFont.cpp
        src/cromwell/sdf/WorldText.cpp
    )
    # freetype is PRIVATE: UiFontSet rasterises with it behind an interface that
    # hands back a raylib Font, so no cromwell header names an FT_ type and
    # nothing downstream needs its include path.
    target_link_libraries(cromwell PUBLIC cromwell_base raylib)
    target_link_libraries(cromwell PRIVATE freetype)

    # ---- the one test that needs raylib's headers ----------------------
    # HERE RATHER THAN WITH THE OTHER TEST TARGETS, because `cromwell` does not
    # exist until this block. Camera construction is arithmetic over a struct
    # whose definition arrives with raylib.h, so this binary links the renderer
    # and still opens no window - it calls no raylib function at all. What it
    # pins is the fovy field meaning two different things depending on the
    # projection, which is a camera that is silently wrong and still renders.
    if(XC_BUILD_TESTS)
        add_executable(xcom_projection_tests tests/ProjectionTests.cpp)
        target_link_libraries(xcom_projection_tests PRIVATE cromwell)
        set_target_properties(xcom_projection_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/tests"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/tests"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/tests"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/tests"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/tests")
        add_test(NAME projection COMMAND xcom_projection_tests)
    endif()

    # ---- Steam ----------------------------------------------------------
    # NOT FetchContent, unlike everything above it: the SDK is behind a partner
    # login, so no build can download it. It is copied into
    # third_party/steamworks by hand, once, and this notices whether that has
    # happened — a missing SDK is a warning and a feature-less build, never a
    # configure error.
    #
    # PUBLIC definitions, both of them. cromwell/steam/SteamClient.hpp has no
    # #if in it today, but XC_HAVE_STEAM decides what the .cpp compiles to, and
    # a definition set for the engine but not for its user is how two
    # translation units end up disagreeing — the same reasoning as XC_HAVE_WEB
    # further down.
    set(XC_STEAM_ENABLED 0)
    if(XC_BUILD_STEAM)
        include("${CMAKE_SOURCE_DIR}/cmake/steamworks.cmake")
        if(XC_STEAM_FOUND)
            set(XC_STEAM_ENABLED 1)
            target_include_directories(cromwell PRIVATE "${XC_STEAM_INCLUDE_DIR}")
            target_link_libraries(cromwell PUBLIC "${XC_STEAM_LIBRARY}")
            message(STATUS "Steam: app id ${XC_STEAM_APPID}, SDK at ${XC_STEAM_INCLUDE_DIR}")
        else()
            message(WARNING
                "Steam: no SDK under third_party/steamworks - building without it. "
                "See third_party/steamworks/README.md.")
        endif()
    endif()

    # The string form exists because the app id reaches Steam as an environment
    # variable rather than as a number — see announceAppId in SteamClient.cpp —
    # and stringifying a macro at the preprocessor needs two levels of
    # indirection that are not worth writing when cmake already has the text.
    target_compile_definitions(cromwell PUBLIC
        XC_HAVE_STEAM=${XC_STEAM_ENABLED}
        XC_STEAM_APPID=${XC_STEAM_APPID}
        XC_STEAM_APPID_STRING="${XC_STEAM_APPID}")

    # ---- game: this game's renderer, rules and app shell ---------------
    add_library(game STATIC
        src/game/Application.cpp
        src/game/controllers/PlayerController.cpp
        src/game/entities/pawns/CameraPawn.cpp
        src/game/render/DrawLayers.cpp
        src/game/render/FrameRenderer.cpp
        # The renderer being built beside the one that works. Both exist during
        # the port and --renderer chooses; see RhiFrameRenderer.hpp for why the
        # conversion cannot be done in place.
        src/game/render/rhi/RhiBodies.cpp
        src/game/render/rhi/RhiFrameRenderer.cpp
        src/game/render/rhi/RhiStatics.cpp
        src/game/cli/CliOptions.cpp
        src/game/path/MoveAnimator.cpp
        src/game/path/PathPreviewBuilder.cpp
        src/game/picking/SurfacePicker.cpp
        src/game/picking/TilePicker.cpp
        src/game/picking/UnitPicker.cpp
        src/game/render/overlay/BlastFlashes.cpp
        src/game/render/dev/DecalDemo.cpp
        src/game/render/dev/DevFonts.cpp
        src/game/render/dev/DevView.cpp
        src/game/render/ribbon/GlowPass.cpp
        src/game/render/overlay/OverlayRenderer.cpp
        src/game/render/scene/ProbePlacement.cpp
        src/game/render/scene/PropSet.cpp
        src/game/render/ribbon/RibbonMeshSet.cpp
        src/game/render/splash/SplashPass.cpp
        src/game/render/splash/SplashMaps.cpp
        src/game/render/scene/StaticsMesh.cpp
        src/game/render/scene/StoreyGeometryEmitter.cpp
        src/game/render/ribbon/StripMeshBuilder.cpp
        src/game/render/scene/UnitRenderer.cpp
        src/game/render/ui/GameUi.cpp
        src/game/render/ui/SplashOverlay.cpp
        src/game/render/ui/WidgetGallery.cpp
        src/game/rules/DestructionSystem.cpp
        src/game/rules/HullCrusher.cpp
        src/game/rules/RestPlacement.cpp
        src/game/state/GameState.cpp
        src/game/state/RingSelector.cpp
    )
    target_link_libraries(game PUBLIC game_core cromwell imgui)

    add_executable(xcom src/main.cpp)
    target_link_libraries(xcom PRIVATE game)

    # steam_api64.lib is a load-time import library, so the DLL has to be beside
    # the exe or the process dies before main with a missing-DLL box rather than
    # anything the log would catch. Named as $<TARGET_FILE_DIR:xcom> rather than
    # XC_STAGE_DIR so a Debug build stages into its own output directory, same
    # as the CEF runtime. copy_if_different, so it is a no-op after the first
    # build.
    if(XC_STEAM_ENABLED)
        add_custom_command(TARGET xcom POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${XC_STEAM_DLL}" "$<TARGET_FILE_DIR:xcom>"
            COMMENT "Staging steam_api64.dll"
            VERBATIM)
    endif()

    # ---- the web surface ------------------------------------------------
    # In its own subdirectory because CEF's cmake rewrites compiler and linker
    # flags wholesale, and a subdirectory is the cheapest way to keep that
    # inside its own scope rather than over the whole project.
    #
    # The surface is cromwell's, not the game's: an embedded browser is a
    # render target like any other. XC_HAVE_WEB is PUBLIC because the game's
    # Application.hpp guards its members on it, and a definition that is set
    # for the engine but not its user is how you get two translation units
    # disagreeing about a class layout.
    if(XC_BUILD_WEB)
        add_subdirectory(src/cromwell/web)
        target_link_libraries(cromwell PUBLIC cromwell_web)
        target_compile_definitions(cromwell PUBLIC XC_HAVE_WEB=1)
    endif()

    # WHERE THIS RENDERER REACHES PAST rlgl. raylib bundles glad and declares
    # its loaders `extern`, so including raylib's own header links against the
    # pointers raylib already resolved at InitWindow rather than loading GL a
    # second time. This include path is what makes that reachable.
    #
    # This used to read "keep it to ReflectionProbeSet.cpp, and the day that
    # stops being true is the day this renderer owns a GL backend it did not
    # mean to own." That rule held while the exception was one file wanting
    # cubemap arrays. Compute ended it: rlgl wraps compute programs, dispatch,
    # SSBOs and image load/store, but not glMemoryBarrier — and a dispatch
    # without a barrier is a race that looks correct on the machine it was
    # written on. That call has to come from somewhere.
    #
    # So the rule is now a boundary rather than an exception:
    #
    #   src/cromwell/gpu/GL.hpp is the ONLY file that includes glad.h.
    #   Everything rlgl wraps goes through rlgl. Everything it does not gets
    #   declared in GL.hpp with a comment naming the pass that needs it.
    #
    # One owned door, because the second and third exception would each have
    # been argued exactly as well as the first, and nobody would be counting.
    #
    # PUBLIC, because GL.hpp is a cromwell header and the door has to be open
    # to whoever walks through it.
    target_include_directories(cromwell PUBLIC ${raylib_SOURCE_DIR}/src/external)

    # GLFW's headers, for exactly one file: platform/ModalLoopPump.cpp needs
    # the refresh callback and the native window handle, neither of which
    # raylib re-exports. PRIVATE, unlike the external/ path above — no cromwell
    # HEADER names a GLFW type, so nothing downstream needs this, and keeping
    # it private is what stops GLFW quietly becoming part of the engine's
    # interface. The symbols themselves are already in raylib's static lib.
    target_include_directories(cromwell PRIVATE ${raylib_SOURCE_DIR}/src/external/glfw/include)

    if(WIN32 AND MSVC)
        set_target_properties(xcom PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
        target_link_options(xcom PRIVATE $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)

        # dbghelp is the crash handler's stack walker and minidump writer, and
        # the crash handler is cromwell's.
        target_link_libraries(cromwell PUBLIC dbghelp)

        # DEBUG INFO IN A RELEASE BUILD, deliberately. Without a .pdb the crash
        # handler can only log "xcom.exe+0x4f21a" and the minidump opens onto a
        # wall of addresses; with one, both name the function, file and line.
        # This changes no code generation - /O2 still applies - and the symbols
        # ship in a separate file, so the exe is the same size and no slower.
        # /OPT:REF /OPT:ICF restore the folding that /DEBUG would otherwise
        # switch off.
        #
        # /Z7 rather than /Zi, and not by preference: /Zi has the compiler write
        # a shared vc143.pdb through mspdbsrv.exe, and when mspdbsrv cannot start
        # on this machine every source file dies on C1041 "cannot open program
        # database". /Z7 puts the debug records in the .obj files instead, so no
        # compile-time pdb and no server; the linker still writes xcom.pdb from
        # them under /DEBUG below, which is the file the crash handler wants.
        # Costs some .obj/.lib size and nothing else.
        foreach(t xcom game cromwell game_core cromwell_base)
            target_compile_options(${t} PRIVATE $<$<CONFIG:Release>:/Z7>)
        endforeach()
        target_link_options(xcom PRIVATE
            $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()
    # No POST_BUILD copy into the project root any more: the linker already
    # writes xcom.exe into XC_STAGE_DIR, so there is exactly one copy of it
    # and no way for the root one to go stale behind a locked file.
endif()
