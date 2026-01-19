cmake_minimum_required(VERSION 3.10)
project(TerminalDashboard)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.cpp
    src/TuiEngine.cpp
    src/Window.cpp
    src/TaskWindow.cpp
    src/StatusWindow.cpp
)

add_executable(terminal_dashboard ${SOURCES})

# Create dummy files for now so cmake doesn't complain before we write them
file(TOUCH ${CMAKE_CURRENT_SOURCE_DIR}/src/TuiEngine.cpp)
file(TOUCH ${CMAKE_CURRENT_SOURCE_DIR}/src/Window.cpp)
file(TOUCH ${CMAKE_CURRENT_SOURCE_DIR}/src/TaskWindow.cpp)
file(TOUCH ${CMAKE_CURRENT_SOURCE_DIR}/src/StatusWindow.cpp)
