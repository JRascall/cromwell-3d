# Post-build helper: put the CEF runtime beside the executables that need it.
#
# libcef.dll is loaded by name from the directory of the running exe, and the
# .pak resources are located relative to it, so the staging directory that
# xcom.exe and cromwell_web_helper.exe link into needs the full set.
#
# Best-effort per file: a locked DLL (the app is running) warns instead of
# failing the build. Invoked with -DCEF_BINARY_DIR= -DCEF_RESOURCE_DIR=
# -DSTAGE=.
#
# Kept ASCII-only: this text is printed to a console whose codepage mangles
# non-ASCII punctuation into mojibake.

set(_binaries
    chrome_elf.dll
    d3dcompiler_47.dll
    dxcompiler.dll
    dxil.dll
    libcef.dll
    libEGL.dll
    libGLESv2.dll
    v8_context_snapshot.bin
    vk_swiftshader.dll
    vk_swiftshader_icd.json
    vulkan-1.dll
)

# icudtl.dat and the .pak files are resources rather than code, but CEF wants
# them beside the exe unless resources_dir_path says otherwise, and WebRuntime
# does not bother saying otherwise.
set(_resources
    chrome_100_percent.pak
    chrome_200_percent.pak
    resources.pak
    icudtl.dat
)

set(_failed 0)

function(_stage_file src dst)
    if(NOT EXISTS "${src}")
        message(WARNING "CEF runtime file missing: ${src}")
        return()
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}" "${dst}"
        RESULT_VARIABLE _r OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT _r EQUAL 0)
        set(_failed 1 PARENT_SCOPE)
    endif()
endfunction()

# locales/ needs creating explicitly now that the files go in one at a time:
# copy_if_different will not make the destination directory for you, and the
# old copy_directory did.
file(MAKE_DIRECTORY "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}/locales")

foreach(_f IN LISTS _binaries)
    _stage_file("${CEF_BINARY_DIR}/${_f}" "${STAGE}/${_f}")
endforeach()

foreach(_f IN LISTS _resources)
    _stage_file("${CEF_RESOURCE_DIR}/${_f}" "${STAGE}/${_f}")
endforeach()

# locales/ ships 220 .pak files and this game is English-only, so it stages
# two of them.
#
# This used to copy the directory wholesale, on the reasoning that the lot
# cost ~10 MB and that was cheaper than a startup failure the day somebody
# set CefSettings::locale. That trade was priced against the wrong CEF: this
# version added _FEMININE/_MASCULINE/_NEUTER variants per language, and the
# directory measures 48 MB, not 10. Staging every language in the world into
# a build directory to insure against a setting nobody has changed is not
# worth 47 MB of copying on a clean build.
#
# WHICH TWO, and why not one: WebRuntime never sets CefSettings::locale, so
# CEF resolves to en-US and that is the file actually loaded. en-GB is here
# because it is the only other locale this project would credibly select,
# and 0.5 MB is a fair price for making that a settings change rather than a
# settings change plus a build-script change.
#
# The gender variants are 18-byte stubs. They cost nothing and CEF looks for
# them beside their base locale, so they come along.
#
# THE FAILURE MODE THIS TRADES INTO, stated plainly because it is now
# reachable: set CefSettings::locale to anything outside this list and CEF
# fails at startup on a missing .pak. The fix is to add it here. That is a
# one-line change with an error message that names the file, which is why it
# is an acceptable trade and copying 48 MB forever is not.
set(_locales en-US en-GB)

foreach(_loc IN LISTS _locales)
    foreach(_suffix "" _FEMININE _MASCULINE _NEUTER)
        _stage_file("${CEF_RESOURCE_DIR}/locales/${_loc}${_suffix}.pak"
                    "${STAGE}/locales/${_loc}${_suffix}.pak")
    endforeach()
endforeach()

if(_failed)
    message(WARNING "Some CEF runtime files could not be staged - is xcom.exe still running? Build is still fine; rerun once it is closed.")
else()
    message(STATUS "CEF runtime staged")
endif()
