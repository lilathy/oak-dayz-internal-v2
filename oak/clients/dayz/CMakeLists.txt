cmake_minimum_required(VERSION 3.20)
project(DayZInternal)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(OAK_PROTECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/protect)

# Optional: bake production runtime SPKI SHA-256 pin into Release builds.
# cmake -DOAK_RUNTIME_SPKI_PIN=<hex> ...
set(OAK_RUNTIME_SPKI_PIN "" CACHE STRING "SHA-256 hex of runtime package SPKI DER")

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/protection_client.cpp
    src/runtime_offsets.cpp
    src/oak_crash_hunt.cpp
    src/oak_lab.cpp
    src/oak_engine.cpp
    ${OAK_PROTECT_DIR}/src/oak_watermark.cpp
    src/imgui_menu.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OAK_PROTECT_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    kernel32 user32 gdi32 shell32 d3d11 dxgi winhttp bcrypt crypt32 ws2_32 winmm windowscodecs
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    OAK_PRODUCT_SLUG=\"dayz\"
    # $<$<CONFIG:Release>:OAK_REQUIRE_PROTECTION>  # disabled for local BE testing
    $<$<BOOL:${OAK_RUNTIME_SPKI_PIN}>:OAK_RUNTIME_SPKI_PIN=\"${OAK_RUNTIME_SPKI_PIN}\">
)

# Manual-map has no CRT startup — /GS cookies are uninitialized and fail-fast.
target_compile_options(${PROJECT_NAME} PRIVATE /GS-)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "dayz_internal"
    SUFFIX ".dll"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/pdb_private"
)

add_executable(OakImGuiTest
    src/imgui_panel_test.cpp
    src/imgui_menu.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(OakImGuiTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(OakImGuiTest PRIVATE
    kernel32 user32 gdi32 shell32 d3d11 dxgi
)

target_compile_definitions(OakImGuiTest PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set_target_properties(OakImGuiTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    WIN32_EXECUTABLE TRUE
)

add_library(OakImGuiOverlay SHARED
    src/imgui_overlay_dll.cpp
    src/imgui_menu.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(OakImGuiOverlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(OakImGuiOverlay PRIVATE
    kernel32 user32 gdi32 shell32 d3d11 dxgi
)

target_compile_definitions(OakImGuiOverlay PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set_target_properties(OakImGuiOverlay PROPERTIES
    OUTPUT_NAME "OakImGuiOverlay"
    SUFFIX ".dll"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_executable(OakImGuiOverlayLoader
    src/imgui_overlay_loader.cpp
)

target_link_libraries(OakImGuiOverlayLoader PRIVATE kernel32 user32)
set_target_properties(OakImGuiOverlayLoader PROPERTIES CXX_STANDARD 17)

add_executable(OakOffsetDumper
    src/offset_dumper.cpp
)
target_link_libraries(OakOffsetDumper PRIVATE kernel32 user32 psapi)
set_target_properties(OakOffsetDumper PROPERTIES CXX_STANDARD 17)

set(OAK_FONT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts)
set(OAK_FONT_FILES
    ${OAK_FONT_DIR}/DMSans-Regular.ttf
    ${OAK_FONT_DIR}/DMSans-Medium.ttf
    ${OAK_FONT_DIR}/DMSans-SemiBold.ttf
)

function(oak_copy_fonts target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OAK_FONT_FILES} "$<TARGET_FILE_DIR:${target_name}>/fonts"
        COMMENT "Copying DM Sans fonts for ${target_name}"
    )
endfunction()

oak_copy_fonts(${PROJECT_NAME})
oak_copy_fonts(OakImGuiTest)
oak_copy_fonts(OakImGuiOverlay)

find_program(OAK_NODE_EXECUTABLE NAMES node node.exe)
if(OAK_NODE_EXECUTABLE)
    # Security gate temporarily disabled for local BE testing
    # add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    #     COMMAND "${OAK_NODE_EXECUTABLE}"
    #             "${OAK_PROTECT_DIR}/tools/ship_security_gate.mjs"
    #             --dll "$<TARGET_FILE:${PROJECT_NAME}>"
    #             --mutate
    #     COMMENT "OakProtect: ship security gate + watermark mutate (always-on)"
    #     VERBATIM
    # )
else()
    message(WARNING "Node.js not found — watermark mutation skipped")
endif()

message(STATUS "Building dayz_internal (clients/dayz) + OakImGuiTest + OakImGuiOverlay")
