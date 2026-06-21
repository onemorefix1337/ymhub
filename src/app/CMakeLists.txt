if(NOT DEFINED WV2_SDK)
    message(FATAL_ERROR
        "WV2_SDK is not set. Point it at an extracted Microsoft.Web.WebView2 "
        "NuGet package, e.g.:\n"
        "  nuget install Microsoft.Web.WebView2 -Version 1.0.2592.51 -OutputDirectory webview2sdk\n"
        "  cmake -S . -B build -A x64 -DWV2_SDK=\"<path>\\webview2sdk\\Microsoft.Web.WebView2.1.0.2592.51\"")
endif()

set(RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/dll_embed.rc")
add_custom_command(
    OUTPUT "${RC_FILE}"
    COMMAND ${CMAKE_COMMAND}
        "-DDLL_PATH=$<TARGET_FILE:YMHubDll>"
        "-DOUT=${RC_FILE}"
        -P "${CMAKE_SOURCE_DIR}/cmake/gen_rc.cmake"
    DEPENDS YMHubDll
    VERBATIM
)
set_source_files_properties("${RC_FILE}" PROPERTIES GENERATED TRUE)

add_executable(YMHub WIN32 main.cpp icon.rc "${RC_FILE}")
target_compile_definitions(YMHub PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(YMHub PRIVATE /std:c++20 /D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
set_property(TARGET YMHub PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_include_directories(YMHub PRIVATE "${WV2_SDK}/build/native/include")
target_link_libraries(YMHub PRIVATE
    "${WV2_SDK}/build/native/x64/WebView2LoaderStatic.lib"
    shell32 user32 gdi32 dwmapi runtimeobject ole32 oleaut32 shlwapi
    windowscodecs shcore version)
set_target_properties(YMHub PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "YMHub")
