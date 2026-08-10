# Post-build helper: put the CEF runtime beside the executables that need it.
#
# libcef.dll is loaded by name from the directory of the running exe, and the
# .pak resources are located relative to it, so the staging directory that
# xcom.exe and xcom_web_helper.exe link into needs the full set.
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

file(MAKE_DIRECTORY "${STAGE}")

foreach(_f IN LISTS _binaries)
    _stage_file("${CEF_BINARY_DIR}/${_f}" "${STAGE}/${_f}")
endforeach()

foreach(_f IN LISTS _resources)
    _stage_file("${CEF_RESOURCE_DIR}/${_f}" "${STAGE}/${_f}")
endforeach()

# locales/ is a directory of ~60 .pak files. Only en-US is needed unless
# CefSettings::locale changes, but copying the lot costs ~10 MB and avoids
# a startup failure the day someone does change it.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_RESOURCE_DIR}/locales" "${STAGE}/locales"
    RESULT_VARIABLE _r OUTPUT_QUIET ERROR_QUIET
)
if(NOT _r EQUAL 0)
    set(_failed 1)
endif()

if(_failed)
    message(WARNING "Some CEF runtime files could not be staged - is xcom.exe still running? Build is still fine; rerun once it is closed.")
else()
    message(STATUS "CEF runtime staged")
endif()
