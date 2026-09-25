cmake_minimum_required(VERSION 3.20)
project(Injector)

set(CMAKE_CXX_STANDARD 17)


add_executable(injector injector.cpp)
target_compile_definitions(injector PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)


set(KDMAPPER_DIR "${CMAKE_SOURCE_DIR}/../kdmapper-master/kdmapper-master/kdmapper")

add_executable(oak_loader 
    oak_loader.cpp
    ${KDMAPPER_DIR}/intel_driver.cpp
    ${KDMAPPER_DIR}/kdmapper.cpp
    ${KDMAPPER_DIR}/utils.cpp
    ${KDMAPPER_DIR}/portable_executable.cpp
    ${KDMAPPER_DIR}/service.cpp
)

target_include_directories(oak_loader PRIVATE
    ${KDMAPPER_DIR}/include
    ${KDMAPPER_DIR}
)

target_compile_definitions(oak_loader PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    DISABLE_OUTPUT
)

target_link_libraries(oak_loader PRIVATE
    ntdll
)

# Legacy C++ launcher — optional; WPF launcher lives under oak/apps/launcher.
# add_executable(OakLauncher ../launcher/OakLauncher.cpp)
# target_compile_definitions(OakLauncher PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# target_link_libraries(OakLauncher PRIVATE shell32 user32 advapi32)
# set_target_properties(OakLauncher PROPERTIES CXX_STANDARD 17)
