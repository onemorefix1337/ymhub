if(NOT DEFINED WV2_SDK)
    set(WV2_SDK "C:/Users/User/claude/ymplayer/webview2sdk")
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
