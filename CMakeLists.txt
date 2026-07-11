cmake_minimum_required(VERSION 3.16)
project(AirPlayGUI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

# build uxplay as a static library
add_subdirectory(libuxplay)

# Keep the upstream engine in its own process. libuxplay still contains
# process-global state and exit() paths, so isolating it prevents engine
# failures or restarts from taking down the Qt tray application.
add_executable(uxplay-engine
    src/engine_main.cpp
)
target_link_libraries(uxplay-engine PRIVATE uxplay)
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
