# The CEF web surface.
#
# THIS DIRECTORY EXISTS TO CONTAIN A BLAST RADIUS. CEF ships its own cmake,
# and that cmake is opinionated in ways this project is not: it defaults to the
# static CRT, compiles at /W4 /WX with RTTI off, and forces C++20. None of that
# should reach cromwell or the game. Everything CEF sets is therefore scoped to
# this subdirectory, and only two targets cross the boundary — cromwell_web, which
# the app links, and cromwell_web_helper, which it launches.
#
# The public header of cromwell_web includes no CEF header at all. That is what
# lets the rest of the project stay at C++17 while this module is at C++20.

include(FetchContent)

# PINNED TO 144, AND NOT AN ARBITRARY CHOICE. 151 — the current stable at the
# time of writing — crashes the whole process on YouTube: type into the search
# box, submit, and the browser process dies with an access violation reading
# 0x10, roughly twenty frames deep inside libcef.dll, reached from an ordinary
# CefDoMessageLoopWork. Google navigates fine, loading a results page directly
# is fine, and a watch page is fine; only navigating INTO results kills it.
#
# It is not ours. Ruled out by testing, in this order: GPU compositing
# (--disable-gpu --disable-gpu-compositing), video autoplay
# (--autoplay-policy=user-gesture-required), re-entrant navigation from
# OnBeforePopup, and a missing CefPermissionHandler. Every one of them still
# crashed at the same instruction. 144 does not crash at all.
#
# The suspected cause is the Alloy runtime style, which offscreen rendering is
# forced onto — CefWindowInfo::SetAsWindowless sets CEF_RUNTIME_STYLE_ALLOY —
# and which is CEF's deprecated, least-maintained path. See
# chromiumembedded/cef#3643 for the same shape of fault on YouTube.
#
# Reproduce before changing this line:
#   xcom.exe --web-selftest soak.log --web-url https://www.youtube.com --web-type music
# and delete builds/win/webcache first, because Chromium refuses to open a
# profile written by a newer version and fails at startup instead.
set(XC_CEF_VERSION "144.0.32+g5ce7d26+chromium-144.0.7559.258")
set(XC_CEF_SHA1    "75dd3287e44c7026f20f2d7d69ec3a4337b2656b")

# OUTSIDE THE BUILD DIRECTORY ON PURPOSE. The distribution is ~163 MB
# compressed and ~700 MB extracted; parking it under third_party/ means
# deleting build2 — which is routine — does not cost another download. Only a
# version bump does.
set(XC_CEF_HOME "${CMAKE_SOURCE_DIR}/third_party/cef")

FetchContent_Declare(
    cef
    URL          "https://cef-builds.spotifycdn.com/cef_binary_${XC_CEF_VERSION}_windows64_minimal.tar.bz2"
    URL_HASH     "SHA1=${XC_CEF_SHA1}"
    SOURCE_DIR   "${XC_CEF_HOME}/${XC_CEF_VERSION}"
    DOWNLOAD_DIR "${XC_CEF_HOME}/download"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Points at nothing, deliberately: it stops FetchContent from running
    # add_subdirectory on the distribution's own CMakeLists, which would build
    # the sample apps. The wrapper is added explicitly below instead.
    SOURCE_SUBDIR "no-cmake-entry-point-here"
)
FetchContent_MakeAvailable(cef)

# ---- CEF's cmake, with its defaults overridden ------------------------
#
# USE_SANDBOX off: with it on, Windows CEF switches to the bootstrap model
# where the application is a DLL loaded by bootstrap.exe. That is a different
# executable shape than this project has, and the sandbox library is /MT-only
# anyway, which the next line rules out.
#
# /MD because everything else here — raylib, imgui, cromwell — is /MD, and
# mixing CRTs across a link is a link error at best.
set(CEF_ROOT "${cef_SOURCE_DIR}" CACHE INTERNAL "CEF distribution root" FORCE)
set(USE_SANDBOX OFF CACHE BOOL "" FORCE)
set(USE_ATL     OFF CACHE BOOL "" FORCE)
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

add_subdirectory("${CEF_ROOT}/libcef_dll" libcef_dll_wrapper)

# ---- our code ---------------------------------------------------------
#
# Note what is NOT here: SET_LIBRARY_TARGET_PROPERTIES from cef_macros.cmake.
# That macro applies CEF's whole flag set, /WX included, and this code is not
# written to Chromium's warning bar. The three things actually needed from CEF
# — the include root, the stdint macros, and C++20 — are set by hand.
# Two halves that must not meet: cef/WebBrowserHost.cpp and cef/WebRuntime.cpp include
# CEF, which drags in windows.h; surface/WebSurface.cpp and surface/WebInput.cpp include
# raylib. Both declare CloseWindow and ShowCursor with C linkage and different
# signatures, so any file seeing both fails to compile. See cef/WebBrowserHost.hpp.
add_library(cromwell_web STATIC
    cef/WebRuntime.cpp        # CEF
    cef/WebBrowserHost.cpp    # CEF
    surface/WebSurface.cpp        # raylib
    surface/WebInput.cpp          # raylib
    surface/WebSelfTest.cpp       # raylib
)

target_include_directories(cromwell_web
    PUBLIC  "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${CEF_ROOT}"
)

target_compile_features(cromwell_web PRIVATE cxx_std_20)

target_compile_definitions(cromwell_web PRIVATE
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(cromwell_web
    PRIVATE libcef_dll_wrapper "${CEF_LIB_RELEASE}" raylib
)

# ---- the subprocess executable ----------------------------------------
#
# A SEPARATE BINARY, NOT A SECOND ENTRY POINT IN xcom.exe. Chromium re-launches
# its own executable once per renderer, GPU and utility process. Pointing that
# at xcom.exe would run InitWindow — and the whole renderer — inside every one
# of them. This does nothing but CefExecuteProcess.
add_executable(cromwell_web_helper WIN32 cef/WebHelperMain.cpp)

target_include_directories(cromwell_web_helper PRIVATE "${CEF_ROOT}")
target_compile_features(cromwell_web_helper PRIVATE cxx_std_20)
target_compile_definitions(cromwell_web_helper PRIVATE
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(cromwell_web_helper PRIVATE libcef_dll_wrapper "${CEF_LIB_RELEASE}")

# ---- runtime files ----------------------------------------------------
#
# libcef.dll and friends must sit beside the executable that loads them, and
# the resources must be findable from there too. ONE destination now: both
# exes link straight into XC_STAGE_DIR, so the runtime goes to the directory
# they land in and nowhere else. The helper is not copied at all - it is
# already there.
#
# The stage is named as $<TARGET_FILE_DIR:cromwell_web_helper> rather than as
# XC_STAGE_DIR so that a Debug build stages into its own output directory
# instead of over the Release one.
#
# copy_if_different, so this is a no-op on every build after the first.
add_custom_target(xcom_cef_runtime ALL
    COMMAND ${CMAKE_COMMAND}
            -DCEF_BINARY_DIR=${CEF_BINARY_DIR_RELEASE}
            -DCEF_RESOURCE_DIR=${CEF_RESOURCE_DIR}
            -DSTAGE=$<TARGET_FILE_DIR:cromwell_web_helper>
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_cef_runtime.cmake"
    DEPENDS cromwell_web_helper
    COMMENT "Staging CEF runtime files"
    VERBATIM
)
