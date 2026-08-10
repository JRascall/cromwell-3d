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

# ---- where the runnable product lands ---------------------------------
# ONE staging directory, not the project root. Everything the app needs at
# runtime — xcom.exe, xcom_web_helper.exe and the whole CEF runtime — is
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

# ---- the simulation core: pure C++, no raylib -------------------------
set(CORE_SOURCES
    src/core/diag/Logger.cpp
    src/core/events/Crier.cpp
    src/core/lattice/Lattice.cpp
    src/core/world/World.cpp
    src/core/world/MapAuthor.cpp
    src/core/world/RampValidator.cpp
    src/core/world/RampSupport.cpp
    src/core/world/DemoMapFactory.cpp
    src/core/query/Terrain.cpp
    src/core/query/Standability.cpp
    src/core/query/BlockedMass.cpp
    src/core/query/LedgeCover.cpp
    src/core/query/LadderQuery.cpp
    src/core/query/CoverModel.cpp
    src/core/movement/ColumnScanner.cpp
    src/core/movement/InfantryMoveGraph.cpp
    src/core/movement/VehicleMoveGraph.cpp
    src/core/movement/ReachField.cpp
    src/core/movement/Pathfinder.cpp
    src/core/movement/PathReconstructor.cpp
    src/core/units/Unit.cpp
    src/core/units/Soldier.cpp
    src/core/units/Vehicle.cpp
    src/core/units/UnitRoster.cpp
    src/core/units/DemoRosterFactory.cpp
    src/core/units/OccupancyMaskBuilder.cpp
    src/core/light/SunBaker.cpp
    src/core/light/SunBakeBenchmark.cpp
    src/core/light/RoomPartition.cpp
    src/core/los/RayCaster.cpp
    src/core/los/PeekFinder.cpp
    src/core/los/EyeSet.cpp
    src/core/los/VisibilityField.cpp
    src/core/los/VisibilityComputer.cpp
    src/core/border/Band.cpp
    src/core/border/BandBuilder.cpp
    src/core/border/BandConnectivity.cpp
    src/core/border/BandExtractor.cpp
    src/core/border/LoopSet.cpp
    src/core/border/EdgeCapHeight.cpp
    src/core/border/LoopPolyliner.cpp
)

add_library(xcom_core STATIC ${CORE_SOURCES})
target_include_directories(xcom_core PUBLIC src)
if(NOT MSVC)
    target_link_libraries(xcom_core PUBLIC m)
endif()

# ---- headless test target (no raylib dependency) -----------------------
if(XC_BUILD_TESTS)
    add_executable(xcom_tests tests/TileCoreTests.cpp)
    target_link_libraries(xcom_tests PRIVATE xcom_core)

    # The event bus gets its own binary rather than more cases in the tile
    # tests: it shares no fixture with them, and a bus bug should name itself.
    add_executable(xcom_crier_tests tests/CrierTests.cpp)
    target_link_libraries(xcom_crier_tests PRIVATE xcom_core)

    # The test binaries stay in the build tree. builds/win is the runnable
    # product - what you would zip and hand to someone - and test exes are
    # not part of it.
    set_target_properties(xcom_tests xcom_crier_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/tests")

    enable_testing()
    add_test(NAME tile  COMMAND xcom_tests)
    add_test(NAME crier COMMAND xcom_crier_tests)
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

    add_executable(xcom
        src/main.cpp
        src/app/Application.cpp
        src/app/cli/CliOptions.cpp
        src/app/diag/CrashHandler.cpp
        src/app/diag/RaylibLogBridge.cpp
        src/app/rules/DestructionSystem.cpp
        src/app/state/GameState.cpp
        src/app/rules/HullCrusher.cpp
        src/app/input/InputHandler.cpp
        src/app/path/MoveAnimator.cpp
        src/app/camera/OrbitCamera.cpp
        src/app/path/PathPreviewBuilder.cpp
        src/app/rules/RestPlacement.cpp
        src/app/state/RingSelector.cpp
        src/app/picking/TilePicker.cpp
        src/app/picking/UnitPicker.cpp
        src/render/overlay/BlastFlashes.cpp
        src/render/overlay/DevView.cpp
        src/app/picking/SurfacePicker.cpp
        src/render/decal/Decal.cpp
        src/render/decal/DecalBuffer.cpp
        src/render/decal/DecalDemo.cpp
        src/render/decal/DecalRenderer.cpp
        src/render/decal/DecalSet.cpp
        src/render/gpu/ComputeSelfTest.cpp
        src/render/gpu/ComputeShader.cpp
        src/render/gpu/DepthTarget.cpp
        src/render/gpu/GL.cpp
        src/render/gpu/HdrTarget.cpp
        src/render/overlay/Hud.cpp
        src/render/geometry/BoxMesh.cpp
        src/render/geometry/MeshVertexBuffer.cpp
        src/render/lighting/ProbeSpheres.cpp
        src/render/lighting/ReflectionProbeSet.cpp
        src/render/lighting/PbrShader.cpp
        src/render/lighting/ShadowMap.cpp
        src/render/lighting/SunLight.cpp
        src/render/material/MaterialLibrary.cpp
        src/render/model/ModelAsset.cpp
        src/render/model/ModelPreview.cpp
        src/render/model/PropSet.cpp
        src/render/overlay/OverlayRenderer.cpp
        src/render/overlay/OverlayShader.cpp
        src/render/gpu/CustomDepthStencil.cpp
        src/render/overlay/TexturePreviews.cpp
        src/render/post/AmbientOcclusion.cpp
        src/render/post/PrepassShader.cpp
        src/render/post/SkyPass.cpp
        src/render/post/ToneMapPass.cpp
        src/render/gpu/ShaderLibrary.cpp
        src/render/world/StaticsMesh.cpp
        src/render/world/StoreyGeometryEmitter.cpp
        src/render/units/UnitRenderer.cpp
        src/render/ribbon/GlowPass.cpp
        src/render/ribbon/RibbonMeshSet.cpp
        src/render/ribbon/RibbonRenderer.cpp
        src/render/ribbon/RibbonShader.cpp
        src/render/ribbon/StripMeshBuilder.cpp
    )
    target_link_libraries(xcom PRIVATE xcom_core raylib imgui)

    # ---- the web surface ------------------------------------------------
    # In its own subdirectory because CEF's cmake rewrites compiler and linker
    # flags wholesale, and a subdirectory is the cheapest way to keep that
    # inside its own scope rather than over the whole project.
    if(XC_BUILD_WEB)
        add_subdirectory(src/render/web)
        target_link_libraries(xcom PRIVATE xcom_web)
        target_compile_definitions(xcom PRIVATE XC_HAVE_WEB=1)
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
    #   src/render/gpu/GL.hpp is the ONLY file that includes glad.h.
    #   Everything rlgl wraps goes through rlgl. Everything it does not gets
    #   declared in GL.hpp with a comment naming the pass that needs it.
    #
    # One owned door, because the second and third exception would each have
    # been argued exactly as well as the first, and nobody would be counting.
    target_include_directories(xcom PRIVATE ${raylib_SOURCE_DIR}/src/external)

    if(WIN32 AND MSVC)
        set_target_properties(xcom PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
        target_link_options(xcom PRIVATE $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)

        # dbghelp is the crash handler's stack walker and minidump writer.
        target_link_libraries(xcom PRIVATE dbghelp)

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
        target_compile_options(xcom       PRIVATE $<$<CONFIG:Release>:/Z7>)
        target_compile_options(xcom_core  PRIVATE $<$<CONFIG:Release>:/Z7>)
        target_link_options(xcom PRIVATE
            $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()
    # No POST_BUILD copy into the project root any more: the linker already
    # writes xcom.exe into XC_STAGE_DIR, so there is exactly one copy of it
    # and no way for the root one to go stale behind a locked file.
endif()
