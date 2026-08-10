# Locates the Steamworks SDK dropped into third_party/steamworks.
#
# NOT FetchContent, unlike CEF and raylib: the SDK is license-gated behind a
# partner login, so there is no URL a build can fetch. It is copied in by hand
# once — see third_party/steamworks/README.md — and this module's whole job is
# to notice whether that has happened.
#
# Sets, on success:
#   XC_STEAM_FOUND        TRUE
#   XC_STEAM_INCLUDE_DIR  the directory ABOVE steam/, so the include reads
#                         <steam/steam_api.h> exactly as Valve's samples do
#   XC_STEAM_LIBRARY      steam_api64.lib
#   XC_STEAM_DLL          steam_api64.dll, to be staged beside the exe
#
# Win64 only. That is not a limitation worth fixing yet — the app already
# depends on a windows64 CEF distribution.

set(XC_STEAM_HOME "${CMAKE_SOURCE_DIR}/third_party/steamworks")

# Two candidate roots, because unzipping the official archive one level too
# deep is the single most common way to get this wrong, and the failure is a
# silent "SDK not found" rather than anything that names the cause.
set(XC_STEAM_ROOTS "${XC_STEAM_HOME}/sdk" "${XC_STEAM_HOME}")

find_path(XC_STEAM_INCLUDE_DIR
    NAMES steam/steam_api.h
    PATHS ${XC_STEAM_ROOTS}
    PATH_SUFFIXES public
    NO_DEFAULT_PATH)

find_library(XC_STEAM_LIBRARY
    NAMES steam_api64
    PATHS ${XC_STEAM_ROOTS}
    PATH_SUFFIXES redistributable_bin/win64
    NO_DEFAULT_PATH)

find_file(XC_STEAM_DLL
    NAMES steam_api64.dll
    PATHS ${XC_STEAM_ROOTS}
    PATH_SUFFIXES redistributable_bin/win64
    NO_DEFAULT_PATH)

if(XC_STEAM_INCLUDE_DIR AND XC_STEAM_LIBRARY AND XC_STEAM_DLL)
    set(XC_STEAM_FOUND TRUE)
else()
    set(XC_STEAM_FOUND FALSE)
endif()

mark_as_advanced(XC_STEAM_INCLUDE_DIR XC_STEAM_LIBRARY XC_STEAM_DLL)
