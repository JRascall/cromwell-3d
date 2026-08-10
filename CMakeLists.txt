cmake_minimum_required(VERSION 3.16)
project(XcomCpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
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
    src/cromwell/settings/Settings.cpp
    src/cromwell/net/WinHttpClient.cpp
    src/cromwell/steam/SteamAvatar.cpp
)
target_include_directories(cromwell_base PUBLIC src)

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
    src/game/world/authoring/MapAuthor.cpp
    src/game/world/ramp/RampValidator.cpp
    src/game/world/ramp/RampSupport.cpp
    src/game/world/authoring/DemoMapFactory.cpp
    src/game/query/Terrain.cpp
    src/game/query/Standability.cpp
    src/game/query/BlockedMass.cpp
    src/game/query/cover/LedgeCover.cpp
    src/game/query/LadderQuery.cpp
    src/game/query/cover/CoverModel.cpp
    src/game/movement/occupancy/ColumnScanner.cpp
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

    # The entity/component plumbing is cromwell's and needs no window. Its
    # think accumulator is invisible from outside, so it is tested rather than
    # eyeballed.
    add_executable(xcom_entity_tests tests/EntityTests.cpp)
    target_link_libraries(xcom_entity_tests PRIVATE cromwell_base)

    # The service container and the settings bag the engine reads its own
    # defaults through.
    add_executable(xcom_services_tests tests/ServicesTests.cpp)
    target_link_libraries(xcom_services_tests PRIVATE cromwell_base)

    # The avatar fetch. Registered with NO arguments, which runs only the
    # offline half - see the file header. `--live <steamid64>` exercises the
    # real transport and is a manual check, deliberately not in ctest.
    add_executable(xcom_steam_tests tests/SteamAvatarTests.cpp)
    target_link_libraries(xcom_steam_tests PRIVATE cromwell_base)

    # The test binaries stay in the build tree. builds/win is the runnable
    # product - what you would zip and hand to someone - and test exes are
    # not part of it.
    set_target_properties(xcom_tests xcom_crier_tests xcom_ui_tests xcom_entity_tests xcom_services_tests xcom_steam_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/tests")

    enable_testing()
    add_test(NAME tile  COMMAND xcom_tests)
    add_test(NAME crier COMMAND xcom_crier_tests)
    add_test(NAME ui     COMMAND xcom_ui_tests)
    add_test(NAME entity   COMMAND xcom_entity_tests)
    add_test(NAME services COMMAND xcom_services_tests)
    add_test(NAME steam    COMMAND xcom_steam_tests)
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
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${rlimgui_SOURCE_DIR})
    target_link_libraries(imgui PUBLIC raylib)

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
        src/cromwell/camera/OrbitCamera.cpp
        src/cromwell/decal/Decal.cpp
        src/cromwell/decal/DecalBuffer.cpp
        src/cromwell/decal/DecalRenderer.cpp
        src/cromwell/decal/DecalSet.cpp
        src/cromwell/diag/CrashHandler.cpp
        src/cromwell/diag/RaylibLogBridge.cpp
        src/cromwell/geometry/BoxMesh.cpp
        src/cromwell/geometry/MeshVertexBuffer.cpp
        src/cromwell/gpu/compute/ComputeSelfTest.cpp
        src/cromwell/gpu/compute/ComputeShader.cpp
        src/cromwell/gpu/target/CustomDepthStencil.cpp
        src/cromwell/gpu/target/DepthTarget.cpp
        src/cromwell/gpu/GL.cpp
        src/cromwell/gpu/target/HdrTarget.cpp
        src/cromwell/gpu/ShaderLibrary.cpp
        src/cromwell/input/InputHandler.cpp
        src/cromwell/lighting/PbrShader.cpp
        src/cromwell/lighting/ProbeSpheres.cpp
        src/cromwell/lighting/ReflectionProbeSet.cpp
        src/cromwell/lighting/ShadowMap.cpp
        src/cromwell/lighting/SunLight.cpp
        src/cromwell/material/MaterialLibrary.cpp
        src/cromwell/model/ModelAsset.cpp
        src/cromwell/model/ModelPreview.cpp
        src/cromwell/overlay/OverlayShader.cpp
        src/cromwell/overlay/TexturePreviews.cpp
        src/cromwell/post/AmbientOcclusion.cpp
        src/cromwell/post/PrepassShader.cpp
        src/cromwell/post/SkyPass.cpp
        src/cromwell/post/ToneMapPass.cpp
        src/cromwell/ribbon/RibbonRenderer.cpp
        src/cromwell/ribbon/RibbonShader.cpp
        src/cromwell/ribbon/StripSet.cpp
        # UNCONDITIONAL, even with no SDK on the machine. The whole of the
        # Steamworks dependency is behind #if XC_HAVE_STEAM inside this one
        # file, so a build without the SDK compiles the same source into stubs
        # and no #if reaches main.cpp or Application. See SteamClient.hpp.
        src/cromwell/steam/SteamClient.cpp
    )
    target_link_libraries(cromwell PUBLIC cromwell_base raylib)

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
        src/game/render/FrameRenderer.cpp
        src/game/cli/CliOptions.cpp
        src/game/path/MoveAnimator.cpp
        src/game/path/PathPreviewBuilder.cpp
        src/game/picking/SurfacePicker.cpp
        src/game/picking/TilePicker.cpp
        src/game/picking/UnitPicker.cpp
        src/game/render/overlay/BlastFlashes.cpp
        src/game/render/dev/DecalDemo.cpp
        src/game/render/dev/DevView.cpp
        src/game/render/ribbon/GlowPass.cpp
        src/game/render/overlay/Hud.cpp
        src/game/render/overlay/OverlayRenderer.cpp
        src/game/render/scene/ProbePlacement.cpp
        src/game/render/scene/PropSet.cpp
        src/game/render/ribbon/RibbonMeshSet.cpp
        src/game/render/scene/StaticsMesh.cpp
        src/game/render/scene/StoreyGeometryEmitter.cpp
        src/game/render/ribbon/StripMeshBuilder.cpp
        src/game/render/scene/UnitRenderer.cpp
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
