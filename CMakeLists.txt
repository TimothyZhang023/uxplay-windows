cmake_minimum_required(VERSION 3.16)
project(AirPlayGUI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 COMPONENTS Core Widgets REQUIRED)
find_package(Threads REQUIRED)

# Keep the Windows-specific renderer changes reviewable in this repository
# while libuxplay remains pinned to its upstream commit. Apply the patch once
# at configure time; a reverse check makes repeated CMake runs idempotent.
if(WIN32)
    find_package(Git REQUIRED)
    set(LIBUXPLAY_WINDOWS_PATCH
        "${CMAKE_SOURCE_DIR}/libuxplay-patches/windows-native-fullscreen-input.patch")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${LIBUXPLAY_WINDOWS_PATCH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/libuxplay"
        RESULT_VARIABLE LIBUXPLAY_PATCH_CAN_APPLY
        ERROR_VARIABLE LIBUXPLAY_PATCH_CHECK_ERROR
    )
    if(LIBUXPLAY_PATCH_CAN_APPLY EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${LIBUXPLAY_WINDOWS_PATCH}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/libuxplay"
            RESULT_VARIABLE LIBUXPLAY_PATCH_RESULT
            ERROR_VARIABLE LIBUXPLAY_PATCH_ERROR
        )
        if(NOT LIBUXPLAY_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the libuxplay Windows patch: ${LIBUXPLAY_PATCH_ERROR}")
        endif()
        message(STATUS "Applied libuxplay Windows fullscreen/input patch")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check
                    "${LIBUXPLAY_WINDOWS_PATCH}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/libuxplay"
            RESULT_VARIABLE LIBUXPLAY_PATCH_ALREADY_APPLIED
            ERROR_VARIABLE LIBUXPLAY_PATCH_REVERSE_ERROR
        )
        if(NOT LIBUXPLAY_PATCH_ALREADY_APPLIED EQUAL 0)
            message(FATAL_ERROR
                "libuxplay source does not match its Windows patch. "
                "Apply/check error: ${LIBUXPLAY_PATCH_CHECK_ERROR}; "
                "reverse-check error: ${LIBUXPLAY_PATCH_REVERSE_ERROR}")
        endif()
        message(STATUS "libuxplay Windows fullscreen/input patch already applied")
    endif()
endif()

# build uxplay as a static library
add_subdirectory(libuxplay)

# Keep the upstream engine in its own process. libuxplay still contains
# process-global state and exit() paths, so isolating it prevents engine
# failures or restarts from taking down the Qt tray application.
add_executable(uxplay-engine
    src/engine_main.cpp
)
target_link_libraries(uxplay-engine PRIVATE uxplay Threads::Threads)
if(WIN32)
    target_link_libraries(uxplay-engine PRIVATE winmm)
endif()
target_include_directories(uxplay-engine PRIVATE
    ${CMAKE_SOURCE_DIR}/libuxplay
)

# the WIN32 flag here tells Windows this is a GUI app, not a Console app
add_executable(uxplay-windows WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mdns_responder.cpp
    src/single_instance.cpp
    src/resources.rc
)

target_link_libraries(uxplay-windows PRIVATE
    Qt6::Core
    Qt6::Widgets
)
