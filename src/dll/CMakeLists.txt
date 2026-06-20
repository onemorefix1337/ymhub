add_library(YMHubDll SHARED dllmain.cpp)
target_compile_definitions(YMHubDll PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET YMHubDll PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_link_libraries(YMHubDll PRIVATE user32 kernel32)
set_target_properties(YMHubDll PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "YMHubDll")
